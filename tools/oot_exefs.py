"""Find the player's OoT NUP and take the game's ExeFS out of it.

The container holds the base game and its update as packages; the game runs the newest package's ExeFS (the update's
1.1.1 modules). The files are written as they are stored, for the lifter (recomp/gen.py), and with --plain a second
copy with the modules uncompressed, which the runtime's loader reads (tools/nso_plain.py). Prints one JSON object:
the container, the package and the SHA-256 of every file, which the setup compares with the hashes the code patches
were written for (tools/data/expected-oot.json). Everything written is game data: it goes where the caller says,
under build/ or the install folder.

    python tools/oot_exefs.py <file.nup or the folder that holds it> [--out <dir>] [--plain <dir>]
"""
import argparse
import hashlib
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools' / 'vendor' / 'nup'))
sys.path.insert(0, str(ROOT / 'tools'))
try:
    import zstandard  # noqa: E402,F401  the NUP reader imports it for compressed extents
except ImportError:   # the release's Python 3.14: its own compression.zstd behind the package's name (tools/py314)
    sys.path.insert(0, str(ROOT / 'tools' / 'py314'))
from nup import access  # noqa: E402
from nup.read import NupReader  # noqa: E402

TITLE = 0x0400486024DEC000   # OoT (Switch 2)


def find_nup(path):
    """The .nup a path names: the file itself, or the largest .nup in the folder or one level below it."""
    path = Path(path)
    if path.is_file():
        return path
    if not path.is_dir():
        return None
    found = sorted(list(path.glob('*.nup')) + list(path.glob('*/*.nup')), key=lambda p: p.stat().st_size)
    return found[-1] if found else None


def newest_exefs(reader):
    """The package whose ExeFS the game runs: the highest version that has one."""
    with_exefs = [p for p in reader.packages if p.exefs_dir >= 0 and list(access.iter_children(reader, p.exefs_dir))]
    return max(with_exefs, key=lambda p: p.version) if with_exefs else None


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('source', type=Path)
    ap.add_argument('--out', type=Path, help='the ExeFS as stored')
    ap.add_argument('--plain', type=Path, help='the ExeFS with uncompressed modules (needs --out)')
    a = ap.parse_args()
    nup = find_nup(a.source)
    if not nup:
        print(json.dumps({'error': 'no_nup', 'path': str(a.source)}))
        return 1
    try:
        reader = NupReader(str(nup))
    except Exception as e:   # not a NUP, or a damaged one
        print(json.dumps({'error': 'bad_nup', 'path': str(nup), 'detail': str(e)}))
        return 1
    with reader:
        pkg = newest_exefs(reader)
        if not pkg:
            print(json.dumps({'error': 'no_exefs', 'path': str(nup)}))
            return 1
        files = {}
        for child in access.iter_children(reader, pkg.exefs_dir):
            if child.is_dir:
                continue
            name = child.name.decode('utf-8', 'surrogateescape')
            data = reader.read_file(child.index)
            files[name] = hashlib.sha256(data).hexdigest()
            if a.out:
                a.out.mkdir(parents=True, exist_ok=True)
                (a.out / name).write_bytes(data)
        info = {'nup': str(nup), 'size': nup.stat().st_size, 'title_id': f'{pkg.title_id:016X}', 'kind': pkg.kind_name,
                'version': pkg.version_str, 'packages': len(reader.packages), 'files': files}
    if a.plain:
        if not a.out:
            ap.error('--plain needs --out')
        import nso_plain
        a.plain.mkdir(parents=True, exist_ok=True)
        for src in sorted(a.out.iterdir()):
            raw = src.read_bytes()
            (a.plain / src.name).write_bytes(nso_plain.plain(raw, src.name) if raw[:4] == b'NSO0' else raw)
    print(json.dumps(info))
    return 0


if __name__ == '__main__':
    sys.exit(main())
