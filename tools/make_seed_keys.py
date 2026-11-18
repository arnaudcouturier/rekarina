"""Write the pipeline keys a release carries (runtime/vk_pipeline.c keys_seed, mk_open, cu_load):
tools/data/pipeline-keys-oot.bin.gz, pipeline-keys-mesh-oot.bin.gz and compute-used-oot.bin.gz.

    python tools/make_seed_keys.py <key file or game folder> [...]

Each input is a key file a game folder gathered in play, or a folder holding them: pipeline-keys.bin beside the exe (a
record per graphics pipeline built, its render state and the ids of its two shaders' inputs), pipeline-keys-mesh.bin
(a record per mesh or geometry pipeline, the ids of its three stages' inputs and its state) or compute-used.bin (the
input id of every compute shader a dispatch asked for). Each goes to the seed of its kind, merged with the records of
the seed file there already is, each kept once. Nothing of the game is in a record: hashes and state only. The files
are gzip-compressed (the records are mostly zero bytes); tools/package_oot.py unpacks them into the game folder, where
the game's entry step builds every one before play.
"""
import gzip
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = {b'TOTKPKEY': ROOT / 'tools' / 'data' / 'pipeline-keys-oot.bin.gz',
       b'TOTKMKEY': ROOT / 'tools' / 'data' / 'pipeline-keys-mesh-oot.bin.gz',
       b'TOTKCUSE': ROOT / 'tools' / 'data' / 'compute-used-oot.bin.gz'}
FOLDER_FILES = ('pipeline-keys.bin', 'pipeline-keys-mesh.bin', 'compute-used.bin')


def records(path):
    b = Path(path).read_bytes()
    if b[:2] == bytes([0x1f, 0x8b]):
        b = gzip.decompress(b)
    if len(b) < 12 or b[:8] not in OUT:
        sys.exit(f'{path}: not a pipeline key file')
    if b[:8] == b'TOTKCUSE':   # "TOTKCUSE", a version (u32), then 32-byte input ids (vk_pipeline.c cs_use)
        version, size, head = struct.unpack_from('<I', b, 8)[0], 32, 12
    else:                      # magic, version, record size, records (KeysHeader)
        version, size = struct.unpack_from('<II', b, 8)
        head = 16
    n = (len(b) - head) // size
    return b[:8], version, size, [b[head + i * size:head + (i + 1) * size] for i in range(n)]


def header(magic, version, size):
    return magic + (struct.pack('<I', version) if magic == b'TOTKCUSE' else struct.pack('<II', version, size))


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    inputs = []
    for a in map(Path, sys.argv[1:]):
        inputs += [a / f for f in FOLDER_FILES if (a / f).is_file()] if a.is_dir() else [a]
    inputs += [p for p in OUT.values() if p.is_file()]
    shape, seen, out = {}, {}, {}
    for p in inputs:
        magic, version, size, recs = records(p)
        if magic not in shape:
            shape[magic], seen[magic], out[magic] = (version, size), set(), []
        elif shape[magic] != (version, size):
            print(f'{p}: version {version}, record size {size} against {shape[magic]}: left out')
            continue
        new = 0
        for r in recs:
            if r not in seen[magic] and any(r):
                seen[magic].add(r), out[magic].append(r)
                new += 1
        print(f'{p}: {len(recs)} records, {new} new')
    for magic, recs in out.items():
        dst = OUT[magic]
        dst.write_bytes(gzip.compress(header(magic, *shape[magic]) + b''.join(recs), 9, mtime=0))
        print(f'{dst}: {len(recs)} records, {dst.stat().st_size / 1e3:.0f} KB')


if __name__ == '__main__':
    main()
