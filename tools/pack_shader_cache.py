"""Fold the loose files of a shader cache into <cache>/shader-cache.pack and delete them.

The translator writes four files per shader into the cache (runtime/gfx_shader.c): <key>.spv, <key>.res.json,
<name>.use and in/<input id>.in, about 250,000 files after an install, which are slow to copy, back up, scan and sync.
rekarina.exe reads the pack first and the loose files after it, and keeps writing new translations as loose files, so
this tool can run again at any time the game is closed: it merges the existing pack with the loose files (a loose file
wins over a packed one of the same name) and writes the pack anew. The installer runs it after preparing the shaders.

Layout, little-endian (gfx_shader.c, section "cache pack"): b'TKSPACK2', u32 count, u32 0, then count entries of 48
bytes sorted by (kind, name): name (the 32 bytes the file name's 64 hex digits spell), u32 kind (low byte: 1 .spv,
2 .res.json, 3 .use, 4 in/*.in; bit 8: the data is one zstd frame of the file), u32 size as stored, u64 offset; then
the data. The translated shaders are 6 of the 7 GiB of an install's cache and compress to about a sixth, so the pack
is written, copied into the game folder and read at a start as about 1.2 GiB instead of 7. A pack of the layout
before (b'TKSPACK1': the same without the compression) is read and written anew in this one.

    python tools\\pack_shader_cache.py <install folder>\\shader-cache [--keep] [--merge-from <another pack>]

--merge-from first adds the entries of another pack made under the same translator that this cache lacks (a union):
a setup that prepares the shaders again keeps what the game folder's pack held.

--keep leaves the loose files in place. Close the game first: a running rekarina.exe has read the old pack's index.
"""
import argparse
import os
import re
import shutil
import struct
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

try:
    import zstandard
except ImportError:   # the release's Python 3.14: its own compression.zstd behind the package's name (tools/py314)
    sys.path.insert(0, str(Path(__file__).resolve().parent / 'py314'))
    import zstandard

MAGIC, MAGIC_PLAIN = b'TKSPACK2', b'TKSPACK1'
PACK = 'shader-cache.pack'
KINDS = {'.spv': 1, '.res.json': 2, '.use': 3, '.in': 4}
NAME = re.compile(r'([0-9a-f]{64})(\.spv|\.res\.json|\.use|\.in)')
USE_SIZE = 20     # gfx_shader.c use_store: magic, version, three masks
ZSTD = 0x100      # in an entry's kind: the data is a zstd frame
LEVEL = 3         # 400 MB/s a thread; level 19 gains a tenth for forty times the time
SMALL = 256       # a file under this many bytes is stored as it is
BATCH = 512       # files read and compressed at a time, on THREADS threads
THREADS = max(1, min(8, (os.cpu_count() or 2) - 1))


def loose_files(cache):
    """{(kind, name): path} of the cache's loose files; *.in only in in/."""
    found = {}
    for folder, kinds in ((cache, (1, 2, 3)), (cache / 'in', (4,))):
        if not folder.is_dir():
            continue
        with os.scandir(folder) as it:
            for e in it:
                m = NAME.fullmatch(e.name)
                if not m or not e.is_file():
                    continue
                kind = KINDS[m.group(2)]
                if kind in kinds:
                    found[(kind, bytes.fromhex(m.group(1)))] = Path(e.path)
    return found


def read_pack(path):
    """({(kind, name): (offset, stored size, stored kind)}, the pack is of the layout before) of an existing pack;
    ({}, False) when there is none or it fails a check."""
    try:
        f = open(path, 'rb')
    except FileNotFoundError:
        return {}, False
    with f:
        size = os.fstat(f.fileno()).st_size
        head = f.read(16)
        if len(head) != 16 or head[:8] not in (MAGIC, MAGIC_PLAIN):
            print(f'{path}: not a cache pack, rebuilt from the loose files only', flush=True)
            return {}, False
        count = struct.unpack_from('<I', head, 8)[0]
        raw = f.read(48 * count)
        if len(raw) != 48 * count:
            print(f'{path}: truncated index, rebuilt from the loose files only', flush=True)
            return {}, False
        out = {}
        for i in range(count):
            name, kind, length, offset = struct.unpack_from('<32sIIQ', raw, 48 * i)
            if offset + length <= size and (head[:8] == MAGIC or kind < ZSTD):
                out[(kind & 0xff, name)] = (offset, length, kind)
        return out, head[:8] == MAGIC_PLAIN


def merge(pack_path, other_path):
    """The entries of the pack at other_path that the pack at pack_path lacks, added to it (both made under the same
    translator identity: their entries are named by what they hold). The stored bytes are copied as they are. Returns
    (entries after, entries added); (0, 0) when other_path holds no pack. A setup that prepares the shaders again from a
    fresh temp folder keeps what the game folder's pack held this way (installer/oot.py, tools/package_oot.py)."""
    other, _ = read_pack(other_path)
    if not other:
        return 0, 0
    mine, _ = read_pack(pack_path)
    extra = [k for k in other if k not in mine]
    if not extra:
        return len(mine), 0
    keys = sorted(set(mine) | set(other))
    tmp = Path(pack_path).with_name(PACK + '.merge.tmp')
    offset = 16 + 48 * len(keys)
    index = []
    with open(tmp, 'wb') as f, open(other_path, 'rb') as fo, (open(pack_path, 'rb') if mine else open(other_path, 'rb')) as fm:
        f.write(bytes(offset))
        for key in keys:
            src, (at, length, kind) = (fm, mine[key]) if key in mine else (fo, other[key])
            src.seek(at)
            data = src.read(length)
            if len(data) != length:
                continue
            f.write(data)
            index.append(struct.pack('<32sIIQ', key[1], kind, length, offset))
            offset += length
        f.seek(0)
        f.write(MAGIC + struct.pack('<II', len(index), 0) + b''.join(index))
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, pack_path)
    return len(index), len(extra)


