"""Prepare every OoT shader once, before play: the Switch 2 counterpart of tools/prepare_shaders.py.

A shader the replay meets for the first time is translated and its pipeline built while the game waits: the first rock
The hero picks up stopped the game for 2.8 s. This tool reads the shaders straight from the player's NUP (tools/oot_romfs.py:
the Shader, Layout2Shader, Effect and UI archives and the agl packages) and the game's modules, writes their default
variants next to the build's rekarina.exe in the record layouts of runtime/gfx_shader.c, and runs `rekarina.exe
--prepare-shaders`, which translates them all and builds their stage libraries and compute pipelines once, so that the
shader cache and the driver's own disk cache hold them. Game-derived output stays in the build directory (ignored).

    .venv/Scripts/python tools/oot_prepare_shaders.py build/oot [--nup <container>] [--exefs ../build/oot/exefs-plain] [--no-run]
"""
import argparse
import gzip
import hashlib
import os
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import oot_romfs                                         # noqa: E402
import shader_census as census                           # noqa: E402
import prepare_shaders as prep                           # noqa: E402

ROOTS = ('Shader/', 'Layout2Shader/', 'Effect/', 'UI/', 'agl/', 'Lib/', 'System/', 'Foliage/', 'Pack/', 'Movie/')
SEED_ANSWERS = Path(__file__).resolve().parent / 'data' / 'shader-answers-oot.bin.gz'   # tools/make_seed_answers.py
# A mesh or task program the runtime also translates as a compute kernel (runtime/gfx.h GFX_STAGE_MSC, GFX_STAGE_TSC:
# vk_exec_draw.c mesh_emulate) is hashed with that stage and the kernel's revision (gfx_shader.c build_code_hash,
# MESHC_REVISION): the same program under another hash, which the seed's played variants name.
EMU_STAGES = {7: 8, 6: 9}     # ms -> msc, ts -> tsc
MESHC_REVISION = 3
# Beside the seed files: what they were made from. A run that was cut short in --prepare-shaders (the setup's longest
# step) finds it at its resume and does not read the dump again (40 s).
INVENTORY_DONE = '.seed-inventory'


def inventory_stamp(a):
    """A hash of everything the seed files follow from: these tools, the seed of played variants, the dump (by its path,
    size and time) and the game's modules (names, sizes, times)."""
    h = hashlib.sha256(b'oot-seed-inventory-1' + (b'-met' if getattr(a, 'met_only', False) else b''))
    here = Path(__file__).resolve().parent
    for f in (Path(__file__).resolve(), here / 'prepare_shaders.py', here / 'oot_romfs.py', here / 'shader_census.py', SEED_ANSWERS):
        h.update(f.name.encode() + b'\0' + (f.read_bytes() if f.is_file() else b'missing') + b'\1')
    for f in [Path(a.nup)] + (sorted(Path(a.exefs).iterdir()) if Path(a.exefs).is_dir() else []):
        try:
            st = f.stat()
            h.update(f'{f.resolve()}\0{st.st_size}\0{st.st_mtime_ns}\1'.encode())
        except OSError:
            h.update(f'{f}\0missing\1'.encode())
    return h.hexdigest()


def ndve_shaders(data):
    """The compute programs of the video decoder's file (Movie/Ndve/*.dat.zs, magic NDVE), which play dispatches when
    a video runs: an NDSR package inside it (census.ndsr_shaders), and tables of <u32 count, 1, 0xa0, u32> followed by
    count entries <u32 offset of the control section from the count, u32 offset of the code from the control section,
    u32 code bytes, 0>. 9 of the 23 programs the inventory did not find before were these (2026-11-16)."""
    at = data.find(b'NDSR')
    if at >= 0:
        try:
            yield from census.ndsr_shaders(data[at:])
        except (ValueError, struct.error):
            pass
    # The same entries also follow a shorter table head, <u32 count, 0>, at +8 (offsets still from the count): the one
    # program of vid_seq5 that play dispatched and the setup had not prepared (2026-11-16).
    for p in range(0, len(data) - 16, 4):
        n, one, head = struct.unpack_from('<3I', data, p)
        if one == 1 and head == 0xa0:
            first = p + 16
        elif one == 0 and 0 < n <= 16:
            first = p + 8
        else:
            continue
        if not 0 < n <= 256 or first + 16 * n > len(data):
            continue
        for i in range(n):
            off, rel, size, zero = struct.unpack_from('<4I', data, first + 16 * i)
            c = p + off
            if zero or c + census.NVN2_CONTROL > len(data) or data[c:c + 4] != census.CONTROL_MAGIC:
                continue
            control = data[c:c + census.NVN2_CONTROL]
            stage = census.NVN2_STAGES.get(struct.unpack_from('<I', control, 0x54)[0] >> 16)
            if not stage or c + rel + size > len(data):
                continue
            control += bytes(0x880 - census.NVN2_CONTROL)
            code = data[c + rel:c + rel + size]
            yield census.digest(control + code), stage, control, code


