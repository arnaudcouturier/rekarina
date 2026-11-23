"""Build renhdrum.dll, RenHDrum's controller haptics library, from its source with a pinned Rust.

    python tools/build_renhdrum.py --src <RenHDrum 0.5.0 tree> [--opus <libopus 1.5.2 tree>] [--cache <folder>]
                                   [--toolchain <llvm-mingw folder>] [--out <dll>]
    python tools/build_renhdrum.py --check <dll>

RenHDrum (https://github.com/trzz1214/RenHDrum, tag v0.5.0, commit e778da6e169640e225a7f898b77cbb40af97c60f; Copyright
(c) 2026 Three-Dola, MIT OR Apache-2.0) turns the game's vibration values into what a DualSense's voice coils, an Xbox
pad's four motors or a Switch Pro Controller's HD rumble play. runtime/rumble_rh.c loads it from beside the game by its
full path; without it every controller keeps today's rumble. Its source stays outside this repository: --src names a
checkout of the tag (Cargo.toml, Cargo.lock, build.rs, src/, include/renhdrum.h), and SOURCE_TREE below must match it.
build.rs compiles libopus 1.5.2 (BSD-3-Clause) into the library from <src>/vendor/opus, or from --opus (the same
release with celt/, silk/, src/, include/ and COPYING; a copy's celt/opus_custom_demo.c, a program of its own, is left
out as upstream's copy has none).

The Rust is pinned (RUST_PARTS: rustc and cargo for x86_64-pc-windows-gnullvm, and that target's standard library,
from static.rust-lang.org) and so are the crates Cargo.lock names (the cc crate and the two it needs, build-time only):
each file is checked by its SHA-256, kept in --cache (default build/renhdrum-cache) and downloaded only when missing
there. Nothing is installed and nothing outside --cache and the output changes. The library is linked by llvm-mingw's
clang (--toolchain, as for tools/build_game.py; the release's llvm/ folder has it), which compiles libopus too, with
the C runtime linked in statically (crt-static): it needs only Windows' own libraries. No time stamp and no path of this
machine go into it.

--out defaults to build/oot/renhdrum.dll, where tools/make_rekarina_release.py and tools/package_oot.py take it from. A
renhdrum.json beside it records the versions and the SHA-256, and renhdrum-licenses/ beside it holds RenHDrum's
LICENSE-MIT, LICENSE-APACHE and NOTICE and libopus' COPYING, which the release carries in its licenses/. --check reads a DLL's headers only (nothing is loaded):
x86-64, every export runtime/rumble_rh.c binds (EXPORTS), and imports of Windows' own libraries only.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TRIPLE = 'x86_64-pc-windows-gnullvm'
RUST = '1.99.1'
DIST = 'https://static.rust-lang.org/dist/'
CRATES = 'https://static.crates.io/crates/'
RUST_PARTS = {   # name: SHA-256 (the .sha256 files beside them on static.rust-lang.org)
    f'rustc-{RUST}-{TRIPLE}': '00eb55f0c1fd330a0f7b82d288b32648b867bd523e55039d69c5914804dd7ad5',
    f'cargo-{RUST}-{TRIPLE}': '44753601667a1b201c9e2e4233d8311e2e7d3321273d6041a07e3d4e7966025b',
    f'rust-std-{RUST}-{TRIPLE}': '8ddc26705e13c46530c898524b0d2657b7d416a25e8d9ffe369a97b23a619bf6',
}
RENHDRUM = '0.5.0'
# SHA-256 over "<path>\0<sha256 of the file with CRLF read as LF>\n" for Cargo.toml, Cargo.lock, build.rs,
# include/renhdrum.h and every file under src/, sorted by path (source_tree)
SOURCE_TREE = '0a2213ac11a69110f1a5d0e86e19d8aeea6fa3959db9e5f6629e38fcf98fcd3e'
# The C ABI runtime/rumble_rh.c binds (RENHDRUM_VERSION 0x000500: every 0.5.x keeps it)
EXPORTS = tuple('renhdrum_' + n for n in (
    'version', 'set_nx2_mode', 'dualsense_dsp_create', 'dualsense_dsp_destroy', 'dualsense_dsp_set_half_target',
    'dualsense_dsp_fill_f32', 'dualsense_dsp_build_bt_report', 'dualsense_dsp_set_speaker',
    'dualsense_dsp_build_bt36_report', 'tri_trigger_create', 'tri_trigger_destroy', 'tri_trigger_update',
    'tri_trigger_note_action', 'hd_haptics_speaker_block', 'hd_haptics_trigger_block', 'xbox_rumble_create',
    'xbox_rumble_destroy', 'xbox_rumble_push_half_sample', 'xbox_rumble_tick', 'xbox_rumble_resend', 'engine_create',
    'engine_destroy', 'engine_push_half_sample', 'engine_tick'))
# Windows' own libraries, on every Windows 10 and 11
SYSTEM_DLL = re.compile(r'^(kernel32|ntdll|advapi32|bcrypt|bcryptprimitives|userenv|ws2_32|ucrtbase|'
                        r'api-ms-win-[a-z0-9-]+)\.dll$', re.I)
OPUS_SKIP = {'celt/opus_custom_demo.c'}


def say(text):
    print('build_renhdrum: ' + text, flush=True)


def stop(text):
    sys.exit('build_renhdrum: ' + text)


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def source_tree(src):
    files = [src / 'Cargo.toml', src / 'Cargo.lock', src / 'build.rs', src / 'include' / 'renhdrum.h']
    files += sorted(p for p in (src / 'src').rglob('*') if p.is_file())
    lines = []
    for p in files:
        if not p.is_file():
            stop(f'{p} is missing: --src must be a RenHDrum {RENHDRUM} tree')
        data = p.read_bytes().replace(b'\r\n', b'\n')
        lines.append(f'{p.relative_to(src).as_posix()}\0{hashlib.sha256(data).hexdigest()}\n')
    return hashlib.sha256(''.join(sorted(lines)).encode()).hexdigest()


def fetch(url, path, want):
    """path, downloaded from url when it is missing or not the pinned bytes; stops when the download is not them"""
    if path.is_file() and sha256(path) == want:
        return path
    say(f'downloading {url}')
    path.parent.mkdir(parents=True, exist_ok=True)
    part = path.with_name(path.name + '.part')
    req = urllib.request.Request(url, headers={'User-Agent': 'build-renhdrum'})
    with urllib.request.urlopen(req, timeout=300) as r, open(part, 'wb') as f:
        shutil.copyfileobj(r, f, 1 << 20)
    got = sha256(part)
    if got != want:
        part.unlink()
        stop(f'{url}: SHA-256 {got}, not the pinned {want}')
    os.replace(part, path)
    return path


def safe_extract(archive, into, mode):
    with tarfile.open(archive, mode) as t:
        t.extractall(into, filter='data')


def toolchain(cache):
    """The pinned Rust, unpacked once into cache/rust-<version> (each part's components merged as its install.sh
    would lay them out): its bin/ folder"""
    prefix = cache / f'rust-{RUST}'
    done = prefix / '.complete'
    key = ''.join(RUST_PARTS[n] for n in sorted(RUST_PARTS))
    if done.is_file() and done.read_text(encoding='utf-8') == key:
        return prefix / 'bin'
    shutil.rmtree(prefix, ignore_errors=True)
    prefix.mkdir(parents=True)
    for name, want in RUST_PARTS.items():
        archive = fetch(DIST + name + '.tar.xz', cache / (name + '.tar.xz'), want)
        say(f'unpacking {archive.name}')
        with tempfile.TemporaryDirectory(dir=cache) as tmp:
            safe_extract(archive, tmp, 'r:xz')
            top = Path(tmp) / name
            for comp in (top / 'components').read_text(encoding='utf-8').split():
                base = top / comp
                for p in base.rglob('*'):
                    rel = p.relative_to(base)
                    if p.is_dir() or rel.as_posix() == 'manifest.in':
                        continue
                    (prefix / rel).parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(p, prefix / rel)
    done.write_text(key, encoding='utf-8')
    return prefix / 'bin'


def locked_crates(lock):
    """[(name, version, sha256)] of Cargo.lock's crates.io packages"""
    out = []
    for block in lock.split('[[package]]')[1:]:
        f = dict(re.findall(r'^(name|version|source|checksum) = "([^"]*)"', block, re.M))
        if 'source' not in f:
            continue   # the crate itself
        if not f['source'].startswith('registry+') or not re.fullmatch(r'[0-9a-f]{64}', f.get('checksum', '')):
            stop(f"Cargo.lock's {f.get('name')} comes from {f['source']} or has no checksum")
        out.append((f['name'], f['version'], f['checksum']))
    return out