def valid(kind, data):
    return len(data) == USE_SIZE if kind == 3 else len(data) > 0


def stored(kind, data):
    """(the kind word, the bytes) a file goes into the pack as: a zstd frame where that is smaller."""
    if kind != 3 and len(data) >= SMALL:
        z = zstandard.ZstdCompressor(level=LEVEL).compress(data)
        if len(z) < len(data) - len(data) // 16:
            return kind | ZSTD, z
    return kind, data


def pack(cache, keep=False):
    cache = Path(cache)
    out = cache / PACK
    packed, plain = read_pack(out)
    loose = loose_files(cache)
    if not loose and not plain:
        # Nothing to fold in: every file of the cache is already in the pack, so writing it again copies the same
        # bytes over the same entries (and leaves a file system the old pack's blocks to release as well). The
        # installer runs this after every prepare. A pack of the layout before is written anew once, compressed.
        return len(packed), 0, 0, out.stat().st_size if out.is_file() else 0
    keys = sorted(set(packed) | set(loose))
    tmp = out.with_name(PACK + '.tmp')
    index, written, skipped, offset = [], [], 0, 16 + 48 * len(keys)
    old = open(out, 'rb') if packed else None

    def one(key):
        """(kind word, bytes, the loose file it came from or None) of a key, None when nothing valid holds it. The
        packed copy is read by the writing thread (one handle): this gives its place instead of its bytes."""
        if key in loose:   # a valid loose file, else the packed copy, else nothing
            try:
                data = loose[key].read_bytes()
            except OSError:
                data = None
            if data is not None and valid(key[0], data):
                return stored(key[0], data) + (loose[key],)
        return packed.get(key)

    try:
        with open(tmp, 'wb') as f, ThreadPoolExecutor(THREADS) as pool:
            f.write(bytes(offset))   # header and index, written once the offsets are known
            for at in range(0, len(keys), BATCH):
                batch = keys[at:at + BATCH]
                got = list(pool.map(one, batch))
                again = []   # packed copies of the layout before: read here, compressed on the pool
                for i, g in enumerate(got):
                    if g is not None and len(g) == 3 and isinstance(g[0], int) and not isinstance(g[1], (bytes, bytearray)):
                        old.seek(g[0])
                        data = old.read(g[1])
                        if g[2] & ZSTD:
                            got[i] = (g[2], data, None)
                        else:
                            again.append((i, batch[i][0], data))
                for (i, kind, data), s in zip(again, pool.map(lambda a: stored(a[1], a[2]) if valid(a[1], a[2]) else None, again)):
                    got[i] = s + (None,) if s else None
                for key, g in zip(batch, got):
                    if g is None:
                        skipped += 1
                        continue
                    kind, data, source = g
                    if source is not None:
                        written.append(source)
                    f.write(data)
                    index.append(struct.pack('<32sIIQ', key[1], kind, len(data), offset))
                    offset += len(data)
            f.seek(0)
            f.write(MAGIC + struct.pack('<II', len(index), 0) + b''.join(index))
            f.flush()
            os.fsync(f.fileno())
    finally:
        if old:
            old.close()
    os.replace(tmp, out)
    if not keep:
        for p in written:
            p.unlink(missing_ok=True)
        try:
            (cache / 'in').rmdir()   # only when empty
        except OSError:
            pass
    return len(index), len(written), skipped, offset


def main():
    ap = argparse.ArgumentParser(description='Fold a shader cache into shader-cache.pack.')
    ap.add_argument('cache', type=Path, help='the shader-cache folder next to rekarina.exe')
    ap.add_argument('--keep', action='store_true', help='keep the loose files')
    ap.add_argument('--merge-from', type=Path, metavar='PACK',
                    help='first add the entries of that pack (made under the same translator) the cache lacks')
    a = ap.parse_args()
    if not a.cache.is_dir():
        sys.exit(f'{a.cache} is not a folder')
    if a.merge_from and a.merge_from.is_file() and a.merge_from.resolve() != (a.cache / PACK).resolve():
        if (a.cache / PACK).is_file():
            total, added = merge(a.cache / PACK, a.merge_from)
            print(f'{a.cache / PACK}: {added} entries of {a.merge_from} added ({total} in all)', flush=True)
        else:
            shutil.copy2(a.merge_from, a.cache / PACK)
            print(f'{a.cache / PACK}: copied from {a.merge_from}', flush=True)
    n, new, skipped, size = pack(a.cache, a.keep)
    print(f'{a.cache / PACK}: {n} files, {size / 2**20:.0f} MiB ({new} loose files folded in'
          f'{", " + str(skipped) + " unreadable or empty skipped" if skipped else ""})', flush=True)


if __name__ == '__main__':
    main()