def inventory(fs, exefs):
    shaders, rejected = {}, [0]

    def add(origin, name, found):
        for key, stage, control, code in found:
            shaders.setdefault(key, dict(stage=stage, control=control, code=code, source=name, origin=origin))

    def visit(name, data, depth=0):
        if data[:4] == b'BAHS':
            return add('sharcb', name, census.sharcb_shaders(data, rejected))
        if data[:4] == b'NDVE':
            return add('ndve', name, ndve_shaders(data))
        if data[:4] == b'NDSR':
            return add('ndsr', name, census.ndsr_shaders(data))
        if data[:4] == b'SARC':
            if depth <= 4:
                for member, raw in census.sarc(data).items():
                    visit(name + '/' + member, fs.unzs(raw) if raw[:4] == census.ZSTD else raw, depth + 1)
            return
        pos = 0
        while (pos := data.find(b'BNSH\0\0\0\0', pos)) >= 0:
            size = struct.unpack_from('<I', census.checked(data, pos + 28, 4))[0]
            add('bnsh', name, census.bnsh_shaders(census.checked(data, pos, size)))
            pos += max(size, 8)

    names = sorted(n for n in fs.index if n.startswith(ROOTS) or n.endswith(('.bfsha.zs', '.bfsha', '.pck', '.bnsh')))
    read = 0
    for name in names:
        raw = fs.read_raw(name)
        read += 1
        if raw[:4] not in (census.ZSTD, b'SARC', b'FSHA', b'BNSH', b'BAHS', b'NDSR') and b'BNSH\0\0\0\0' not in raw:
            continue
        try:
            visit(name, fs.unzs(raw) if raw[:4] == census.ZSTD else raw)
        except (ValueError, KeyError, struct.error) as e:
            print(f'{name}: skipped ({e})', flush=True)
    for module in sorted(exefs.iterdir()):
        if module.name in ('main', 'sdk', 'subsdk0', 'rtld'):
            visit('module:' + module.name, census.nso_image(module))
    print(f'{read} files read, {len(shaders)} shaders, {rejected[0]} records rejected', flush=True)
    return shaders