def vendor_crates(src, cache, into):
    """Cargo.lock's crates as a cargo directory source in into/ (each checked by Cargo.lock's SHA-256, which cargo
    compares again with --locked through the .cargo-checksum.json it reads)"""
    into.mkdir(parents=True)
    for name, version, want in locked_crates((src / 'Cargo.lock').read_text(encoding='utf-8')):
        crate = fetch(f'{CRATES}{name}/{name}-{version}.crate', cache / 'crates' / f'{name}-{version}.crate', want)
        safe_extract(crate, into, 'r:gz')
        (into / f'{name}-{version}' / '.cargo-checksum.json').write_text(
            json.dumps({'files': {}, 'package': want}), encoding='utf-8')


def copy_opus(opus, into):
    for sub in ('celt', 'silk', 'src', 'include'):
        if not (opus / sub).is_dir():
            stop(f'{opus / sub} is missing: --opus must be a libopus 1.5.2 tree')
        for p in (opus / sub).rglob('*'):
            rel = p.relative_to(opus).as_posix()
            if p.is_file() and rel not in OPUS_SKIP:
                (into / rel).parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(p, into / rel)
    if (opus / 'COPYING').is_file():
        shutil.copy2(opus / 'COPYING', into / 'COPYING')


# ---- the DLL's own headers (nothing is loaded) ----

