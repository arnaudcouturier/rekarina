"""Write tools/data/shader-answers-oot.bin.gz: the shader variants play needs, which a release prepares at install.

    python tools/make_seed_answers.py <game folder> [<game folder> ...]

A translated shader's key holds the answers the draw's state gave (texture types, formats, the mesh output kind:
runtime/gfx_shader.c "answers met in play"), so the default variants the setup prepares from the dump
(tools/oot_prepare_shaders.py) are not all that play asks for. A variant first met in play is translated then, and its
draw or dispatch is left out until it is there; and a pipeline key of the release's seed (tools/make_seed_keys.py)
whose shader the player's cache does not hold is dropped by the boot prewarm, so that pipeline too is built in play.

Each input is a game folder that gathered these files in play: shader-answers.bin (every variant resolved, by input
id), pipeline-keys.bin and pipeline-keys-mesh.bin (the shaders of every graphics, mesh and geometry pipeline built)
and compute-used.bin (every compute shader dispatched). The seed takes the answers records of the input ids those
key files and the release's key seeds (tools/data) name, once per (stage, program hash, answers), with the input id cleared (the translator's use mask decides
it) and the priority byte set to 1 (warm-up); the seed there already is is kept. Nothing of the game is in a record:
the stage, the hash of the program and the answers, which are what the runtime knew of the draw. The setup's
tools/oot_prepare_shaders.py takes the records whose program the dump holds into the variants it prepares.
"""
import gzip
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / 'tools' / 'data' / 'shader-answers-oot.bin.gz'
REC = 4 + 32 + 32 + 147 + 1   # runtime/gfx_shader.c ANS_REC: stage, program hash, input id, packed answers, priority


def read(path, magic):
    b = Path(path).read_bytes()
    if b[:2] == bytes([0x1f, 0x8b]):
        b = gzip.decompress(b)
    if len(b) < 16 or b[:8] != magic:
        return None, None
    return b, struct.unpack_from('<II', b, 8)


def named_ids(folder, keys=None, mesh=None, cu=None):
    """The input ids the folder's key files name (or the given key files)."""
    ids = set()
    keys = keys or folder / 'pipeline-keys.bin'
    mesh = mesh or folder / 'pipeline-keys-mesh.bin'
    b, (_, size) = read(keys, b'TOTKPKEY') if keys.is_file() else (None, (0, 0))
    if b:   # KeyRec: vs[32], fs[32], has_fs u32, ... (runtime/vk_pipeline.c)
        for o in range(16, len(b) - size + 1, size):
            ids.add(b[o:o + 32])
            if struct.unpack_from('<I', b, o + 64)[0]:
                ids.add(b[o + 32:o + 64])
    b, (_, size) = read(mesh, b'TOTKMKEY') if mesh.is_file() else (None, (0, 0))
    if b:   # MeshKeyRec: sh[3][32], has u32, ...
        for o in range(16, len(b) - size + 1, size):
            has = struct.unpack_from('<I', b, o + 96)[0]
            for i in range(3):
                if has & 1 << i:
                    ids.add(b[o + 32 * i:o + 32 * i + 32])
    p = cu or folder / 'compute-used.bin'
    if p.is_file():   # "TOTKCUSE", a version, then 32-byte ids
        c = p.read_bytes()
        if c[:2] == bytes([0x1f, 0x8b]):
            c = gzip.decompress(c)
        if c[:8] == b'TOTKCUSE':
            for o in range(12, len(c) - 31, 32):
                ids.add(c[o:o + 32])
    ids.discard(bytes(32))
    return ids


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    seen, out = set(), []
    if OUT.is_file():
        b, shape = read(OUT, b'TOTKSANS')
        if b and shape == (1, REC):
            for o in range(16, len(b) - REC + 1, REC):
                r = b[o:o + REC]
                seen.add(r[:36] + r[68:REC - 1]), out.append(r)
        print(f'{OUT}: {len(out)} records kept')
    data = ROOT / 'tools' / 'data'   # the release's key seeds name shaders too
    seeded = named_ids(data, data / 'pipeline-keys-oot.bin.gz', data / 'pipeline-keys-mesh-oot.bin.gz',
                       data / 'compute-used-oot.bin.gz')
    for folder in map(Path, sys.argv[1:]):
        ids = named_ids(folder) | seeded
        b, shape = read(folder / 'shader-answers.bin', b'TOTKSANS')
        if not b or shape != (1, REC):
            sys.exit(f'{folder}: no shader-answers.bin of the layout this tool reads')
        n = found = new = 0
        hit = set()
        for o in range(16, len(b) - REC + 1, REC):
            r = b[o:o + REC]
            n += 1
            if r[36:68] not in ids:
                continue
            found += 1
            hit.add(r[36:68])
            key = r[:36] + r[68:REC - 1]          # stage, program hash, answers
            if key in seen:
                continue
            seen.add(key)
            out.append(r[:36] + bytes(32) + r[68:REC - 1] + bytes([1]))
            new += 1
        print(f'{folder}: {len(ids)} input ids named by the key files, {len(hit)} of them in {n} answers records; '
              f'{found} records taken, {new} new')
    OUT.write_bytes(gzip.compress(b'TOTKSANS' + struct.pack('<II', 1, REC) + b''.join(out), 9, mtime=0))
    print(f'{OUT}: {len(out)} records, {OUT.stat().st_size / 1e3:.0f} KB')


if __name__ == '__main__':
    main()
