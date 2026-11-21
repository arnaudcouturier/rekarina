"""Which files of a dump (.nup) are damaged, and where they lie in it. A file is damaged when its bytes differ from the
SHA-256 the .nup records for it (the copy was damaged on its way: a disk, a download, a cut-off copy), or when they
match it but its zstd data does not unpack (the dump was made from damaged data). The setup runs this after a step
that reads the dump failed on data it could not unpack (installer/oot.py dump_damage), so the player is told which
file is damaged instead of a Python traceback, and that the .nup must be copied (or dumped) again: the game reads the
same files while it runs, so a step cannot leave one out.

    python tools/nup_damage.py <file.nup> [--shaders | --all]

--shaders (the default): the files tools/oot_prepare_shaders.py inventory reads, their zstd archives and the zstd
members of those archives, as it unpacks them. --all: every file of the dump (its SHA-256, and the zstd data of
every .zs file); it reads the whole .nup, minutes on a hard disk.
Prints "damaged: <file> at byte <offset> of the .nup (<n> bytes there): <why>" for each, then a count. Exit code 0
when none is damaged, 3 when some are, 1 when the .nup cannot be read at all.
"""
import argparse
import hashlib
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import oot_romfs                    # noqa: E402

ZSTD = b'\x28\xb5\x2f\xfd'
DICTS = 'Pack/ZsDic.pack.zs'
# the files tools/oot_prepare_shaders.py inventory reads (its ROOTS and suffixes; that file is not imported: it is one
# of the shader step's inputs, and this tool runs only after it failed)
SHADER_ROOTS = ('Shader/', 'Layout2Shader/', 'Effect/', 'UI/', 'agl/', 'Lib/', 'System/', 'Foliage/', 'Pack/', 'Movie/')
SHADER_SUFFIXES = ('.bfsha.zs', '.bfsha', '.pck', '.bnsh')


def why_short(e):
    text = f'{type(e).__name__}: {e}'.replace('\n', ' ')
    return text[:200]


def members_unpack(fs, data, depth=0):
    """The zstd members of a SARC archive that do not unpack: [(member path, why)], as inventory visits them."""
    bad = []
    if data[:4] != b'SARC' or depth > 4:
        return bad
    try:
        members = oot_romfs.sarc_members(data)
    except Exception as e:   # (the archive's own table is damaged)
        return [('', 'its archive table cannot be read: ' + why_short(e))]
    for member, (lo, hi) in members.items():
        raw = data[lo:hi]
        inner = raw
        if raw[:4] == ZSTD:
            try:
                inner = fs.unzs(raw)
            except Exception as e:
                bad.append((member, 'its zstd data does not unpack: ' + why_short(e)))
                continue
        bad += [(member + ('/' + sub if sub else ''), why) for sub, why in members_unpack(fs, inner, depth + 1)]
    return bad


def check(nup, scope='shaders', out=print):
    """[(file path, offset in the .nup, stored size, why)] of the damaged files."""
    fs = oot_romfs.RomFS(str(nup))
    try:
        return check_files(fs, scope, out)
    finally:
        fs.r.close()


def check_files(fs, scope, out):
    names = sorted(fs.index)
    if scope == 'shaders':
        names = [n for n in names if n.startswith(SHADER_ROOTS) or n.endswith(SHADER_SUFFIXES)]
    damaged = []
    dicts_ok = True
    if DICTS in fs.index:
        try:
            fs.dicts()
        except Exception as e:   # every file packed with a dictionary fails then: name the dictionaries' file alone
            dicts_ok = False
            fi = fs.index[DICTS]
            damaged.append((DICTS, fs.r.files[fi][3], fs.r.files[fi][4], 'the dictionaries do not unpack: ' + why_short(e)))
    t0, last = time.monotonic(), time.monotonic()
    for i, name in enumerate(names):
        fi = fs.index[name]
        _n, size, sha, eoff, esz, _codec = fs.r.files[fi]
        why = None
        try:
            raw = fs.r.read_file(fi)
        except Exception as e:
            raw, why = None, 'it cannot be read from the .nup: ' + why_short(e)
        if raw is not None and hashlib.sha256(raw).digest() != sha:
            why = "its bytes differ from the SHA-256 the .nup records for it: the copy of the .nup is damaged"
        elif raw is not None and raw[:4] == ZSTD and name != DICTS and (dicts_ok or not zstd_dict(raw)):
            try:
                data = fs.unzs(raw)
            except Exception as e:
                why = ("its zstd data does not unpack, though its bytes match the .nup's SHA-256: the dump was made from "
                       "damaged data (" + why_short(e) + ")")
            else:
                for member, mwhy in members_unpack(fs, data):
                    damaged.append((f'{name}/{member}' if member else name, eoff, esz, mwhy))
        if why:
            damaged.append((name, eoff, esz, why))
        if time.monotonic() - last > 2:
            last = time.monotonic()
            out(f'nup_damage: checked {i + 1} of {len(names)} files, {len(damaged)} damaged', flush=True)
    out(f'nup_damage: checked {len(names)} files in {time.monotonic() - t0:.0f} s, {len(damaged)} damaged', flush=True)
    return damaged


def zstd_dict(raw):
    """Whether a zstd frame needs one of the game's dictionaries."""
    try:
        return oot_romfs.zstandard.get_frame_parameters(raw).dict_id != 0
    except Exception:
        return False


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('nup', type=Path)
    g = ap.add_mutually_exclusive_group()
    g.add_argument('--shaders', dest='scope', action='store_const', const='shaders')
    g.add_argument('--all', dest='scope', action='store_const', const='all')
    a = ap.parse_args()
    try:
        damaged = check(a.nup, a.scope or 'shaders', out=print)
    except Exception as e:
        print(f'nup_damage: the .nup cannot be read: {why_short(e)}', flush=True)
        return 1
    for name, offset, size, why in damaged:
        print(f'damaged: {name} at byte {offset} of the .nup ({size} bytes there): {why}', flush=True)
    return 3 if damaged else 0


if __name__ == '__main__':
    sys.exit(main())
