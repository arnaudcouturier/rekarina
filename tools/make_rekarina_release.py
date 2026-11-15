"""Assemble the Rekarina setup folder: everything a player needs beside their own dump to build and start the game.

    python tools/make_rekarina_release.py --from <an unpacked TotK-Setup release> --out <folder>
                                          [--zip <file.zip>] [--single <Rekarina.exe>]
                                          [--dist <folder> [--site-url <address>] [--notes <file>] [--sign <key file>]]

The folder is what installer/oot.py calls a release (find_layout): its own Python, compiler and tools, so the machine
needs nothing installed. The player starts Rekarina-Launcher.exe; the game it builds is Rekarina.exe in the game
folder the player chooses. Its root holds Rekarina-Launcher.exe, LICENSE, THIRD_PARTY_NOTICES.md, VERSION and the
folders below, nothing else.

    Rekarina-Launcher.exe   installer/launcher/rekarina.c, built here with the folder's llvm: the launcher and setup
                            window (python\\pythonw.exe -m installer.gui_oot), and the swap of a downloaded update
    python/                 the bundled Python 3.14 with tkinter, numpy and fontTools  (from --from: tools/make_release.py
                            made it from python.org's embeddable package and pinned wheels)
    llvm/                   llvm-mingw's clang and lld                                 (from --from)
    bin/                    ninja.exe (from --from), rekarina_xlate.exe and its .id (build/xlate/totk_xlate.exe,
                            tools/build_xlate.py), rekarina_dlssg.dll (build/oot/totk_dlssg.dll: our own library
                            between the game and Streamline)
    runtime/                the runtime's sources as in this tree (the setup compiles them with the player's lifted code)
                            and deps/: headers, import libraries and DLLs of SDL3, Opus, libvpx, Vulkan and VMA
                            (toolchain/deps-mingw)
    recomp/, tools/, installer/, THIRD_PARTY_NOTICES.md, LICENSE, VERSION
    data/pgo-oot.profdata   the execution profile the exe is compiled with (build/pgo; counts per lifted function, no
                            game bytes), when there is one

On Linux (--from the folder tools/make_release_linux.py makes, which has python/bin/python3) the folder is the same
for that platform: python/ (python-build-standalone 3.14 with tkinter), llvm/ with llvm/sysroot (the Steam Runtime
sniper SDK files the game compiles and links against), bin/ninja, bin/rekarina_xlate (the release's own Linux build of
the translator) and bin/libSDL3.so.0, runtime/deps from that folder; the launcher is the AppImage entry, AppRun,
which starts python3 -m installer.gui_oot, with rekarina.desktop and rekarina.png beside it. --appimage packs the
folder (tools/make_release_linux.py write_appimage).

Nothing of the game is in it: tools/check_content.py runs over the folder at the end, as for the TotK
release. Nothing of NVIDIA's or AMD's either: the DLSS, Streamline and FidelityFX libraries are downloaded by the
setup's components step from their vendors (installer/components.py), and a copy that got into a source folder is
left out here (VENDOR_DLLS).

--dist writes what a release of the launcher's update (installer/update.py) expects into a folder:
Rekarina-Setup-<version>-windows.zip (the version is the VERSION file's) and SHA256SUMS with a line for it and for
every other file already in that folder (a --single program written there, for one). --sign <key file> adds
SHA256SUMS.sig (tools/release_sign.py: the launcher takes an update only with it once installer/update.py names the
public key). --site-url <address> adds latest.json, the record the launcher reads from the project's own site: the
folder's files are expected at <address>/<version>/<name> and the record itself at <address>/latest.json
(docs/distribution.md); --notes <file> is the release's text in it. The same files are the assets of a GitHub
release.

The Python zstd bindings: the bundled 3.14 has compression.zstd; tools/py314/zstandard.py stands in for the
zstandard package the tools and the vendored NUP reader import.
"""
import argparse
import fnmatch
import hashlib
import json
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import check_content  # noqa: E402
SKIP_DIRS = {'__pycache__', '.pytest_cache', '.git'}
SKIP_SUFFIXES = {'.pyc', '.orig', '.rej', '.tmp', '.obj', '.o'}
# NVIDIA's and AMD's libraries: theirs to distribute, not ours (installer/components.py downloads them at install)
VENDOR_DLLS = ('nvngx_*.dll', 'sl.*.dll', 'NvLowLatencyVk.dll', 'amd_fidelityfx*.dll')
ROOT_FILES = ('Rekarina-Launcher.exe', 'LICENSE', 'THIRD_PARTY_NOTICES.md', 'VERSION')
LINUX_ROOT_FILES = ('AppRun', 'rekarina.desktop', 'rekarina.png', '.DirIcon', 'LICENSE', 'THIRD_PARTY_NOTICES.md', 'VERSION')
APPRUN = '''#!/bin/sh
# The AppImage entry: Rekarina's launcher window, run by the bundled Python from the read-only mount.
HERE="$(dirname "$(readlink -f "$0")")"
export PYTHONDONTWRITEBYTECODE=1
export TCL_LIBRARY="$HERE/python/lib/tcl9.0"
export TK_LIBRARY="$HERE/python/lib/tk9.0"
exec "$HERE/python/bin/python3" -m installer.gui_oot "$@"
'''
DESKTOP = '''[Desktop Entry]
Type=Application
Name=Rekarina
Comment=Builds the game for this PC from your own game files and starts it
Exec=AppRun
Icon=rekarina
Categories=Game;
Terminal=false
'''


