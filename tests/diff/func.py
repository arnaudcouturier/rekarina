"""Differential test of whole emitted functions: recomp/gen.py's C for a guest function against Unicorn running the
same function from its entry until it returns.

Picks functions from the ExeFS modules: no branches out of the function, no SVC, BRK, system register, exclusive or
atomic instructions; with conditional branches, and loops, jump tables, SIMD and calls where the module has them.
Emits them with the generator's own Emitter, compiles them with tests/diff/harness.c, and runs each from random
states: x0-x7 point into the scratch buffer (or hold small integers), the buffer holds pointers into itself and random
data, SP sits near the buffer's top and LR is a sentinel address. Unicorn (Cortex-A57) stops when PC reaches the
sentinel. A case is skipped when Unicorn faults, writes outside the scratch buffer (module images are mapped
read-only), leaves the function, or runs longer than --steps instructions. Compares X0-X30, SP, V0-V31 and the
scratch buffer; NZCV is local to a generated function (recomp/flags.py).

Every BL and BLR goes to one stub on both sides (rt_stub in harness.c, Runner.stub here) instead of its callee. The
stub folds the guest state at the call into a digest and sets the caller-saved registers from it, so the test sees
whether the emitted code had every register in the Cpu at the call and reads them back after it. Callers of another
picked function are dropped, as the C side would run that function instead of the stub.

With --patches, the spin waits of the table (recomp/patches.py `spin`) are emitted with their gates, and functions
that hold one come first (a quarter of the picks at most); a case whose loop does not leave runs into --steps.

--at ADDR[,ADDR...] tests those functions (absolute addresses of function starts) in place of random picks, and in
half the cases gives v0's low lane a float between 0.05 and 2 (a step or a scale, as such functions take). With
--patches the code patch sites inside them are emitted as the generator emits them; --patch-on SET[,SET...] switches
those sets on for both sides: the C reads rt_patch_on, and Unicorn runs a copy of the module whose site words branch
to the replacement words placed after its image and back (replacement words that branch or name the patch page are
not supported here).

Usage: func.py [--exefs DIR] [--modules main,subsdk0,sdk] [--functions N] [--cases K] [--seed S] [--jobs J]
               [--patches FILE] [--at ADDR,...] [--patch-on SET,...]
"""
import argparse
import os
import struct
import platform
import random
import subprocess
import sys
import time
import zlib
from collections import Counter
from pathlib import Path

import numpy as np
import unicorn as uc
from unicorn import arm64_const as A

from common import BUF, BUF_SIZE, MSYS_BIN, ROOT, Emu, compiler, diff_state, rand_fpcr, rand_v, rand_x, run_harness
import gen
import patches
from layout import layout
from module import Module

PATCH_ON = [0] * 64   # rt_patch_on of the harness, by set index (--patch-on)
SENTINEL = 0x7210000000  # LR on entry; never executed
PTR_LO, PTR_HI = BUF + 0x8000, BUF + 0x28000
STACK_TOP = BUF + 0x3F000
MIN_INSNS, MAX_INSNS = 6, 600


FNV = 0x100000001B3
M64 = (1 << 64) - 1


def splitmix(z):
    z = (z + 0x9E3779B97F4A7C15) & M64
    z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & M64
    z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & M64
    return z ^ (z >> 31)


def call_mask(w):
    return ((w & 0xFC000000) == 0x94000000) | ((w & 0xFFFFFC1F) == 0xD63F0000)   # BL, BLR


def forbidden_mask(w):
    """Words a picked function must not contain."""
    m = (w & 0xFF000000) == 0xD4000000                    # SVC, HVC, SMC, BRK, HLT, DCPS
    sysi = (w & 0xFFC00000) == 0xD5000000                 # system: MRS, MSR, SYS (DC ZVA), hints, barriers
    hint_or_barrier = ((w & 0xFFFFF000) == 0xD5032000) | ((w & 0xFFFFF000) == 0xD5033000)
    m |= sysi & ~hint_or_barrier
    m |= (w & 0x3F000000) == 0x08000000                   # exclusives, load-acquire / store-release, CAS
    m |= (w & 0x3F200C00) == 0x38200000                   # LSE atomics
    return m


def cond_mask(w):
    return (((w & 0xFE000010) == 0x54000000) | ((w & 0x7E000000) == 0x34000000) |
            ((w & 0x7E000000) == 0x36000000))


def simd_mask(w):
    return ((w & 0x0E000000) == 0x0E000000) | (((w & 0x0A000000) == 0x08000000) & (((w >> 26) & 1) == 1))


