"""Write tools/data/pipeline-keys-oot.bin.gz: the pipeline keys a release carries (runtime/vk_pipeline.c keys_seed).

    python tools/make_seed_keys.py <pipeline-keys.bin> [<pipeline-keys.bin> ...]

Each input is the key file a game folder gathered in play (pipeline-keys.bin beside the exe: a record per graphics
pipeline built, its render state and the ids of its two shaders' inputs). The records of all inputs and of the seed
file there already is are merged, each kept once. Nothing of the game is in a record: hashes and state only. The
file is gzip-compressed (the records are mostly zero bytes); tools/package_oot.py unpacks it into the game folder.
"""
import gzip
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / 'tools' / 'data' / 'pipeline-keys-oot.bin.gz'
MAGIC = b'TOTKPKEY'


def records(path):
    b = Path(path).read_bytes()
    if b[:2] == bytes([0x1f, 0x8b]):
        b = gzip.decompress(b)
    if len(b) < 16 or b[:8] != MAGIC:
        sys.exit(f'{path}: not a pipeline key file')
    version, size = struct.unpack_from('<II', b, 8)
    n = (len(b) - 16) // size
    return version, size, [b[16 + i * size:16 + (i + 1) * size] for i in range(n)]


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    inputs = [Path(p) for p in sys.argv[1:]] + ([OUT] if OUT.is_file() else [])
    shape, seen, out = None, set(), []
    for p in inputs:
        version, size, recs = records(p)
        if shape is None:
            shape = (version, size)
        elif shape != (version, size):
            print(f'{p}: version {version}, record size {size} against {shape}: left out')
            continue
        new = 0
        for r in recs:
            if r not in seen:
                seen.add(r), out.append(r)
                new += 1
        print(f'{p}: {len(recs)} records, {new} new')
    OUT.write_bytes(gzip.compress(MAGIC + struct.pack('<II', *shape) + b''.join(out), 9, mtime=0))
    print(f'{OUT}: {len(out)} records, {OUT.stat().st_size / 1e3:.0f} KB')


if __name__ == '__main__':
    main()
