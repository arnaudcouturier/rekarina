"""Fill the texture cache for every ASTC texture of the game before play (rekarina.exe --prepare-textures).

A GPU without native ASTC sampling decodes each ASTC texture and encodes it to BC3 at its first use, in the frame that
draws it, and keeps the blocks in tex-cache/ beside the game (runtime/vk_texture.c, "the texture cache"); a new
player's first visit of a place therefore paid every one of those encodes in play. This tool reads every BNTX archive
of the player's dump (tools/oot_romfs.py), takes the ASTC textures' headers and storage as the game loads them into
memory, and streams them to `rekarina.exe --prepare-textures`, which keys, decodes, encodes and writes each one exactly
as play would (runtime/tex_prepare.c). Textures already in the cache are skipped, so a stopped run goes on where it
was. Nothing of the game goes anywhere else: the files are written into the game folder's tex-cache.

    .venv/Scripts/python tools/oot_prepare_textures.py <game folder> [--nup <container>] [--exe-name Rekarina.exe]
        [--limit N] [--check] [--estimate]

--estimate prints the set's size on disk and the room the disk has, and does nothing else. The run stops with a message
when the disk has under 2 GiB left, and the exe writes nothing past the folder's limit (REKARINA_TEX_CACHE_MB, which play
enforces at start by deleting files): the game then fills the rest of the cache in play, as before.

Texture quality (REKARINA_TEX_QUALITY; auto picks half under 7.25 GiB of free video memory and quarter under 6.2) loads
large ASTC textures from their second or third level, which is other storage under another key. The exe prepares those
shapes too where play could pick them: REKARINA_TEX_QUALITY half or quarter, or auto on a card of less than 10 GiB
(REKARINA_PREPARE_TEXTURES_LEVELS=all or full overrides it).
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
import time
from collections import deque
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import oot_romfs                                         # noqa: E402

ASTC_TYPES = range(0x2d, 0x3b)   # BNTX format type << 8 | variant (1 UNORM, 6 sRGB), in the runtime's footprint order
NVN_TARGET_2D, NVN_TARGET_3D, NVN_TARGET_2D_ARRAY = 1, 2, 4


def nvn_format(fmt):
    """The NVN format of a BNTX ASTC format (runtime/vk_formats.c tex_vk_format: 0x79.. UNORM, 0x87.. sRGB)."""
    t, v = fmt >> 8, fmt & 0xff
    if t not in ASTC_TYPES or v not in (1, 6):
        return None
    return (0x87 if v == 6 else 0x79) + (t - 0x2d)


def bc3_bytes(w, h, d, levels, layers, is3d):
    total = 0
    for lv in range(levels):
        lw, lh, ld = max(1, w >> lv), max(1, h >> lv), max(1, d >> lv) if is3d else 1
        total += ((lw + 3) // 4) * ((lh + 3) // 4) * 16 * ld * (1 if is3d else layers)
    return total


def textures(data):
    """(record header, storage bytes, BC3 bytes) of each ASTC texture of a BNTX file (the "Ounc" section's table at
    +0x20: count +4, table pointer +8; BRTI: dim +0x11, levels u16 +0x16, format +0x1C, width/height/depth/layers
    +0x24.., packaged layout +0x34, storage bytes +0x50, pointer to the level offsets +0x70)."""
    if data[:4] != b'BNTX':
        return
    count = struct.unpack_from('<I', data, 0x24)[0]
    table = struct.unpack_from('<Q', data, 0x28)[0]
    for i in range(count):
        o = struct.unpack_from('<Q', data, table + 8 * i)[0]
        if data[o:o + 4] != b'BRTI':
            continue
        dim = data[o + 0x11]
        levels = struct.unpack_from('<H', data, o + 0x16)[0]
        fmt, _access, w, h, d, layers = struct.unpack_from('<IIiiiI', data, o + 0x1C)
        layout0 = data[o + 0x34]
        size = struct.unpack_from('<I', data, o + 0x50)[0]
        ptrs = struct.unpack_from('<Q', data, o + 0x70)[0]
        nf = nvn_format(fmt)
        if nf is None or not 0 < ptrs < len(data) - 8:
            continue
        first = struct.unpack_from('<Q', data, ptrs)[0]
        if first + size > len(data):
            continue
        is3d = dim == 3 and d > 1
        if is3d:
            target, depth = NVN_TARGET_3D, d
        elif layers > 1:
            target, depth = NVN_TARGET_2D_ARRAY, layers
        else:
            target, depth = NVN_TARGET_2D, 1
        head = b'TXPR' + struct.pack('<7IQ', nf, target, w, h, depth, levels, layout0, size)
        shapes = 0   # half and quarter (runtime/vk_texture.c texq_shape), an upper bound: the tile checks are left out
        if target == NVN_TARGET_2D and levels >= 2 and (w >= 512 or h >= 512):
            shapes += bc3_bytes(max(1, w >> 1), max(1, h >> 1), 1, levels - 1, 1, False)
            if levels >= 3 and (w >= 1024 or h >= 1024):
                shapes += bc3_bytes(max(1, w >> 2), max(1, h >> 2), 1, levels - 2, 1, False)
        yield head, data[first:first + size], (bc3_bytes(w, h, d, levels, layers, is3d), shapes)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('game', type=Path, help='the game folder: rekarina.exe (or --exe-name) and its tex-cache')
    ap.add_argument('--nup', default=oot_romfs.NUP)
    ap.add_argument('--exe-name', default='rekarina.exe' if os.name == 'nt' else 'rekarina')
    ap.add_argument('--limit', type=int, default=0, help='at most this many archives (a test)')
    ap.add_argument('--check', action='store_true', help='encode the textures the cache holds too and compare')
    ap.add_argument('--estimate', action='store_true', help='the set size and the room on the disk, nothing else')
    a = ap.parse_args()
    t0 = time.monotonic()
    fs = oot_romfs.RomFS(a.nup)
    names = sorted(n for n in fs.index if n.endswith('.bntx.zs') or n.endswith('.bntx'))
    if a.limit:
        names = names[::max(1, len(names) // a.limit)][:a.limit]
    print(f'{len(names)} texture archives', flush=True)

    def read(name):
        try:
            raw = fs.read_raw(name)
            return list(textures(fs.unzs(raw) if raw[:4] == b'\x28\xb5\x2f\xfd' else raw))
        except (ValueError, KeyError, struct.error) as e:
            print(f'{name}: skipped ({e})', flush=True)
            return []

    cache = a.game / 'tex-cache'
    if a.estimate:
        need, extra, n = 0, 0, 0
        with ThreadPoolExecutor(max_workers=min(16, os.cpu_count() or 4)) as ex:
            for got in ex.map(read, names):
                for _h, _s, (bc, shapes) in got:
                    need, extra, n = need + bc, extra + shapes, n + 1
        free = shutil.disk_usage(a.game).free
        print(f'{n} ASTC textures, {need / 2**30:.2f} GiB as BC3, {extra / 2**30:.2f} GiB more with the half and quarter '
              f'shapes (upper bounds: textures of equal bytes share a file); {free / 2**30:.1f} GiB free on the disk; '
              f'{time.monotonic() - t0:.0f} s', flush=True)
        return 0
    exe = a.game / a.exe_name
    if not exe.is_file():
        sys.exit(f'{exe}: missing')
    env = dict(os.environ)
    if a.check:
        env['REKARINA_PREPARE_TEXTURES_CHECK'] = '1'
    p = subprocess.Popen([str(exe), '--prepare-textures'], cwd=str(a.game), stdin=subprocess.PIPE, env=env)
    sent = [0, 0]

    def room_ok():
        return shutil.disk_usage(a.game).free > (2 << 30)   # 2 GiB left for everything else

    # At most WINDOW archives read ahead: the encoder is slower than the reads, and an unbounded Executor.map would hold
    # the whole game's texture storage in memory.
    WINDOW = 24
    ex = ThreadPoolExecutor(max_workers=min(8, os.cpu_count() or 4))
    ahead = deque()
    it = iter(names)
    archives = [0, time.monotonic()]

    def counted():   # the launcher's background job reads the count (installer/oot.py TEXTURE_ARCHIVES)
        archives[0] += 1
        if archives[0] == len(names) or time.monotonic() - archives[1] >= 2:
            archives[1] = time.monotonic()
            print(f'textures: {archives[0]} of {len(names)} archives', flush=True)

    try:
        for name in it:
            ahead.append(ex.submit(read, name))
            if len(ahead) < WINDOW:
                continue
            for head, storage, _bc in ahead.popleft().result():
                p.stdin.write(head)
                p.stdin.write(storage)
                sent[0] += 1
                sent[1] += len(storage)
            counted()
            if not room_ok():
                print('the disk is nearly full: the texture preparation stops here; the game fills the rest of the '
                      'cache in play', flush=True)
                ahead.clear()
                break
        while ahead:
            for head, storage, _bc in ahead.popleft().result():
                p.stdin.write(head)
                p.stdin.write(storage)
                sent[0] += 1
                sent[1] += len(storage)
            counted()
    except BrokenPipeError:   # the exe stopped (no texture cache on this device, or an error): nothing more to send
        pass
    finally:
        ex.shutdown(wait=False, cancel_futures=True)
        try:
            p.stdin.close()
        except OSError:
            pass
    rc = p.wait()
    print(f'{sent[0]} textures ({sent[1] / 2**30:.2f} GiB of storage) sent; --prepare-textures rc={rc}; '
          f'total {time.monotonic() - t0:.0f} s; cache {cache}', flush=True)
    return rc


if __name__ == '__main__':
    sys.exit(main())