STUB_SKIP_TEMPS = False   # --temps-unsaved (main sets it)


def pick_functions(name, mod, count, r, spins=None, kept_uses=0, reg_locals=gen.regs.LOCALS, temps_unsaved=False,
                   at=None, patched=None):
    """[(entry, end, tags)] of up to count functions of mod, plus the Emitter that will emit them. spins: {spin wait
    back-edge: head}, emitted with gates. at: these function starts in place of random picks; patched: the module's
    code patch sites (recomp/patches.py Table.for_module), emitted at theirs."""
    starts, _ = gen.discover(mod)
    w = mod.words
    ends = np.append(starts[1:], len(w))

    def per_fn(mask):
        cs = np.concatenate([[0], np.cumsum(mask.astype(np.int64))])
        return cs[ends] - cs[starts]

    size = ends - starts
    ok = (per_fn(forbidden_mask(w)) == 0) & (per_fn(cond_mask(w)) > 0) & (size >= MIN_INSNS) & (size <= MAX_INSNS)
    has_br = per_fn((w & 0xFFFFFC1F) == 0xD61F0000) > 0
    has_call = per_fn(call_mask(w)) > 0
    cand = np.flatnonzero(ok)
    with_br = [int(i) for i in cand if has_br[i]]
    with_call = [int(i) for i in cand if has_call[i] and not has_br[i]]
    rest = [int(i) for i in cand if not has_br[i] and not has_call[i]]
    spins = spins or {}
    gated = np.searchsorted(mod.lo + starts * 4, np.array(sorted(spins), dtype=np.int64), side='right') - 1
    with_spin = sorted({int(i) for i in gated} & set(cand.tolist()))
    for pool in (with_spin, with_br, with_call, rest):
        r.shuffle(pool)
    emitter = gen.Emitter(mod, set((mod.lo + starts * 4).tolist()), spins=spins, kept_uses=kept_uses,
                          reg_locals=reg_locals, temps_unsaved=temps_unsaved, patched=patched)
    picks, tags_seen = [], Counter()
    if at is not None:   # the functions named, whatever the pools say
        pools = ()
        for entry in at:
            i = int(np.searchsorted(mod.lo + starts * 4, entry))
            if i >= len(starts) or mod.lo + int(starts[i]) * 4 != entry:
                print(f'{name}: {entry:#x} is no function start')
                continue
            end = mod.lo + int(ends[i]) * 4
            tags = check_function(emitter, entry, end)
            if tags is None:
                print(f'{name}: {gen.fname(entry)} cannot run alone (a branch out of it, an unresolved br, no ret)')
                continue
            if any(a in (patched or {}) for a in range(entry, end, 4)):
                tags.add('patched')
            picks.append((entry, end, tags))
    else:
        pools = ((with_spin, count // 4), (with_br, count // 4), (with_call, count // 2), (rest, count))
    # Functions with a spin wait first, then with a br (jump tables, fallbacks), at most a quarter of the picks each;
    # then functions with calls, at most half.
    for pool, quota in pools:
        taken = 0
        for i in pool:
            if len(picks) >= count or taken >= quota:
                break
            entry, end = mod.lo + int(starts[i]) * 4, mod.lo + int(ends[i]) * 4
            if any(e == entry for e, _, _ in picks):
                continue
            tags = check_function(emitter, entry, end)
            if tags is None:
                continue
            if any(spins.get(x) is not None for x in range(entry, end, 4)):
                tags.add('spin_gate')
            picks.append((entry, end, tags))
            taken += 1
    picked = {e for e, _, _ in picks}
    picks = [p for p in picks if not callees(emitter, p[0], p[1]) & picked]
    for _, _, tags in picks:
        tags_seen.update(tags)
    print(f'{name}: {len(starts)} functions, {len(cand)} candidates, picked {len(picks)}: '
          f'{dict(tags_seen)}', flush=True)
    return picks, emitter


def call_sites(mod, entry, end):
    """Addresses of the BL and BLR instructions of [entry, end)."""
    return [a for a in range(entry, end, 4) if mod.lift(a).kind in ('bl', 'blr')]


def callees(emitter, entry, end):
    """Function starts the emitted C of [entry, end) calls directly (`f_<addr>(c)`, not through rt_dispatch)."""
    mod = emitter.mod
    return {mod.lift(a).target for a in call_sites(mod, entry, end)
            if mod.lift(a).kind == 'bl' and mod.lift(a).target in emitter.starts}


def check_function(emitter, entry, end):
    """Tags of a function the test can run in isolation, or None."""
    mod = emitter.mod
    insns = [mod.lift(a) for a in range(entry, end, 4)]
    if insns[-1].kind not in gen.TERMINATORS or not any(i.kind == 'ret' for i in insns):
        return None
    tags = set()
    for k, ins in enumerate(insns):
        a = entry + 4 * k
        if ins.kind in ('bl', 'blr'):
            tags.add('calls')
        if ins.kind in ('b', 'bcond'):
            if not entry <= ins.target < end:
                return None
            if ins.target <= a:
                tags.add('loop')
        if ins.kind == 'br':
            jt = gen.jumptables.resolve(mod, a, entry, end)
            if jt:
                tags.add('jump_table')
            elif gen.jumptables.is_computed(mod, a, entry):
                tags.add('all_label_fallback')
            else:
                return None
    if simd_mask(mod.words[(entry - mod.lo) >> 2:(end - mod.lo) >> 2]).any():
        tags.add('simd')
    return tags


def build(work, emitted, jobs, cflags):
    """Compile the emitted functions with harness.c into work/func[.exe]; tests[k] is emitted[k]."""
    cc = compiler()
    env = dict(os.environ)
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    flags = ['-O1', '-fno-strict-aliasing', '-Wno-return-type', '-Wno-unused-label', '-I', str(ROOT / 'runtime')]
    flags += cflags.split()
    groups = {}
    for name, entry, end, emitter in emitted:
        groups.setdefault(name, []).append((entry, end))
    srcs, n = [], max(1, jobs)
    for name, fns in groups.items():
        emitter = next(e for m, _, _, e in emitted if m == name)
        per = (len(fns) + n - 1) // n
        for j in range(0, len(fns), per):
            path = work / f'{name}_{j // per:03d}.c'
            path.write_text(gen.chunk_text(name, j // per, emitter, fns[j:j + per]))
            srcs.append(path)
    table = work / 'table.c'
    called = set()
    for _, entry, end, emitter in emitted:
        called |= callees(emitter, entry, end)
    table.write_text('#include "cpu.h"\n' + ''.join(f'void {gen.fname(e)}(Cpu *c);\n' for _, e, _, _ in emitted) +
                     'GuestFn tests[] = {' + ','.join(gen.fname(e) for _, e, _, _ in emitted) + '};\n' +
                     f'int ntests = {len(emitted)};\n' + 'void rt_stub_call(Cpu *c, uint64_t target);\n' +
                     # the code patch sets as --patch-on switches them (read by the patched sites' C)
                     'RT_LOCAL uint8_t rt_patch_on[64] = {' + ','.join(str(v) for v in PATCH_ON) + '};\n' +
                     ''.join(f'void {gen.fname(t)}(Cpu *c) {{ rt_stub_call(c, {hex(t)}ull); }}\n'
                             for t in sorted(called)))
    # fma.c: a --cflags=-march=x86-64-v2 run must exercise the software fma the v2 build links, not the C runtime's.
    srcs += [table, ROOT / 'tests' / 'diff' / 'harness.c', ROOT / 'runtime' / 'fma.c']
    objs, procs = [], []
    for s in srcs:
        o = work / (s.stem + '.o')
        objs.append(o)
        procs.append(subprocess.Popen([cc] + flags + ['-c', str(s), '-o', str(o)], env=env))
    if any(p.wait() for p in procs):
        raise SystemExit('compile failed')
    exe = work / ('func.exe' if os.name == 'nt' else 'func')
    subprocess.check_call([cc, '-fuse-ld=lld', '-o', str(exe)] + [str(o) for o in objs], env=env)
    return exe


def make_buffer(r, pointers=0.4):
    """The scratch buffer: pointers into itself (that share of the words), small numbers and random words."""
    out = bytearray()
    for _ in range(BUF_SIZE // 8):
        k = r.random()
        if k < pointers:
            v = r.randrange(PTR_LO, PTR_HI, 8)
        elif k < pointers + (1 - pointers) / 2:
            v = r.randrange(0, 256)
        else:
            v = r.getrandbits(64)
        out += v.to_bytes(8, 'little')
    return bytes(out)


def make_case(r):
    x = [rand_x(r) for _ in range(31)]
    for i in range(8):
        k = r.random()
        if k < 0.7:
            x[i] = r.randrange(PTR_LO, PTR_HI, 16)
        elif k < 0.9:
            x[i] = r.randrange(0, 64)
    x[30] = SENTINEL
    sp = STACK_TOP - r.randrange(0, 0x100, 16)
    return rand_fpcr(r), x, sp, r.getrandbits(4) << 28, [rand_v(r) for _ in range(32)]


class Runner:
    """Unicorn with the module images read-only, a block hook that stops when execution leaves the function, and a
    code hook on every call site of the picked functions that stops before the call, for the stub."""

    MAX_CALLS = 1000

    def __init__(self, images, buf0, sites, extra=()):
        self.extra = list(extra)   # [(lo, hi)]: code outside the function that counts as its own (patch trampolines)
        self.emu = Emu(images)
        mu = self.emu.mu
        for base, data in images:
            mu.mem_protect(base, (len(data) + 0xFFFF) & ~0xFFFF, uc.UC_PROT_READ | uc.UC_PROT_EXEC)
        mu.mem_map(SENTINEL, 0x1000)
        mu.mem_write(BUF, buf0)
        self.buf0 = buf0
        self.lo = self.hi = self.h = 0
        self.left = False
        self.at_call = None
        mu.hook_add(uc.UC_HOOK_BLOCK, self._block)
        # One hook per call site, all added before anything is translated, so no cached block lacks one.
        for a in sorted(sites):
            mu.hook_add(uc.UC_HOOK_CODE, self._call, begin=a, end=a)

    def _block(self, mu, address, size, user):
        if not self.lo <= address < self.hi and address != SENTINEL and \
                not any(lo <= address < hi for lo, hi in self.extra):
            self.left = True
            mu.emu_stop()

    def _call(self, mu, address, size, user):
        if self.lo <= address < self.hi:
            self.at_call = address
            mu.emu_stop()

    def stub(self, a):
        """harness.c rt_stub for the call at a, which Unicorn has not executed yet. Returns the address to resume at."""
        e, mu = self.emu, self.emu.mu
        w = int.from_bytes(mu.mem_read(a, 4), 'little')
        if (w & 0xFC000000) == 0x94000000:   # BL
            imm = w & 0x03FFFFFF
            target = (a + ((imm - (1 << 26) if imm >> 25 else imm) << 2)) & M64
        else:                                 # BLR
            target = mu.reg_read(e.xregs[(w >> 5) & 31])
        mu.reg_write(A.UC_ARM64_REG_X30, a + 4)
        h = ((self.h ^ target) * FNV) & M64
        for i, val in enumerate(list(mu.reg_read_batch(e.xregs)) + [mu.reg_read(A.UC_ARM64_REG_SP)]):
            if STUB_SKIP_TEMPS and 9 <= i <= 15:
                continue
            h = ((h ^ val) * FNV) & M64
        for val in mu.reg_read_batch(e.qregs[:8]):
            h = ((h ^ (val & M64)) * FNV) & M64
            h = ((h ^ (val >> 64)) * FNV) & M64
        self.h = h
        regs = []
        for i in range(19):
            s = splitmix((h + i) & M64)
            regs.append((e.xregs[i], PTR_LO + ((s >> 4) % ((PTR_HI - PTR_LO) // 16)) * 16 if i < 8 and s & 3 else s))
        regs.append((A.UC_ARM64_REG_NZCV, splitmix((h + 19) & M64) >> 60 << 28))
        for i in range(8):
            regs.append((e.qregs[i], splitmix((h + 33 + 2 * i) & M64) << 64 | splitmix((h + 32 + 2 * i) & M64)))
        mu.reg_write_batch(regs)
        top = BUF + BUF_SIZE - 16
        n = int.from_bytes(mu.mem_read(top, 8), 'little')
        mu.mem_write(top, ((n + 1) & M64).to_bytes(8, 'little') + h.to_bytes(8, 'little'))
        return a + 4

    def run(self, entry, end, case, steps):
        """(x, sp, nzcv, pc, v, crc of the buffer) or None if the case is skipped."""
        fpcr, x, sp, nzcv, v = case
        e, mu = self.emu, self.emu.mu
        self.lo, self.hi, self.left, self.h = entry, end, False, 0
        mu.reg_write_batch(list(zip(e.xregs, x[:31])) + list(zip(e.qregs, v)) +
                           [(A.UC_ARM64_REG_SP, sp), (A.UC_ARM64_REG_NZCV, nzcv), (A.UC_ARM64_REG_FPCR, fpcr)])
        pc, ok = entry, False
        try:
            for _ in range(self.MAX_CALLS):
                self.at_call = None
                mu.emu_start(pc, SENTINEL, count=steps)
                if self.at_call is None:
                    ok = not self.left and mu.reg_read(A.UC_ARM64_REG_PC) == SENTINEL
                    break
                pc = self.stub(self.at_call)
        except uc.UcError:
            ok = False
        if not ok:
            mu.mem_write(BUF, self.buf0)
            return None
        o = mu.reg_read_batch(e.out_regs)
        crc = zlib.crc32(bytes(mu.mem_read(BUF, BUF_SIZE)))
        mu.mem_write(BUF, self.buf0)
        return list(o[:31]), o[31], o[32] & 0xF0000000, o[33], list(o[34:]), crc


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--exefs', type=Path, default=ROOT / 'build' / 'extracted' / 'v121' / 'exefs')
    ap.add_argument('--modules', default='main,subsdk0,sdk')
    ap.add_argument('--functions', type=int, default=400, help='per module')
    ap.add_argument('--cases', type=int, default=12, help='random states tried per function')
    ap.add_argument('--steps', type=int, default=200000, help='Unicorn instruction cap per case')
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    ap.add_argument('--show', type=int, default=20)
    ap.add_argument('--patches', type=Path, help='patch table whose spin waits are emitted with their gates')
    ap.add_argument('--temps-saved', action='store_true', help='recomp/gen.py --temps-saved; without it (the default, '
                    'as the generator) the stubs (harness.c rt_stub, Runner.stub) leave X9-X15 out of the state they '
                    'fold, as a callee may not read them')
    ap.add_argument('--reg-locals', type=int, default=gen.regs.LOCALS, help='recomp/gen.py --reg-locals')
    ap.add_argument('--kept-locals', type=int, default=0, help='recomp/gen.py --kept-locals for the emitted functions')
    ap.add_argument('--at', help='test these function starts (hex, comma separated) instead of random picks')
    ap.add_argument('--patch-on', default='', help='code patch sets (names, comma separated) switched on for both sides')
    ap.add_argument('--cflags', default='-march=x86-64-v3' if platform.machine().lower() in ('amd64', 'x86_64') else '')
    args = ap.parse_args()
    r = random.Random(args.seed)
    t0 = time.time()
    want = args.modules.split(',')

    work = ROOT / 'build' / 'test' / 'func'
    work.mkdir(parents=True, exist_ok=True)
    images, emitted, tags = [], [], []
    mods = layout(args.exefs)
    table = patches.load(args.patches, mods) if args.patches else patches.Table()
    at = [int(a, 16) for a in args.at.split(',')] if args.at else None
    on = [s for s in args.patch_on.split(',') if s]
    names = [n for n, _, _ in table.sets]
    for s in on:
        if s not in names:
            sys.exit(f'func.py: --patch-on {s}: no such set in {args.patches}')
    PATCH_ON[:] = [1 if i < len(names) and names[i] in on else 0 for i in range(64)]
    tramps = []   # (lo, hi) of the trampolines Unicorn runs for the sets switched on
    for name, base, size in mods:
        if name not in want:
            continue
        mine = [a for a in at if base <= a < base + size] if at is not None else None
        if mine == []:
            continue
        mod = Module(name, args.exefs / name, base, 'c')
        img = work / f'{name}.img'
        img.write_bytes(mod.image)
        patched = table.for_module(name, base) if at is not None else None
        picks, emitter = pick_functions(name, mod, args.functions, r, table.spins_for(name, base), args.kept_locals,
                                        args.reg_locals, not args.temps_saved, mine, patched)
        uc_image = mod.image
        if at is not None:   # retaa / retab: Unicorn's Cortex-A57 has no pointer authentication, the lifter takes them
            data = bytearray(mod.image)   # as ret (its paciasp / pacibsp are hints, nothing on that core)
            for e, end, _ in picks:
                for a in range(e, end, 4):
                    if int.from_bytes(data[a - base:a - base + 4], 'little') in (0xD65F0BFF, 0xD65F0FFF):
                        data[a - base:a - base + 4] = (0xD65F03C0).to_bytes(4, 'little')
            uc_image = bytes(data)
        if on and patched:   # each site of a set switched on: b to its replacement words, then b back to the next word
            data = bytearray(uc_image)
            tbase = base + ((len(data) + 0xFFFF) & ~0xFFFF)
            data += bytes(tbase - base - len(data))
            for a, alts in sorted(patched.items()):
                if not any(e <= a < end for e, end, _ in picks):
                    continue
                words = next((w for k, w in alts if names[k] in on), None)
                if words is None:
                    continue
                t = base + len(data)
                for w in words:
                    lifted = mod.lifter.lift(w, a)
                    if lifted.kind != 'op' or (w & 0x1F000000) == 0x10000000:
                        sys.exit(f'func.py: {a:#x}: replacement word {w:08x} branches or is pc-relative')
                    data += w.to_bytes(4, 'little')
                data += (0x14000000 | ((a + 4 - (base + len(data))) >> 2 & 0x3FFFFFF)).to_bytes(4, 'little')
                data[a - base:a - base + 4] = (0x14000000 | ((t - a) >> 2 & 0x3FFFFFF)).to_bytes(4, 'little')
                print(f'{name}: {a:#x} runs the replacement at {t:#x} in Unicorn ({len(words)} words)')
            tramps.append((tbase, base + len(data)))
            uc_image = bytes(data)
        images.append((base, img, uc_image))
        for entry, end, t in picks:
            emitted.append((name, entry, end, emitter))
            tags.append(t)
    print(f'picked {len(emitted)} functions ({time.time() - t0:.0f}s)', flush=True)

    t1 = time.time()
    globals()['STUB_SKIP_TEMPS'] = not args.temps_saved
    exe = build(work, emitted, args.jobs, args.cflags + ('' if args.temps_saved else ' -DSTUB_SKIP_TEMPS'))
    print(f'compiled ({time.time() - t1:.0f}s)', flush=True)

    buf0 = make_buffer(r, 0.95 if at is not None else 0.4)   # named functions walk objects: mostly pointers
    sites = set()
    for _, entry, end, emitter in emitted:
        sites.update(call_sites(emitter.mod, entry, end))
    runner = Runner([(base, data) for base, _, data in images], buf0, sites, tramps)
    cases, expect, tested, skipped = [], [], set(), 0
    t2 = time.time()
    for k, (name, entry, end, _) in enumerate(emitted):
        for _ in range(args.cases):
            case = make_case(r)
            if at is not None and r.random() < 0.5:   # a step or a scale in v0's low lane
                fpcr, x, sp, nzcv, v = case
                v[0] = v[0] & ~0xFFFFFFFF | struct.unpack('<I', struct.pack('<f', r.uniform(0.05, 2)))[0]
            want_state = runner.run(entry, end, case, args.steps)
            if want_state is None:
                skipped += 1
                continue
            fpcr, x, sp, nzcv, v = case
            cases.append((k, fpcr, x, sp, nzcv, v, True))
            expect.append(want_state)
            tested.add(k)
    print(f'unicorn: {len(cases)} cases on {len(tested)} functions, {skipped} skipped ({time.time() - t2:.0f}s)',
          flush=True)

    t3 = time.time()
    recs, err = run_harness(exe, work, buf0, cases, [(base, img) for base, img, _ in images], stubs=True)
    print(f'harness: {len(recs)} of {len(cases)} cases ({time.time() - t3:.0f}s)')
    if err:
        k = cases[len(recs)][0] if len(recs) < len(cases) else None
        where = f', stopped in {emitted[k][0]} {emitted[k][1]:#x}' if k is not None else ''
        print(f'harness failed: {err}{where}')

    fails, nfail = {}, 0
    for case, want_state, got in zip(cases, expect, recs):
        # Generated functions keep NZCV in a local (recomp/flags.py) and leave c->nzcv as it was: flags only count
        # where the function reads them, which the registers and memory show.
        d = diff_state(got, want_state, check_pc=False, check_mem=True, check_nzcv=False)
        if d:
            nfail += 1
            fails.setdefault(case[0], d[:4])
    by_tag, fail_tag = Counter(), Counter()
    for k in tested:
        by_tag.update(tags[k] or {'plain'})
        if k in fails:
            fail_tag.update(tags[k] or {'plain'})
    print(f'{len(tested)} functions, {len(cases)} cases: {nfail} failing cases, {len(fails)} failing functions '
          f'({time.time() - t0:.0f}s total)')
    print(f'  tested by tag: {dict(by_tag)}; failing by tag: {dict(fail_tag)}')
    for k, d in list(fails.items())[:args.show]:
        name, entry, end, _ = emitted[k]
        print(f'  {name:8} {gen.fname(entry)} ({(end - entry) // 4} insns, {sorted(tags[k])}): {"; ".join(d)}')
    return 1 if fails or err else 0


if __name__ == '__main__':
    sys.exit(main())