def linux_release(base, out, appimage=None):
    """The Linux folder (see the module text), from the folder tools/make_release_linux.py made. 0 when complete."""
    need = [base / 'python' / 'bin' / 'python3', base / 'llvm' / 'bin' / 'clang', base / 'llvm' / 'sysroot',
            base / 'bin' / 'ninja', base / 'bin' / 'totk_xlate', base / 'bin' / 'libSDL3.so.0', base / 'runtime' / 'deps' / 'include',
            ROOT / 'runtime' / 'rekarina.png']
    missing = [str(x) for x in need if not x.exists()]
    if missing:
        sys.exit('make_rekarina_release: missing: ' + ', '.join(missing))
    if out.exists():
        if not (out / 'VERSION').is_file() or not (out / 'installer' / 'gui_oot.py').is_file():
            sys.exit(f'make_rekarina_release: {out} exists and is not a Rekarina setup folder: not replaced')
        for x in out.iterdir():
            if x.name in ROOT_FOLDERS:
                shutil.rmtree(x)
    out.mkdir(parents=True, exist_ok=True)
    counts = {'python': copy_tree(base / 'python', out / 'python', whole=True),
              'llvm': copy_tree(base / 'llvm', out / 'llvm', whole=True)}
    (out / 'bin').mkdir(exist_ok=True)
    for src, dst in (('ninja', 'ninja'), ('totk_xlate', 'rekarina_xlate'), ('totk_xlate.id', 'rekarina_xlate.id'),
                     ('libSDL3.so.0', 'libSDL3.so.0')):
        if (base / 'bin' / src).is_file():
            shutil.copy2(base / 'bin' / src, out / 'bin' / dst)
    counts['runtime'] = copy_tree(ROOT / 'runtime', out / 'runtime')
    counts['deps'] = copy_tree(base / 'runtime' / 'deps', out / 'runtime' / 'deps', whole=True)
    counts['recomp'] = copy_tree(ROOT / 'recomp', out / 'recomp')
    counts['tools'] = copy_tree(ROOT / 'tools', out / 'tools', skip=('xlate',))
    counts['installer'] = copy_tree(ROOT / 'installer', out / 'installer')
    if (base / 'licenses').is_dir():
        counts['licenses'] = copy_tree(base / 'licenses', out / 'licenses')
    for name in ('THIRD_PARTY_NOTICES.md', 'LICENSE', 'VERSION'):
        if (ROOT / name).is_file():
            shutil.copy2(ROOT / name, out / name)
    pgo = ROOT / 'build' / 'pgo' / 'pgo-oot.profdata'
    if pgo.is_file():
        (out / 'data').mkdir(exist_ok=True)
        shutil.copy2(pgo, out / 'data' / pgo.name)
    (out / 'AppRun').write_text(APPRUN, encoding='utf-8')
    (out / 'AppRun').chmod(0o755)
    (out / 'rekarina.desktop').write_text(DESKTOP, encoding='utf-8')
    shutil.copy2(ROOT / 'runtime' / 'rekarina.png', out / 'rekarina.png')
    if not (out / '.DirIcon').exists():
        (out / '.DirIcon').symlink_to('rekarina.png')
    stray = sorted(x.name for x in out.iterdir() if x.name not in LINUX_ROOT_FILES + ROOT_FOLDERS and not players(x.name))
    bad = []
    for f in sorted(out.rglob('*')):
        if f.is_file() and not f.is_symlink() and not players(f.relative_to(out).parts[0]):
            bad += [f'{f.relative_to(out)}: {problem}' for problem in check_content.problems_for(f, allow_large=True)]
    size = sum(f.stat().st_size for f in out.rglob('*') if f.is_file() and not f.is_symlink())
    print(f'make_rekarina_release: {out}: {size / 2**20:.0f} MiB; files: ' + ', '.join(f'{k} {v}' for k, v in counts.items()))
    print('make_rekarina_release: content check: ' + ('no hit' if not bad else f'{len(bad)} hits: ' + '; '.join(bad[:20])))
    if stray:
        print('make_rekarina_release: not part of a release, in its root: ' + ', '.join(stray))
    if appimage and not bad:
        import make_release_linux
        make_release_linux.write_appimage(out, appimage)
    return 1 if bad or stray else 0