def pe_facts(data):
    """(machine, imported DLL names, exported names) of a PE image; ValueError when it is not one"""
    if data[:2] != b'MZ':
        raise ValueError('no MZ header')
    pe = struct.unpack_from('<I', data, 0x3C)[0]
    if data[pe:pe + 4] != b'PE\0\0':
        raise ValueError('no PE header')
    machine, nsec = struct.unpack_from('<HH', data, pe + 4)
    opt_size = struct.unpack_from('<H', data, pe + 20)[0]
    opt = pe + 24
    if struct.unpack_from('<H', data, opt)[0] != 0x20B:
        raise ValueError('not a PE32+ image')
    dirs, sections = opt + 112, opt + opt_size

    def off(rva):
        for i in range(nsec):
            vsize, vaddr, rsize, rptr = struct.unpack_from('<IIII', data, sections + i * 40 + 8)
            if vaddr <= rva < vaddr + max(vsize, rsize):
                return rptr + rva - vaddr
        raise ValueError(f'RVA {rva:#x} in no section')

    def cstr(rva):
        o = off(rva)
        return data[o:data.index(b'\0', o)].decode('ascii', 'replace')

    exports, imports = [], []
    erva = struct.unpack_from('<I', data, dirs)[0]
    if erva:
        e = off(erva)
        count, names = struct.unpack_from('<I', data, e + 24)[0], struct.unpack_from('<I', data, e + 32)[0]
        exports = [cstr(struct.unpack_from('<I', data, off(names) + 4 * i)[0]) for i in range(count)]
    irva = struct.unpack_from('<I', data, dirs + 8)[0]
    if irva:
        o = off(irva)
        while data[o:o + 20] != b'\0' * 20:
            imports.append(cstr(struct.unpack_from('<I', data, o + 12)[0]))
            o += 20
    return machine, imports, exports


def problems(path):
    try:
        machine, imports, exports = pe_facts(Path(path).read_bytes())
    except (ValueError, struct.error, IndexError) as e:
        return [f'not a usable PE image ({e})'], []
    out = [] if machine == 0x8664 else [f'machine {machine:#x}, not x86-64']
    out += [f'no export {n}' for n in EXPORTS if n not in exports]
    out += [f'imports {i}, which is not part of Windows' for i in imports if not SYSTEM_DLL.match(i)]
    return out, imports


def check(path):
    bad, imports = problems(path)
    say(f'{path}: {Path(path).stat().st_size} bytes, sha256 {sha256(path)}; imports {", ".join(sorted(imports)) or "-"}')
    for p in bad:
        say('  ' + p)
    say('  usable' if not bad else '  not usable')
    return 1 if bad else 0