def write_seed_files(a, out, t0):
    """The inventory of the dump's shaders and the seed's played variants, written as seed-programs.bin and
    seed-answers.bin beside the build's exe for --prepare-shaders."""
    shaders = inventory(oot_romfs.RomFS(a.nup), a.exefs)
    progs, recs, seen, skipped, stages = {}, [], set(), 0, {}
    emu = {}   # the kernel hash of a mesh or task program -> (its stage, the program's body without stage and hash)
    for key in sorted(shaders):
        s = shaders[key]
        stage = prep.STAGES.get(s['stage'])
        if stage is None:
            skipped += 1
            continue
        stages[s['stage']] = stages.get(s['stage'], 0) + 1
        control, code = bytearray(s['control'][:prep.CONTROL]), s['code']
        # The file holds offsets and sizes of its own sections in these words (0x30, 0xd4-0xdf); in the program the game hands
        # over they are zero, and the cache hashes the section, so with them hardly any key matched what play asks for.
        control[0x30:0x34] = bytes(4)
        control[0xd4:0xe0] = bytes(12)
        control = bytes(control)
        # As the replay hands a program to the cache (runtime/nvn_ctl.h nvn_ctl_code_len): the code area runs to control +0x44,
        # which is the instructions' end at +0x38 rounded up to 0x100. A shorter length gave other keys than play asks for
        # and failed req_init's check for 5,691 of the 19,774 programs. The padding of the area is zeros.
        c1_off, length = struct.unpack_from('<II', control, 0x40)
        code = code[:length].ljust(length, bytes(1))
        c1 = code[c1_off:length]
        if key not in progs:
            h = prep.code_hash(stage, control, code, length, c1)
            progs[key] = (h, struct.pack('<III', stage, length, len(c1)) + h + control + code[:length] + c1)
            if stage in EMU_STAGES:
                es = EMU_STAGES[stage]
                eh = prep.code_hash(es | MESHC_REVISION << 16, control, code, length, c1)
                emu[eh] = (es, struct.pack('<II', length, len(c1)), control + code[:length] + c1)
        h = progs[key][0]
        # No texture answers: the first translation names the real types (runtime/gfx_shader_guess.c), which the 0x80
        # flag asks the runtime to apply.
        packed = prep.answers({}, stage, control)
        if (h, packed) in seen:
            continue
        seen.add((h, packed))
        recs.append(struct.pack('<I', stage) + h + bytes(32) + packed + bytes([2 | 0x80]))
    # The variants play met (tools/make_seed_answers.py): the answers of real draws, which the default variants above do
    # not always give. They go first, so the boot prebuild warms them before the rest; a record whose program the dump
    # does not hold (another version of the game) is left out.
    played, unmatched, kernels = [], 0, 0
    hashes = {h for h, _ in progs.values()}
    if SEED_ANSWERS.is_file():
        b = gzip.decompress(SEED_ANSWERS.read_bytes())
        if b[:8] == b'TOTKSANS' and struct.unpack_from('<II', b, 8) == (1, prep.ANS_REC):
            for i in range(16, len(b) - prep.ANS_REC + 1, prep.ANS_REC):
                r = b[i:i + prep.ANS_REC]
                h, packed = r[4:36], r[68:prep.ANS_REC - 1]
                if h not in hashes and h in emu:   # a compute kernel of a mesh or task program: its body under that stage
                    es, sizes, body = emu[h]
                    progs[('kernel', h)] = (h, struct.pack('<I', es) + sizes + h + body)
                    hashes.add(h)
                    kernels += 1
                if h not in hashes:
                    unmatched += 1
                elif (h, packed) not in seen:
                    seen.add((h, packed))
                    played.append(r)
        print(f'{len(played)} variants met in play taken from {SEED_ANSWERS.name} ({kernels} of them compute kernels of mesh '
              f'or task programs), {unmatched} for programs not in this dump', flush=True)
    # --met-only: the variants play met alone; the default variants of every other shader of the dump (five of six)
    # are left for a later run without the flag, which finds these translated and built and goes on from them.
    recs = played + ([] if a.met_only else recs)
    with open(out / 'seed-programs.bin', 'wb') as f:
        f.write(b'TOTKSPRG' + struct.pack('<II', 1, 0))
        for _, body in progs.values():
            f.write(body)
    with open(out / 'seed-answers.bin', 'wb') as f:
        f.write(b'TOTKSANS' + struct.pack('<II', 1, prep.ANS_REC))
        for rec in recs:
            f.write(rec)
    print(f'{len(recs)} variants of {len(progs)} shaders {stages}, {skipped} of other stages, {time.monotonic() - t0:.1f} s',
          flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('build', type=Path)
    ap.add_argument('--nup', default=oot_romfs.NUP)
    ap.add_argument('--exefs', type=Path, default=Path('../build/oot/exefs-plain'))
    ap.add_argument('--no-run', action='store_true')
    ap.add_argument('--met-only', action='store_true',
                    help='prepare only the variants play met (tools/data/shader-answers-oot.bin.gz), not the default '
                         'variant of every other shader of the dump: a sixth of the work')
    ap.add_argument('--exe-name', default='rekarina.exe' if os.name == 'nt' else 'rekarina',
                    help='the file name the game will be started under: the preparation runs a copy of rekarina.exe of that name')
    a = ap.parse_args()
    t0 = time.monotonic()
    out = a.build.resolve()
    stamp = inventory_stamp(a)
    done = out / INVENTORY_DONE
    if (out / 'seed-programs.bin').is_file() and (out / 'seed-answers.bin').is_file() and done.is_file() and \
            done.read_text(encoding='ascii').strip() == stamp:
        n = max(0, ((out / 'seed-answers.bin').stat().st_size - 16) // prep.ANS_REC)
        print(f'{n} variants of the shaders from the run before (seed-programs.bin, seed-answers.bin): the dump is '
              f'not read again', flush=True)
    else:
        done.unlink(missing_ok=True)
        write_seed_files(a, out, t0)
        done.write_text(stamp + '\n', encoding='ascii')
    if a.no_run:
        return 0
    # NVIDIA's driver keeps its disk cache in a file named after the executable's file name (the same name in another
    # folder finds it, another name in the same folder does not): prepared as rekarina.exe and played as Rekarina.exe, none
    # of the 2.4 GiB of prepared pipelines was found, and the first minutes of play built them again while the game
    # waited (frames of 36 s and 32 s in the first run of a fresh install, 2026-11-11). So the preparation runs under
    # the name the game is started with.
    built = out / ('rekarina.exe' if os.name == 'nt' else 'rekarina')   # tools/build_game.py's name; no suffix on Linux
    exe, made = out / a.exe_name, False
    if not exe.is_file():
        try:
            os.link(built, exe)
        except OSError:
            shutil.copy2(built, exe)
        made = True
    try:
        r = subprocess.run([str(exe), '--prepare-shaders'], cwd=str(out))
    finally:
        if made:
            exe.unlink(missing_ok=True)
    print(f'--prepare-shaders: rc={r.returncode}; total {time.monotonic() - t0:.1f} s', flush=True)
    return r.returncode


if __name__ == '__main__':
    sys.exit(main())