ROOT_FOLDERS = ('python', 'llvm', 'bin', 'runtime', 'recomp', 'tools', 'installer', 'data', 'licenses')
# what a player's use adds to the folder: never packed, never counted as a stray
PLAYER = ('temp', 'update-staging')


def players(name):
    return name in PLAYER or name.startswith('update-old')


def write_zip(out, path):
    path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for f in sorted(out.rglob('*')):
            if f.is_file() and not players(f.relative_to(out).parts[0]):
                z.write(f, Path(out.name) / f.relative_to(out))
    print(f'make_rekarina_release: {path}: {path.stat().st_size / 2**20:.0f} MiB')


def copy_tree(src, dst, skip=(), whole=False):
    """whole: a third-party tree, copied as it is (a compiler's own start-up objects are .o files)."""
    n = 0
    for p in sorted(src.rglob('*')):
        rel = p.relative_to(src)
        if not whole and (any(part in SKIP_DIRS for part in rel.parts) or p.suffix in SKIP_SUFFIXES or rel.parts[0] in skip):
            continue
        if p.is_dir():
            (dst / rel).mkdir(parents=True, exist_ok=True)
        else:
            (dst / rel).parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(p, dst / rel)
            n += 1
    return n


def write_single(out, path, root):
    """Rekarina as one file (installer/launcher/rekarina_single.c): that program, compiled with the folder's own
    compiler and the launcher's icon, then the folder as a zip (its files at the zip's root), then the trailer the
    program reads: "RKSFX001", the zip's offset and size, the version in 32 bytes."""
    llvm = out / 'llvm' / 'bin'
    path.parent.mkdir(parents=True, exist_ok=True)
    rc, stub, payload = path.with_suffix('.rc.o'), path.with_suffix('.stub.exe'), path.with_suffix('.payload.zip')
    subprocess.run([str(llvm / 'llvm-windres.exe'), '-I', str(root / 'runtime'), '-O', 'coff',
                    str(root / 'installer' / 'launcher' / 'rekarina.rc'), '-o', str(rc)], check=True)
    subprocess.run([str(llvm / 'clang.exe'), '-O2', '-mwindows', '-municode',
                    str(root / 'installer' / 'launcher' / 'rekarina_single.c'), str(rc), '-o', str(stub)], check=True)
    with zipfile.ZipFile(payload, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for f in sorted(out.rglob('*')):
            if f.is_file() and not players(f.relative_to(out).parts[0]):
                z.write(f, f.relative_to(out))
    version = (out / 'VERSION').read_text(encoding='utf-8').strip().encode('ascii')
    assert 0 < len(version) < 32, version
    program = stub.read_bytes()
    with open(path, 'wb') as f, open(payload, 'rb') as z:
        f.write(program)
        shutil.copyfileobj(z, f, 1 << 20)
        f.write(b'RKSFX001' + len(program).to_bytes(8, 'little') + payload.stat().st_size.to_bytes(8, 'little')
                + version.ljust(32, bytes(1)))
    for scrap in (rc, stub, payload):
        scrap.unlink()
    print(f'make_rekarina_release: {path}: {path.stat().st_size / 2**20:.0f} MiB, one file')


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        while block := f.read(1 << 20):
            h.update(block)
    return h.hexdigest()


DIST_OWN = ('SHA256SUMS', 'SHA256SUMS.sig', 'latest.json')


def write_dist(out, dist, site_url='', notes=None, sign=None):
    """The release's files in dist (the module text, --dist). Returns the names it wrote."""
    version = (out / 'VERSION').read_text(encoding='utf-8').strip()
    archive = dist / f'Rekarina-Setup-{version}-windows.zip'
    write_zip(out, archive)
    files = sorted(f for f in dist.iterdir() if f.is_file() and f.name not in DIST_OWN and not f.name.endswith('.part'))
    sums = {f.name: sha256_of(f) for f in files}
    (dist / 'SHA256SUMS').write_bytes(''.join(f'{digest}  {name}\n' for name, digest in sums.items()).encode())
    print(f'make_rekarina_release: {dist / "SHA256SUMS"}: ' + ', '.join(f'{name} {digest[:12]}...' for name, digest in sums.items()))
    wrote = [f.name for f in files] + ['SHA256SUMS']
    (dist / 'SHA256SUMS.sig').unlink(missing_ok=True)   # never an older file's signature beside the new sums
    if sign:
        import release_sign
        print(f'make_rekarina_release: {release_sign.sign_file(sign, dist / "SHA256SUMS")}')
        wrote.append('SHA256SUMS.sig')
    (dist / 'latest.json').unlink(missing_ok=True)
    if site_url:
        base = site_url.rstrip('/')
        assets = [{'name': f.name, 'browser_download_url': f'{base}/{version}/{f.name}', 'size': f.stat().st_size,
                   'digest': 'sha256:' + sums[f.name]} for f in files]
        assets += [{'name': name, 'browser_download_url': f'{base}/{version}/{name}', 'size': (dist / name).stat().st_size}
                   for name in wrote if name.startswith('SHA256SUMS')]
        record = {'tag_name': 'v' + version, 'name': f'Rekarina {version}', 'html_url': f'{base}/{version}/',
                  'body': notes.read_text(encoding='utf-8') if notes else '', 'assets': assets}
        with open(dist / 'latest.json', 'w', encoding='utf-8', newline='\n') as f:
            json.dump(record, f, indent=1, ensure_ascii=False)
            f.write('\n')
        print(f'make_rekarina_release: {dist / "latest.json"}: the record for {base}/latest.json')
        wrote.append('latest.json')
    return wrote


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--from', dest='base', type=Path, required=True,
                    help='an unpacked TotK-Setup release: its python/, llvm/ and bin/ninja.exe are taken')
    ap.add_argument('--out', type=Path, required=True, help='the folder to make (replaced when it holds a VERSION of ours)')
    ap.add_argument('--zip', type=Path, help='also write the folder as this zip')
    ap.add_argument('--appimage', type=Path, help='Linux: also pack the folder as this AppImage')
    ap.add_argument('--dist', type=Path, help='also write Rekarina-Setup-<version>-windows.zip and SHA256SUMS into this '
                                              'folder: the assets of a GitHub release')
    ap.add_argument('--single', type=Path, help='also write the folder as one program, e.g. Rekarina.exe: at its first '
                                                'start it unpacks the folder into the local application data and '
                                                'starts the launcher')
    ap.add_argument('--only-single', action='store_true', help='write --single from the folder --out already is')
    ap.add_argument('--site-url', default='', help='with --dist: also write latest.json for the site, whose release '
                                                   'files are served under this address (https://<site>/releases)')
    ap.add_argument('--notes', type=Path, help='with --site-url: a text file with the release notes for latest.json')
    ap.add_argument('--sign', type=Path, help='with --dist: the private key file (tools/release_sign.py keygen) that '
                                              'signs SHA256SUMS')
    a = ap.parse_args()
    base, out = a.base.resolve(), a.out.resolve()
    if (base / 'python' / 'bin' / 'python3').is_file():   # the Linux toolchain folder
        return linux_release(base, out, a.appimage.resolve() if a.appimage else None)
    if a.only_single:
        if not a.single or not (out / 'Rekarina-Launcher.exe').is_file():
            sys.exit('--only-single needs --single and a finished --out folder')
        write_single(out, a.single.resolve(), ROOT)
        return
    need = [base / 'python' / 'python.exe', base / 'python' / 'pythonw.exe', base / 'llvm' / 'bin' / 'clang.exe',
            base / 'bin' / 'ninja.exe', ROOT / 'build' / 'xlate' / 'totk_xlate.exe', ROOT / 'toolchain' / 'deps-mingw' / 'include',
            ROOT / 'runtime' / 'rekarina.ico', ROOT / 'installer' / 'launcher' / 'rekarina.c']
    missing = [str(p) for p in need if not p.exists()]
    if missing:
        sys.exit('make_rekarina_release: missing: ' + ', '.join(missing))
    if out.exists():
        if not (out / 'VERSION').is_file() or not (out / 'installer' / 'gui_oot.py').is_file():
            sys.exit(f'make_rekarina_release: {out} exists and is not a Rekarina setup folder: not replaced')
        for p in out.iterdir():   # the player's temp and anything else we did not put there stay
            if p.name in ROOT_FOLDERS:
                shutil.rmtree(p)
    out.mkdir(parents=True, exist_ok=True)

    counts = {}
    counts['python'] = copy_tree(base / 'python', out / 'python', whole=True)
    counts['llvm'] = copy_tree(base / 'llvm', out / 'llvm', whole=True)
    (out / 'bin').mkdir(exist_ok=True)
    shutil.copy2(base / 'bin' / 'ninja.exe', out / 'bin' / 'ninja.exe')
    xl = ROOT / 'build' / 'xlate' / 'totk_xlate.exe'   # the build's names; the release carries Rekarina's
    shutil.copy2(xl, out / 'bin' / 'rekarina_xlate.exe')
    if xl.with_suffix('.id').is_file():   # the translator identity is the file's content, whatever its name
        shutil.copy2(xl.with_suffix('.id'), out / 'bin' / 'rekarina_xlate.id')
    shim = ROOT / 'build' / 'oot' / 'totk_dlssg.dll'
    if shim.is_file():
        shutil.copy2(shim, out / 'bin' / 'rekarina_dlssg.dll')
    else:
        print(f'make_rekarina_release: no {shim}: the release goes without DLSS Frame Generation')
    counts['runtime'] = copy_tree(ROOT / 'runtime', out / 'runtime')
    counts['deps'] = copy_tree(ROOT / 'toolchain' / 'deps-mingw', out / 'runtime' / 'deps', whole=True)
    counts['recomp'] = copy_tree(ROOT / 'recomp', out / 'recomp')
    counts['tools'] = copy_tree(ROOT / 'tools', out / 'tools', skip=('xlate',))   # the translator's sources: bin/ has it built
    counts['installer'] = copy_tree(ROOT / 'installer', out / 'installer')
    if (base / 'licenses').is_dir():
        counts['licenses'] = copy_tree(base / 'licenses', out / 'licenses')
    for name in ('THIRD_PARTY_NOTICES.md', 'LICENSE', 'VERSION'):
        if (ROOT / name).is_file():
            shutil.copy2(ROOT / name, out / name)
    pgo = ROOT / 'build' / 'pgo' / 'pgo-oot.profdata'
    if pgo.is_file():
        (out / 'data').mkdir(exist_ok=True)
        shutil.copy2(pgo, out / 'data' / pgo.name)

    # the launcher, with the folder's own compiler
    llvm = out / 'llvm' / 'bin'
    rc = out / 'installer' / 'launcher' / 'rekarina_rc.o'
    subprocess.run([str(llvm / 'llvm-windres.exe'), '-I', str(ROOT / 'runtime'), '-O', 'coff',
                    str(ROOT / 'installer' / 'launcher' / 'rekarina.rc'), '-o', str(rc)], check=True)
    subprocess.run([str(llvm / 'clang.exe'), '-O2', '-mwindows', '-municode', str(ROOT / 'installer' / 'launcher' / 'rekarina.c'),
                    str(rc), '-o', str(out / 'Rekarina-Launcher.exe')], check=True)
    rc.unlink()

    # NVIDIA's and AMD's libraries stay out, wherever a source folder had them
    vendor = [f for f in sorted(out.rglob('*.dll')) if not players(f.relative_to(out).parts[0])
              and any(fnmatch.fnmatch(f.name.lower(), pat.lower()) for pat in VENDOR_DLLS)]
    for f in vendor:
        f.unlink()
    if vendor:
        print('make_rekarina_release: left out: ' + ', '.join(str(f.relative_to(out)) for f in vendor))
    stray = sorted(p.name for p in out.iterdir() if p.name not in ROOT_FILES + ROOT_FOLDERS and not players(p.name))
    lacking = sorted(n for n in ROOT_FILES if not (out / n).is_file())
    if stray:
        print('make_rekarina_release: not part of a release, in its root: ' + ', '.join(stray))
    if lacking:
        print('make_rekarina_release: the root lacks: ' + ', '.join(lacking))

    # no game content in the folder (the rule of tools/check_content.py; toolchain binaries are over its size limit)
    bad = []
    for f in sorted(out.rglob('*')):
        if f.is_file() and not players(f.relative_to(out).parts[0]):
            bad += [f'{f.relative_to(out)}: {problem}' for problem in check_content.problems_for(f, allow_large=True)]
    size = sum(f.stat().st_size for f in out.rglob('*') if f.is_file())
    print(f'make_rekarina_release: {out}: {size / 2**20:.0f} MiB; files: ' + ', '.join(f'{k} {v}' for k, v in counts.items()))
    print('make_rekarina_release: content check: ' + ('no hit' if not bad else f'{len(bad)} hits: ' + '; '.join(bad[:20])))
    if a.zip:
        write_zip(out, a.zip)
    if a.single:
        write_single(out, a.single.resolve(), ROOT)
    if a.dist:
        write_dist(out, a.dist.resolve(), a.site_url, a.notes, a.sign)
    return 1 if bad or stray or lacking else 0


if __name__ == '__main__':
    sys.exit(main())