def build(a):
    src = a.src.resolve()
    tree = source_tree(src)
    if tree != SOURCE_TREE and not a.any_source:
        stop(f'{src} is not the pinned RenHDrum {RENHDRUM} source (tree {tree}); --any-source builds it anyway')
    opus = (a.opus or src / 'vendor' / 'opus').resolve()
    llvm = a.toolchain.resolve() / 'bin'
    clang, ar = llvm / 'x86_64-w64-mingw32-clang.exe', llvm / 'llvm-ar.exe'
    if not clang.is_file() or not ar.is_file():
        stop(f'{llvm} has no x86_64-w64-mingw32-clang.exe and llvm-ar.exe: --toolchain names an llvm-mingw folder')
    cache = (a.cache or ROOT / 'build' / 'renhdrum-cache').resolve()
    out = (a.out or ROOT / 'build' / 'oot' / 'renhdrum.dll').resolve()
    rust = toolchain(cache)
    work = cache / 'work'   # one fixed folder, emptied first: the same source gives the same bytes
    shutil.rmtree(work, ignore_errors=True)
    crate = work / 'renhdrum'
    crate.mkdir(parents=True)
    for f in ('Cargo.toml', 'Cargo.lock', 'build.rs'):
        shutil.copy2(src / f, crate / f)
    shutil.copytree(src / 'src', crate / 'src')
    copy_opus(opus, crate / 'vendor' / 'opus')
    vendor_crates(src, cache, work / 'crates')
    home = work / 'cargo-home'
    home.mkdir()
    (home / 'config.toml').write_text('[source.crates-io]\nreplace-with = "pinned"\n\n[source.pinned]\n'
                                      f'directory = "{(work / "crates").as_posix()}"\n', encoding='utf-8')
    under = TRIPLE.replace('-', '_')
    env = {k: v for k, v in os.environ.items()
           if k.upper() not in ('RUSTUP_TOOLCHAIN', 'CARGO_BUILD_TARGET', 'CARGO_ENCODED_RUSTFLAGS', 'RUSTFLAGS', 'CC',
                                'AR', 'CFLAGS', 'TARGET_CC', 'TARGET_AR', 'TARGET_CFLAGS', 'CRATE_CC_NO_DEFAULTS',
                                'CARGO_HOME', 'CARGO_TARGET_DIR')}
    env.update({
        'PATH': os.pathsep.join([str(rust), str(llvm), env.get('PATH', '')]),   # this process's children only
        'RUSTC': str(rust / 'rustc.exe'),
        'CARGO_HOME': str(home),
        'CARGO_TARGET_DIR': str(work / 'target'),
        f'CARGO_TARGET_{under.upper()}_LINKER': str(clang),
        'RUSTFLAGS': f'--remap-path-prefix={crate}=renhdrum -C target-feature=+crt-static '
                     '-C link-arg=-Wl,--no-insert-timestamp',
        f'CC_{under}': str(clang),
        f'AR_{under}': str(ar),
        f'CFLAGS_{under}': f'-ffile-prefix-map={work}=renhdrum',
    })
    cmd = [str(rust / 'cargo.exe'), 'build', '--release', '--locked', '--offline', '--lib', '--target', TRIPLE]
    say(f'cargo {" ".join(cmd[1:])} (Rust {RUST}, {clang})')
    if subprocess.run(cmd, cwd=crate, env=env).returncode:
        stop('cargo build failed')
    built = work / 'target' / TRIPLE / 'release' / 'renhdrum.dll'
    if not built.is_file():
        stop(f'cargo made no {built}')
    bad, _ = problems(built)
    if bad:
        stop('what was built is not usable: ' + '; '.join(bad))
    out.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(built, out)
    # its licences go with it (tools/make_rekarina_release.py puts them into the release's licenses/)
    lic = out.parent / 'renhdrum-licenses'
    shutil.rmtree(lic, ignore_errors=True)
    lic.mkdir()
    for name, src_file in (('renhdrum-LICENSE-MIT.txt', src / 'LICENSE-MIT'),
                           ('renhdrum-LICENSE-APACHE.txt', src / 'LICENSE-APACHE'), ('renhdrum-NOTICE.txt', src / 'NOTICE'),
                           ('renhdrum-opus-COPYING.txt', opus / 'COPYING')):
        if not src_file.is_file():
            stop(f'{src_file} is missing: the library goes nowhere without its licences')
        shutil.copyfile(src_file, lic / name)
    record = {'renhdrum': RENHDRUM, 'source_tree': tree, 'rust': RUST, 'target': TRIPLE, 'sha256': sha256(out),
              'size': out.stat().st_size}
    out.with_suffix('.json').write_text(json.dumps(record, indent=1) + '\n', encoding='utf-8')
    shutil.rmtree(work, ignore_errors=True)
    say(f'{out}: {record["size"]} bytes, sha256 {record["sha256"]}')
    return check(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--src', type=Path, help=f'the RenHDrum {RENHDRUM} source tree')
    ap.add_argument('--opus', type=Path, help='libopus 1.5.2 (default <src>/vendor/opus)')
    ap.add_argument('--cache', type=Path, help='where the Rust parts and crates are kept (default build/renhdrum-cache)')
    ap.add_argument('--toolchain', type=Path, help='the llvm-mingw folder whose clang links the library')
    ap.add_argument('--out', type=Path, help='the library (default build/oot/renhdrum.dll)')
    ap.add_argument('--any-source', action='store_true', help='build a source tree other than the pinned one')
    ap.add_argument('--check', type=Path, metavar='DLL', help="only check a built library's headers")
    a = ap.parse_args()
    if a.check:
        return check(a.check)
    if not a.src or not a.toolchain:
        ap.error('--src and --toolchain are needed to build')
    if os.name != 'nt':
        stop('builds the Windows library on Windows only')
    return build(a)


if __name__ == '__main__':
    sys.exit(main())
