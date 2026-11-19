"""Rekarina setup: OoT (Switch 2) from the player's own dump to a folder to play from.

    python -m installer.oot --game <file.nup or its folder> --dest <install folder> [--lang ru] [--prompts auto]
                            [--toolchain DIR] [--temp DIR] [--root DIR] [--dlls DIR]

The player names the folder with their dump; the setup finds the NUP container in it, checks that it is the version
the code patches were written for (the SHA-256 of every ExeFS file, tools/data/expected-oot.json), and then does what
the tools do by hand, as steps under the same rule as the TotK setup (runner.py: a step whose inputs
did not change since it was recorded is skipped, so a second run continues or updates):

    oot_check     the dump, the tools, the disk space
    oot_extract   the ExeFS out of the container (tools/oot_exefs.py): as stored for the lifter, uncompressed for the game
    oot_lift      the game code to C (recomp/gen.py with recomp/patches-oot-v111.txt)
    oot_compile   the C and the runtime to totk.exe (tools/build_game.py --version oot)
    oot_components  the upscaler libraries this machine's graphics card can use (DLSS, frame generation, FSR 3.1),
                  downloaded from NVIDIA's and AMD's own repositories (components.py); a failure only warns
    oot_prompts   the button prompt overlays and fonts (a minute)
    oot_prepare   the shaders translated and built before play: the variants play met, or with --all-shaders every one
    oot_package   the folder to play from (tools/package_oot.py): Rekarina.exe, its libraries, caches and rekarina.ini,
                  a new rekarina.ini with first settings for the graphics card (components.preset)

The RomFS stays in the player's NUP: the game reads it in place, so the dump must stay where it is. Prints JSON events
on stdout like installer/cli.py (events.py), and after "start" one "eta" event with the time each step should take
(eta.py); the window (gui_oot.py) reads them. Exit code: 0 done, 1 failed, 2 bad command line, 3 cancelled.

Where the tools come from: a release folder brings its own llvm, ninja and libraries (layout.py). In a checkout the
compiler is the one --toolchain names (a folder with llvm/bin/clang and bin/ninja, such as an unpacked TotK-Setup
release), else REKARINA_TOOLCHAIN, else toolchain/llvm-mingw-* of the checkout, else the newest TotK-Setup release
found beside the checkout's parent folder.
"""
import argparse
import hashlib
import io
import json
import os
import platform
import re
import shutil
import signal
import subprocess
import sys
import time
import traceback
import urllib.request
import zipfile
from dataclasses import dataclass
from pathlib import Path

from . import components, eta, mods, net
from . import state as statefile
from .events import Reporter
from .runner import Context, InstallError, Step, run_steps

EXIT = {'ok': 0, 'failed': 1, 'cancelled': 3}
WIN = os.name == 'nt'
EXE = '.exe' if WIN else ''
GAME_EXE = 'Rekarina' + EXE
INI = 'rekarina.ini'      # the settings file beside the game
OLD_INI = 'totk.ini'      # its name in a folder made before 1.0.0: the game and package_oot.py rename it
SHIM = 'rekarina_dlssg.dll'   # our library between the game and Streamline, as the game folder and a release name it
STYLES = (('switch', 'switch'), ('xbox', 'xbox'), ('xbox-label', 'xbox-label'))   # shared-fonts-<folder>, --style
# Libraries the game loads when they lie beside it (DLSS, FSR 3.1, frame generation). They are not ours to ship: the
# components step downloads them from their vendors; --dlls names a folder with the player's own copies, which then
# take the place of the downloaded ones.
OPTIONAL_DLLS = ('nvngx_dlss.dll', 'amd_fidelityfx_vk.dll', 'amd_fidelityfx_loader_vk.dll', 'amd_fidelityfx_upscaler_vk.dll',
                 'amd_fidelityfx_framegeneration_vk.dll', SHIM, 'totk_dlssg.dll', 'sl.interposer.dll', 'sl.common.dll',
                 'sl.dlss_g.dll', 'sl.reflex.dll', 'sl.pcl.dll', 'nvngx_dlssg.dll', 'NvLowLatencyVk.dll')
NEED_DEST = 14 << 30   # the exe, the shader and driver caches, the overlays
NEED_TEMP = 9 << 30    # the generated C, the objects, the build's copy of the caches

# The helpers below repeat installer/steps.py's (file_sha256, fingerprint, kill_tree, run_tool, free space): that module
# imports the Switch 1 extraction tools and their libraries, which this setup does not need.
NINJA_LINE = re.compile(r'^\[(\d+)/(\d+)\] ')
TRANSLATED = re.compile(r'gfx_shader: prepare: (\d+) of (\d+) pairs')   # totk.exe --prepare-shaders
LIBRARIES = re.compile(r'prebuild preparation (?:progress|done): (\d+) of (\d+) variants')   # its driver cache phase
RUN_SCRIPT = ('import os, runpy, sys; p = sys.argv[1]; sys.path.insert(0, os.path.dirname(p)); sys.argv = sys.argv[1:]; '
              "runpy.run_path(p, run_name='__main__')")


def file_sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb', buffering=0) as f:
        buf = bytearray(8 << 20)
        view = memoryview(buf)
        while n := f.readinto(buf):
            h.update(view[:n])
    return h.hexdigest()


def fingerprint(*parts):
    """SHA-256 over strings and the contents of files (a missing file counts as missing). A (name, file) pair counts
    the file under that name: a file that was renamed keeps the fingerprints made under its old name."""
    h = hashlib.sha256()
    for p in parts:
        name = None
        if isinstance(p, tuple):
            name, p = p
        if isinstance(p, Path):
            h.update((name or p.name).encode() + b'\0' + (bytes.fromhex(file_sha256(p)) if p.is_file() else b'missing'))
        else:
            h.update(str(p).encode())
        h.update(b'\1')
    return h.hexdigest()


WORK_IN_DEST = '.setup-work'   # in the game folder: the working files of a setup whose own disk has no room for them


def work_folder(default, asked, dest):
    """(the folder of this game folder's working files, the free GiB of the setup's disk when that sent them into the
    game folder, else None). The files (the generated C, the objects, the build's caches: NEED_TEMP) live with the
    setup, in its temp folder, one sub-folder per game folder; --temp names another place. A setup that was unpacked on
    a disk without room for them (the single-file Rekarina.exe unpacks into the user's application data, on the system
    disk) puts them into the game folder instead, when that is on another disk with room for both: the game folder is
    the one place the player chose. Later runs go where the objects of the earlier one are, wherever the free space
    is by then, so an update compiles only what changed."""
    tag = f'oot-{hashlib.sha256(str(dest).lower().encode()).hexdigest()[:8]}'
    if asked:
        return Path(asked).resolve() / tag, None
    base = Path(default).resolve() if default else dest / WORK_IN_DEST
    here, there = base / tag, dest / WORK_IN_DEST / tag
    if here == there or (here / 'build' / 'obj').is_dir():
        return here, None
    if (there / 'build' / 'obj').is_dir():
        return there, None
    try:
        drive = lambda p: os.path.splitdrive(str(p))[0].lower()
        free, room = free_bytes(here), free_bytes(dest)
        if drive(here) != drive(dest.resolve()) and free < NEED_TEMP + (1 << 30) and room >= NEED_DEST + NEED_TEMP:
            return there, round(free / 2**30, 1)
    except OSError:
        pass
    return here, None


def free_bytes(path):
    p = Path(path)
    while not p.exists():
        p = p.parent
    return shutil.disk_usage(p).free


def kill_tree(proc):
    if proc.poll() is not None:
        return
    if WIN:
        subprocess.run(['taskkill', '/T', '/F', '/PID', str(proc.pid)], capture_output=True)
        return
    try:
        os.killpg(proc.pid, signal.SIGKILL)
    except OSError:
        proc.kill()


def run_tool(ctx, step, script, args, parse=None):
    """Run a tool script with the setup's Python. Its output goes to <temp>/logs/<step>-<script>.log and, line by line,
    to parse(line). Returns (exit code, log path)."""
    logs = ctx.temp / 'logs'
    logs.mkdir(parents=True, exist_ok=True)
    log = logs / f'{step}-{Path(script).stem}.log'
    command = [str(ctx.layout.python), '-c', RUN_SCRIPT, str(script), *map(str, args)]
    ctx.reporter.log('run ' + ' '.join(command[3:]))
    env = dict(os.environ, PYTHONUNBUFFERED='1', PYTHONIOENCODING='utf-8')
    with open(log, 'w', encoding='utf-8', newline='\n') as out:
        proc = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, cwd=ctx.layout.root, env=env,
                                text=True, encoding='utf-8', errors='replace', bufsize=1, start_new_session=not WIN)
        try:
            for line in proc.stdout:
                out.write(line)
                if parse:
                    parse(line.rstrip())
            code = proc.wait()
        except BaseException:
            kill_tree(proc)
            raise
    ctx.reporter.log(f'{Path(script).name}: exit code {code}; output in {log}')
    return code, log



def cpu_level():
    """The level the game is compiled for on this machine (tools/build_game.py --cpu): 'v3', the default, needs AVX2
    and FMA; a processor without AVX2 gets 'v2' (SSE4.2), slower, where the default build would stop at its first
    AVX2 instruction. REKARINA_CPU names one for a test."""
    forced = os.environ.get('REKARINA_CPU')
    if forced in ('v2', 'v3'):
        return forced
    try:
        if os.name == 'nt':
            import ctypes
            return 'v3' if ctypes.windll.kernel32.IsProcessorFeaturePresent(40) else 'v2'   # PF_AVX2_INSTRUCTIONS_AVAILABLE
        with open('/proc/cpuinfo', encoding='ascii', errors='replace') as f:
            return 'v3' if ' avx2' in f.read() else 'v2'
    except (OSError, AttributeError):
        return 'v3'


@dataclass(frozen=True)
class OotLayout:
    root: Path
    dev: bool
    release: str
    python: Path
    tools: Path
    recomp: Path
    llvm: Path           # the tree with bin/clang
    ninja: Path
    deps: Path
    xlate: Path
    temp: Path
    pgo: Path | None     # the execution profile the runtime is compiled with, when there is one
    # A Linux release: the Steam Runtime sysroot the game compiles and links against (llvm/sysroot, as the TotK setup's
    # installer/layout.py), and the libraries the game finds beside itself (bin/libSDL3.so.0); a checkout uses the host's
    sysroot: Path | None = None
    libs: tuple = ()

    @property
    def runtime(self):   # the icons the window shows
        return self.root / 'runtime'

    @property
    def shim(self):
        """Our own library between the game and Streamline (runtime/dlssg): a release carries it in bin/ as
        rekarina_dlssg.dll, a checkout has it where its build put it, as totk_dlssg.dll."""
        return self.root / 'build' / 'oot' / 'totk_dlssg.dll' if self.dev else self.root / 'bin' / SHIM

    @property
    def patches(self):
        return self.recomp / 'patches-oot-v111.txt'

    @property
    def expected(self):
        return self.tools / 'data' / 'expected-oot.json'

    def missing(self):
        need = [self.python, self.tools / 'oot_exefs.py', self.tools / 'build_game.py', self.tools / 'oot_prepare_shaders.py',
                self.tools / 'make_oot_prompts.py', self.tools / 'make_ext_font.py', self.tools / 'package_oot.py',
                self.recomp / 'gen.py', self.patches, self.expected, self.llvm / 'bin' / ('clang' + EXE), self.ninja,
                self.deps / 'include', self.deps / 'lib', self.xlate, *([self.sysroot] if self.sysroot else []), *self.libs]
        return [str(p) for p in need if not p.exists()]


def toolchain_root(root, named):
    """The folder that holds llvm/ and bin/ninja (see the module text)."""
    for cand in (named, os.environ.get('REKARINA_TOOLCHAIN')):
        if cand:
            return Path(cand)
    trees = sorted(p.parent.parent for p in (root / 'toolchain').glob('llvm-mingw-*/llvm-mingw-*/bin/clang' + EXE))
    if trees:
        return trees[-1].parent
    setups = sorted(root.parent.parent.glob('TotK-Setup-*/release/TotK-Setup-*'))
    return setups[-1] if setups else root / 'toolchain' / 'missing'


def find_layout(root=None, toolchain=None, temp=None):
    root = Path(root or Path(__file__).resolve().parent.parent).resolve()
    version = (root / 'VERSION').read_text(encoding='utf-8').strip() if (root / 'VERSION').is_file() else 'unknown'
    bundled = root / 'python' / ('python.exe' if WIN else 'bin/python3')
    if bundled.is_file():   # a release folder
        return OotLayout(root=root, dev=False, release=version, python=bundled, tools=root / 'tools', recomp=root / 'recomp',
                         llvm=root / 'llvm', ninja=root / 'bin' / ('ninja' + EXE), deps=root / 'runtime' / 'deps',
                         xlate=root / 'bin' / ('rekarina_xlate' + EXE), temp=Path(temp) if temp else root / 'temp',
                         pgo=root / 'data' / 'pgo-oot.profdata',
                         sysroot=None if WIN else root / 'llvm' / 'sysroot',
                         libs=() if WIN else (root / 'bin' / 'libSDL3.so.0',))
    tc = toolchain_root(root, toolchain)
    llvm = tc / 'llvm' if (tc / 'llvm').is_dir() else tc
    ninja = tc / 'bin' / ('ninja' + EXE)
    return OotLayout(root=root, dev=True, release=f'{version}-dev', python=Path(sys.executable), tools=root / 'tools',
                     recomp=root / 'recomp', llvm=llvm, ninja=ninja if ninja.is_file() else llvm / 'bin' / ('ninja' + EXE),
                     deps=root / 'toolchain' / 'deps-mingw', xlate=root / 'build' / 'xlate' / ('totk_xlate' + EXE),
                     temp=Path(temp) if temp else root / 'build' / 'install-temp', pgo=root / 'build' / 'pgo' / 'pgo-oot.profdata')


# The game's main module is stored as ZBIC (the platform's zstd variant, system 22.0.0 on), which the tools read with the
# zbic package (recomp/nso.py). It is GPL software, so no release folder carries it: a Python that lacks it gets the
# wheel for its version from the Python Package Index, checked against the index's own SHA-256, as a player's own
# download (like the upscaler libraries). A source checkout installs it with its other requirements.
ZBIC_INDEX = 'https://pypi.org/pypi/zbic/json'


def ensure_zbic(ctx, step):
    python = str(ctx.layout.python)
    quiet = dict(capture_output=True, text=True, creationflags=subprocess.CREATE_NO_WINDOW if WIN else 0)
    if subprocess.run([python, '-c', 'import zbic'], **quiet).returncode == 0:
        return
    ctx.reporter.progress(step, 'zbic', force=True)
    probe = subprocess.run([python, '-c', 'import sys, sysconfig; print(f"cp{sys.version_info[0]}{sys.version_info[1]}"); '
                            'print(sysconfig.get_paths()["purelib"])'], **quiet)
    lines = probe.stdout.splitlines()   # one per line: the folder may hold spaces
    try:
        # the wheel of this platform: win_amd64, or on x86-64 Linux the manylinux one (glibc 2.17, older than the
        # setup's own floor of 2.34)
        plat = 'win_amd64' if WIN else 'manylinux' if sys.platform.startswith('linux') and platform.machine() == 'x86_64' else None
        if probe.returncode or len(lines) < 2 or not plat:
            raise OSError('no wheel is known for this Python; install the package with: pip install zbic')
        tag, site = lines[0], Path(lines[1])
        request = urllib.request.Request(ZBIC_INDEX, headers={'User-Agent': 'Rekarina-Setup'})
        with urllib.request.urlopen(request, timeout=30) as r:
            meta = json.load(r)
        wheels = [f for f in meta.get('urls', []) if f['filename'].endswith('.whl') and (
            f['filename'].endswith(f'-{tag}-{tag}-win_amd64.whl') if WIN else
            f'-{tag}-{tag}-manylinux' in f['filename'] and f['filename'].endswith('_x86_64.whl'))]
        if not wheels:
            raise OSError(f'the index has no {tag} {plat} wheel of zbic {meta.get("info", {}).get("version", "")}')
        with urllib.request.urlopen(urllib.request.Request(wheels[0]['url'], headers={'User-Agent': 'Rekarina-Setup'}),
                                    timeout=60) as r:
            data = r.read()
        if hashlib.sha256(data).hexdigest() != wheels[0]['digests']['sha256']:
            raise OSError('the downloaded wheel does not match the checksum the index gives')
        site.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(io.BytesIO(data)) as z:
            for name in z.namelist():
                if not name.endswith('/') and '..' not in Path(name).parts and not Path(name).is_absolute():
                    z.extract(name, site)
        if subprocess.run([python, '-c', 'import zbic'], **quiet).returncode:
            raise OSError('the package was unpacked but does not load')
    except (OSError, ValueError, KeyError, zipfile.BadZipFile) as e:
        raise InstallError('oot.zbic_failed', reason=str(e)) from None


def tool_json(ctx, step, script, args):
    """Run a tool that prints one JSON object as its last line; (object or None, log path)."""
    last = ['']

    def parse(line):
        if line.startswith('{'):
            last[0] = line

    code, log = run_tool(ctx, step, script, args, parse)
    try:
        return json.loads(last[0]), log
    except ValueError:
        return ({'error': 'tool', 'code': code} if code else None), log


def tree_stamp(*paths):
    """A fingerprint of source trees: the path and the contents of every file. Contents, not modification times: a
    release unpacked on another day, or by the launcher's update, has new times on files that did not change, and
    the steps that read them must then be skipped, not run again (a compile and a shader preparation for nothing).
    The trees are small (a release's runtime folder is 50 MiB: a fraction of a second)."""
    h = hashlib.sha256()
    for p in paths:
        files = sorted(f for f in p.rglob('*') if f.is_file() and '__pycache__' not in f.parts) if p.is_dir() else [p]
        for f in files:
            name = f.relative_to(p).as_posix() if p.is_dir() else f.name
            h.update(name.encode() + bytes(1) + (bytes.fromhex(file_sha256(f)) if f.is_file() else b'missing') + bytes([1]))
    return h.hexdigest()


def host_gpu(ctx):
    """The graphics card of this machine (components.detect), asked once per run; None when nothing answered."""
    if not hasattr(ctx, 'gpu'):
        try:
            ctx.gpu = components.detect()
        except Exception as e:   # a detection that breaks must cost the presets, not the setup
            ctx.reporter.log(f'components: the graphics card could not be detected: {type(e).__name__}: {e}')
            ctx.gpu = None
    return ctx.gpu


class Check(Step):
    id = 'oot_check'
    always = True

    def run(self, ctx):
        layout, rep, opt = ctx.layout, ctx.reporter, ctx.options
        missing = layout.missing()
        if missing:
            raise InstallError('oot.tools_missing', paths=', '.join(missing))
        ensure_zbic(ctx, self.id)
        rep.progress(self.id, 'dump', force=True)
        game = opt.game or (ctx.state.get('oot') or {}).get('nup')
        if not game:
            raise InstallError('oot.need_game')
        info, _ = tool_json(ctx, self.id, layout.tools / 'oot_exefs.py', [game])
        if not info or info.get('error') in ('tool', 'bad_nup'):
            raise InstallError('oot.bad_nup', path=str(game), detail=(info or {}).get('detail', ''))
        if info.get('error') == 'no_nup':
            raise InstallError('oot.no_nup', path=str(game))
        if info.get('error'):
            raise InstallError('oot.no_exefs', path=info.get('path', str(game)))
        expected = json.loads(layout.expected.read_text(encoding='utf-8'))
        other = sorted(n for n, h in expected['exefs'].items() if info['files'].get(n) != h)
        if other:
            raise InstallError('oot.other_version', path=info['nup'], version=expected['version'], files=', '.join(other))
        ctx.nup, ctx.exefs_hashes = Path(info['nup']), info['files']
        ctx.state['oot'] = {'nup': info['nup'], 'version': expected['version']}
        rep.progress(self.id, 'space', force=True)
        ctx.dest.mkdir(parents=True, exist_ok=True)
        ctx.temp.mkdir(parents=True, exist_ok=True)
        first = not (ctx.dest / GAME_EXE).is_file()
        need, temp_need = (NEED_DEST if first else 2 << 30), (NEED_TEMP if not (ctx.temp / 'build' / 'obj').is_dir() else 2 << 30)
        free, temp_free = free_bytes(ctx.dest), free_bytes(ctx.temp)
        same = os.path.splitdrive(str(ctx.dest.resolve()))[0].lower() == os.path.splitdrive(str(ctx.temp.resolve()))[0].lower()
        rep.emit('space', dest=str(ctx.dest), need=need + (temp_need if same else 0), free=free, temp=str(ctx.temp),
                 temp_need=0 if same else temp_need, temp_free=temp_free, same_disk=same)
        if free < need + (temp_need if same else 0) or (not same and temp_free < temp_need):
            raise InstallError('oot.no_space', path=str(ctx.dest if same or free < need else ctx.temp),
                               need=round((need + (temp_need if same else 0)) / 2**30, 1), free=round(free / 2**30, 1))
        return {}


class Extract(Step):
    id = 'oot_extract'

    def key(self, ctx):
        expected = json.loads(ctx.layout.expected.read_text(encoding='utf-8'))
        return fingerprint('oot-extract-1', json.dumps(expected['exefs'], sort_keys=True), ctx.layout.tools / 'nso_plain.py')

    def current(self, ctx, record):
        sizes = record.get('sizes') or {}
        return bool(sizes) and all((ctx.dest / 'exefs' / n).is_file() and (ctx.dest / 'exefs' / n).stat().st_size == s and
                                   (ctx.temp / 'exefs' / n).is_file() for n, s in sizes.items())

    def run(self, ctx):
        ctx.reporter.progress(self.id, 'exefs', force=True)
        info, log = tool_json(ctx, self.id, ctx.layout.tools / 'oot_exefs.py',
                              [ctx.nup, '--out', ctx.temp / 'exefs', '--plain', ctx.dest / 'exefs'])
        if not info or info.get('error'):
            raise InstallError('oot.extract_failed', log=str(log))
        return {'sizes': {n: (ctx.dest / 'exefs' / n).stat().st_size for n in info['files']}}


def mod_patch_files(ctx):
    """The files of the mods folder that change the game's code: each mod's cheat files and its ExeFS patch files
    (.ips, .pchtxt in its exefs folder and in atmosphere/exefs_patches/<name>/), as tools/cheats.py and
    tools/exefs_patches.py find them. All of them, whatever build they are for: the tools sort that out."""
    return [f for m in mods.scan(ctx.dest) for f in mods.patch_files(m)]


def status_file(ctx, name):
    """build-cache/<name>.status in the game folder: what the build holds of the mods' cheats (tools/cheats.py) or
    ExeFS patches (tools/exefs_patches.py). The game reads it (runtime/cheats.c, runtime/mods.c)."""
    return ctx.dest / 'build-cache' / f'{name}.status'


def lift_base_key(ctx):
    """The lift's key without the mods' files: what it was before mods were built in, and what a game folder without
    code patches still has, so nothing is built again for it."""
    return fingerprint('oot-lift-2', ctx.keys['oot_extract'], ctx.layout.patches, tree_stamp(ctx.layout.recomp))


def compile_base_key(ctx):
    """The compile step's key for the lift without mods. The shaders are prepared for the game's code, not for the
    few sites a mod patches, so the shader step stands on this key and a mod costs no second preparation."""
    layout = ctx.layout
    return fingerprint('oot-compile-2', lift_base_key(ctx), tree_stamp(layout.root / 'runtime', layout.tools / 'build_game.py'),
                       tree_stamp(layout.pgo) if layout.pgo and layout.pgo.is_file() else 'no profile', cpu_level())


class Lift(Step):
    id = 'oot_lift'

    def key(self, ctx):
        files = mod_patch_files(ctx)
        if not files:
            return lift_base_key(ctx)
        # a file's name and content, and its place under mods/: the tools name each patch set after its mod folder
        return fingerprint('oot-lift-3', lift_base_key(ctx), ctx.layout.tools / 'cheats.py', ctx.layout.tools / 'exefs_patches.py',
                           *files, *(f.relative_to(mods.mods_dir(ctx.dest)).as_posix() for f in files))

    def current(self, ctx, record):
        return (ctx.temp / 'gen' / 'lift.done').is_file() and \
            (ctx.temp / 'gen' / 'lift.done').read_text(encoding='utf-8').strip() == record.get('key')

    def run(self, ctx):
        rep, gen = ctx.reporter, ctx.temp / 'gen'
        phase = ['discover']

        def parse(line):   # recomp/gen.py's own lines: functions found, then files written
            if 'emit' in line.lower() or 'writing' in line.lower():
                phase[0] = 'emit'
            rep.progress(self.id, phase[0])

        rep.progress(self.id, 'discover', force=True)
        (gen / 'lift.done').unlink(missing_ok=True)
        # The code cheats and the ExeFS patch files of the mods become patch sets beside the port's own; the status
        # files tell the game what the build holds. They are the player's downloads: files the tools refuse cost
        # those cheats or patches, not the build, which then goes on without them and says so.
        plain, patches, skipped, inputs = ctx.layout.patches, ctx.layout.patches, {}, mod_patch_files(ctx)
        stages = [('cheats.py', 'patches-cheats.txt', status_file(ctx, 'cheats'), 'warn.cheats_skipped'),
                  ('exefs_patches.py', 'patches-exefs.txt', status_file(ctx, 'exefs'), 'warn.exefs_skipped')]
        for tool, out, status, warn in stages:
            status.unlink(missing_ok=True)   # no status: the game takes every such file as not built
            if not inputs:
                continue
            status.parent.mkdir(parents=True, exist_ok=True)
            code, log = run_tool(ctx, self.id, ctx.layout.tools / tool,
                                 ['--exefs', ctx.temp / 'exefs', '--mods', mods.mods_dir(ctx.dest), '--patches', patches,
                                  '--out', ctx.temp / out, '--status', status])
            if code:
                status.unlink(missing_ok=True)
                skipped[warn] = log
            else:
                patches = ctx.temp / out
        args = ['--exefs', ctx.temp / 'exefs', '--out', gen]
        code, log = run_tool(ctx, self.id, ctx.layout.recomp / 'gen.py', args + ['--patches', patches], parse)
        if code and patches != plain:   # the lift does not take the mods' sets: without them
            for tool, out, status, warn in stages:
                status.unlink(missing_ok=True)
                skipped.setdefault(warn, log)
            code, log = run_tool(ctx, self.id, ctx.layout.recomp / 'gen.py', args + ['--patches', plain], parse)
        if code:
            raise InstallError('oot.lift_failed', code=code, log=str(log))
        for warn, wlog in skipped.items():
            rep.warning(warn, log=str(wlog))
        mods.write_built(ctx.dest, inputs)   # what this build was given: the launcher's Mods page reads it
        (gen / 'lift.done').write_text(ctx.keys[self.id] + '\n', encoding='utf-8')
        return {}


class Compile(Step):
    id = 'oot_compile'

    def key(self, ctx):   # (compile_base_key is this for the lift without mods: the two stay alike)
        layout = ctx.layout
        return fingerprint('oot-compile-2', ctx.keys['oot_lift'], tree_stamp(layout.root / 'runtime', layout.tools / 'build_game.py'),
                           tree_stamp(layout.pgo) if layout.pgo and layout.pgo.is_file() else 'no profile', cpu_level())

    def current(self, ctx, record):
        exe = ctx.temp / 'build' / ('rekarina' + EXE)
        return exe.is_file() and exe.stat().st_size == record.get('exe_size')

    def run(self, ctx):
        layout, rep, build = ctx.layout, ctx.reporter, ctx.temp / 'build'
        phase = ['compile']

        def parse(line):
            m = NINJA_LINE.match(line)
            if m:
                rep.progress(self.id, phase[0], int(m.group(1)), int(m.group(2)), 'objects')
            elif line.startswith('compile: ') or 'linking' in line.lower():
                phase[0] = 'link'
                rep.progress(self.id, 'link', force=True)

        rep.progress(self.id, 'compile', force=True)
        # With an execution profile the whole exe is compiled for it: the generated code at -O2 with the game's hot
        # paths known (the game's own frame is what limits the frame rate, and it is that code), the runtime linked as
        # one unit. The profile was trained on the C this lifter writes for this game version, so it applies here; a
        # function it does not know compiles without it. Without a profile the generated code stays at -O1 but for the
        # chunks with the game's hottest code (tools/data/hot-oot-v111.txt).
        pgo = layout.pgo if layout.pgo and layout.pgo.is_file() else None
        os.environ['REKARINA_RT_LTO'] = '1'
        os.environ.pop('REKARINA_PGO_RUNTIME_ONLY', None)
        hot = layout.tools / 'data' / 'hot-oot-v111.txt'
        if not pgo and hot.is_file():
            os.environ['REKARINA_HOT_OBJS'] = str(hot)
        else:
            os.environ.pop('REKARINA_HOT_OBJS', None)
        code, log = run_tool(ctx, self.id, layout.tools / 'build_game.py', [
            '--version', 'oot', '--gen', ctx.temp / 'gen', '--out', build, '--toolchain', layout.llvm, '--ninja', layout.ninja,
            '--deps', layout.deps, '--exefs', ctx.dest / 'exefs', '--no-romfs', *(['--pgo', pgo] if pgo else []),
            *(['--sysroot', layout.sysroot] if layout.sysroot else []),
            *(['--cpu', 'v2'] if cpu_level() == 'v2' else [])], parse)
        exe = build / ('rekarina' + EXE)
        if code or not exe.is_file():
            raise InstallError('oot.compile_failed', code=code, log=str(log))
        return {'exe_size': exe.stat().st_size}


class Components(Step):
    """The upscaler libraries for this machine's graphics card, beside the game (components.py: which, from where,
    under which license). Never fails the setup: without them the game runs in its base mode, the record says what
    is missing, and the next run (or the launcher's Retry) downloads only that."""

    id = 'oot_components'

    def key(self, ctx):
        return fingerprint('oot-components-1', components.pins_digest(), components.gpu_key(host_gpu(ctx)))

    def current(self, ctx, record):
        return not record.get('failed') and all((ctx.dest / n).is_file() for n in record.get('files') or [])

    def run(self, ctx):
        rep, gpu = ctx.reporter, host_gpu(ctx)
        rep.progress(self.id, 'detect', force=True)
        rep.log(f'components: graphics card: {components.describe(gpu) or "not detected"}; '
                f'groups: {", ".join(components.groups_for(gpu))}')
        failed, files = {}, []
        try:
            shim = ctx.layout.shim   # ours, bundled: frame generation loads Streamline through it
            if shim.is_file() and 'dlssg' in components.groups_for(gpu):
                shutil.copy2(shim, ctx.dest / SHIM)
            result = components.install(ctx.dest, gpu, ctx.temp / 'components', log=rep.log,
                                        progress=lambda group, done, total: rep.progress(self.id, group, done, total, 'bytes'))
            failed = result['failed']
            files = result['installed'] + result['present'] + result['kept']
            for name in result['kept']:
                rep.log(f'components: {name} in the game folder is not the pinned version: kept as it is')
        except net.Cancelled:
            raise KeyboardInterrupt from None
        except Exception as e:   # anything else is ours to survive: the libraries are optional
            rep.log(f'components: {type(e).__name__}: {e}')
            failed = {'*': f'{type(e).__name__}: {e}'}
        shutil.rmtree(ctx.temp / 'components', ignore_errors=True)
        if failed:
            rep.warning('oot.components_failed', files=', '.join(n for n in failed if n != '*') or '-',
                        detail=next(iter(failed.values()))[:300])
        chosen = components.preset(gpu, components.usable(ctx.dest))
        return {'gpu': gpu.to_json() if gpu else None, 'groups': components.groups_for(gpu), 'files': files,
                'failed': sorted(failed), 'preset': {'id': chosen['id'], 'tex': chosen['tex']}}


class Prompts(Step):
    """The button prompt overlays and the fonts that carry them: a minute of drawing, redone when the drawing tools or
    the dump change. It is a step of its own because it once shared a key with the shader preparation, and a change
    to a prompt then ran the shaders again (half an hour or more)."""

    id = 'oot_prompts'

    def key(self, ctx):
        return fingerprint('oot-prompts-2', ctx.keys['oot_extract'],
                           tree_stamp(*(ctx.layout.tools / n for n in ('make_oot_prompts.py', 'oot_hint_layout.py', 'button_icons.py',
                                                                      'make_ext_font.py'))))

    def current(self, ctx, record):
        build = ctx.temp / 'build'
        return (build / 'romfs-patches-xbox' / 'manifest.tsv').is_file() and \
            all((build / f'shared-fonts-{d}' / '5-nintendo-extension.bfttf').is_file() for d, _ in STYLES)

    def run(self, ctx):
        layout, rep, build = ctx.layout, ctx.reporter, ctx.temp / 'build'
        rep.progress(self.id, 'prompts', force=True)
        code, log = run_tool(ctx, self.id, layout.tools / 'make_oot_prompts.py', ['--out', build, '--nup', ctx.nup])
        if code:
            raise InstallError('oot.prompts_failed', code=code, log=str(log))
        for folder, style in STYLES:
            out = build / f'shared-fonts-{folder}'
            out.mkdir(parents=True, exist_ok=True)
            code, log = run_tool(ctx, self.id, layout.tools / 'make_ext_font.py',
                                 [out / '5-nintendo-extension.bfttf', '--style', style])
            if code:
                raise InstallError('oot.prompts_failed', code=code, log=str(log))
        return {}


# Bump when a change to the runtime (or anything else the shader key does not read) alters what --prepare-shaders makes
# from the same inputs: every folder then prepares its shaders again. The compile key is not in the shader key on
# purpose (see Prepare.key), so this is the one hand switch.
SHADER_FORMAT = 1
# The tools the prepared shaders follow from, and the seed of variants play met (tools/data).
SHADER_TOOLS = ('oot_prepare_shaders.py', 'prepare_shaders.py', 'oot_romfs.py', 'shader_census.py')
SHADER_DATA = ('shader-answers-oot.bin.gz',)
# A folder that an earlier setup prepared has the old form of this step's key (it took in the compile key and the
# prompt tools, so it cannot be recomputed from what is on disk). The releases below prepared exactly what this setup
# would (every variant, the same seed and translator rules), so their record is taken over; any other release's is not.
LEGACY_PREPARED_BY = ('1.2.0', '1.2.1')


def shader_mode(ctx):
    """'met' (the variants play met: a first install) or 'all' (every variant of the dump). A folder that has
    prepared all stays on 'all' (state 'shaders'), and so does one an earlier setup prepared: that one always did.
    --shaders-later (the launcher's run screen) never prepares the rest in the foreground: it keeps 'all' only while
    the step has nothing to do (the shaders' inputs did not change and the prepared output is in the temp folder) and
    otherwise prepares the variants play met only, which the launcher's background job (--all-shaders) then completes.
    The output matters as much as the inputs: a setup unpacked again, or a temp folder that was cleaned, has none, and
    'all' then prepared every variant in the foreground (35 minutes on 16 threads, hours on a weak machine)."""
    state = getattr(ctx, 'state', None) or {}
    opt = ctx.options
    if getattr(opt, 'all_shaders', False):
        return 'all'
    rec = (state.get('steps') or {}).get('oot_prepare')
    prepared_all = state.get('shaders') == 'all' or bool(rec and rec.get('shaders', 'all') == 'all')
    if getattr(opt, 'shaders_later', False):
        step = Prepare()
        stands = prepared_all and rec and step.current(ctx, rec) and \
            (rec.get('key') == step.key(ctx, 'all') or step.adopts(ctx, rec))
        return 'all' if stands else 'met'
    return 'all' if prepared_all else 'met'


def shader_case(ctx):
    """The shader step's case for the estimate (eta.py), from what it will do in this run: 'rerun' when it will be
    skipped (its record stands and its output is in the temp folder, as Context.plan decides), else what it prepares:
    'all' (every variant) or 'first' (the variants play met, the amount of a first install). A folder that prepared
    before but has no prepared output in the temp folder does that work in full, which a 'rerun' estimate (a minute)
    hid: the window counted down to a couple of minutes and stayed there."""
    rec = ctx.record('oot_prepare')
    step = Prepare()
    mode = shader_mode(ctx)
    if rec and step.current(ctx, rec) and (rec.get('key') == step.key(ctx, mode) or step.adopts(ctx, rec)):
        return 'rerun'
    return 'all' if mode == 'all' else 'first'


def shaders_only_met(state):
    """True for a folder whose shaders were prepared for the variants play met only (and has not prepared all since): the
    launcher offers the rest. A record of the old form (no 'shaders' field) prepared all."""
    rec = ((state or {}).get('steps') or {}).get('oot_prepare')
    return bool(rec) and rec.get('shaders') == 'met' and (state or {}).get('shaders') != 'all'


class Prepare(Step):
    """Every shader translated, and the driver's cache filled, before play (tools/oot_prepare_shaders.py)."""

    id = 'oot_prepare'

    def key(self, ctx, mode=None):
        """What the prepared shaders follow from, and nothing else: the dump, the translator's identity (the shader
        cache is keyed by it), the preparation tools and the seed, the mode. Not the compile key: the exe only carries
        the --prepare-shaders command, the shaders it makes follow from the translator and these inputs, and the
        package step builds the driver's cache again for the exe it copies (--warm-shaders), under the name the driver
        keys it by. A new exe therefore costs the compile and the package step, not half an hour of preparation."""
        layout = ctx.layout
        xid = layout.xlate.with_suffix('.id')
        # The dump's key is in ctx.keys once ctx.plan() ran. shader_mode also asks for this key before that (main's
        # estimate, which runs before run_steps plans): the extraction's key reads no other step's, so it is computed
        # here the way plan computes it.
        dump = ctx.keys['oot_extract'] if 'oot_extract' in ctx.keys else Extract().key(ctx)
        # (the identity file under the name it had before the translator was renamed)
        return fingerprint('oot-prepare-4', SHADER_FORMAT, mode or shader_mode(ctx), dump,
                           ('totk_xlate.id', xid) if xid.is_file() else tree_stamp(layout.xlate),
                           tree_stamp(*(layout.tools / n for n in SHADER_TOOLS), *(layout.tools / 'data' / n for n in SHADER_DATA)))

    PREPARING = '.shader-prepare-running'   # in the build folder from the start of run() until its end
    JOURNAL = 'shader-prepare-journal.bin'   # the runtime's, while the driver builds (vk_pipeline.c lb_warm_prepared)

    def current(self, ctx, record):
        """The output of a run that finished. --prepare-shaders writes shader-prepared.bin after the translation, before
        the driver's long phase, so a run cut short later leaves it: such a run also leaves PREPARING behind, and,
        cut short while the driver built (also by a setup from before PREPARING), the runtime's journal of that phase,
        which only a finished build removes."""
        build = ctx.temp / 'build'
        return (build / 'shader-prepared.bin').is_file() and not (build / self.PREPARING).exists() and \
            not (build / self.JOURNAL).exists()

    def adopts(self, ctx, record):
        """A record of the old key form is kept when its release prepared what this one would and the translator is the
        one that prepared it (the copy the step left in the build folder): a changed key form alone is no reason to
        prepare again."""
        if 'shaders' in record or record.get('release') not in LEGACY_PREPARED_BY:
            return False
        xid = ctx.layout.xlate.with_suffix('.id')
        for name in (xid.name, 'totk_xlate.id'):
            try:
                if xid.is_file() and (ctx.temp / 'build' / name).read_bytes() == xid.read_bytes():
                    return True
            except OSError:
                pass
        return False

    def adopted(self, ctx, record):
        record['shaders'] = 'all'   # what an old record means, kept in the new form
        ctx.state['shaders'] = 'all'

    def run(self, ctx):
        layout, rep, build = ctx.layout, ctx.reporter, ctx.temp / 'build'
        mode = shader_mode(ctx)
        # the translator beside the exe, where --prepare-shaders and the game look for it
        shutil.copy2(layout.xlate, build / layout.xlate.name)
        if layout.xlate.with_suffix('.id').is_file():
            shutil.copy2(layout.xlate.with_suffix('.id'), build / layout.xlate.with_suffix('.id').name)
        for lib in layout.libs:   # Linux: the game finds libSDL3.so.0 beside itself ($ORIGIN), here as in the package
            shutil.copy2(lib, build / lib.name)
        (build / self.PREPARING).write_text(mode + '\n', encoding='ascii')   # a run cut short leaves it (current)
        phase = ['seed']

        def parse(line):
            m = TRANSLATED.search(line)
            if m:
                phase[0] = 'translate'
                rep.progress(self.id, 'translate', int(m.group(1)), int(m.group(2)), 'shaders')
            elif 'variants of' in line:
                phase[0] = 'translate'
                rep.progress(self.id, 'translate', force=True)
            elif LIBRARIES.search(line):   # the long phase (half an hour on a busy machine): its count, not only its name
                m = LIBRARIES.search(line)
                phase[0] = 'libraries'
                rep.progress(self.id, 'libraries', int(m.group(1)), int(m.group(2)), 'shaders')
            elif phase[0] == 'translate' and ('librar' in line or 'pipeline' in line):
                rep.progress(self.id, 'libraries')

        rep.progress(self.id, 'seed', force=True)
        code, log = run_tool(ctx, self.id, layout.tools / 'oot_prepare_shaders.py',
                             [build, '--nup', ctx.nup, '--exefs', ctx.dest / 'exefs', '--exe-name', GAME_EXE,
                              *(['--met-only'] if mode == 'met' else [])], parse)
        if code or not (build / 'shader-prepared.bin').is_file():
            raise InstallError('oot.shaders_failed', code=code, log=str(log))
        # One shader-cache.pack instead of four files per shader (about 250,000 after the preparation): the game reads
        # the pack first, and with the loose files a start after the setup spent 7 s reading the shaders named by the
        # pipeline keys and 53 s reading their use masks, where a packed cache takes a fraction of a second; the
        # package step then copies one file instead of all of them.
        rep.progress(self.id, 'pack', force=True)
        code, log = run_tool(ctx, self.id, layout.tools / 'pack_shader_cache.py', [build / 'shader-cache'])
        if code:
            raise InstallError('oot.shaders_failed', code=code, log=str(log))
        (build / self.PREPARING).unlink(missing_ok=True)
        ctx.state['shaders'] = mode   # 'all' is kept through later updates (shader_mode)
        return {'shaders': mode}


LOOSE_SHADER_FILE = re.compile(r'[0-9a-f]{64}\.(spv|res\.json|use)$')   # tools/pack_shader_cache.py NAME, beside in/*.in


def has_loose_shader_files(cache):
    """Whether a shader cache holds files tools/pack_shader_cache.py would fold into its pack: stops at the first one."""
    try:
        with os.scandir(cache) as it:
            for e in it:
                if LOOSE_SHADER_FILE.match(e.name):
                    return True
        with os.scandir(cache / 'in') as it:
            return any(e.name.endswith('.in') for e in it)
    except OSError:
        return False


class Package(Step):
    id = 'oot_package'

    def key(self, ctx):
        opt = ctx.options
        # (the compile key only when mods changed the code: a folder without them keeps the key it had)
        built = ctx.keys['oot_compile']
        # (the texture step's tool and choice: a folder set up before it gets its textures prepared once)
        # (the compile key through compile_base_key: the shader key no longer holds it, and a new exe must be copied and
        # its driver cache warmed; the pack tool: a changed pack layout is converted here)
        return fingerprint('oot-package-4', ctx.keys['oot_prepare'], ctx.keys.get('oot_prompts', ''), compile_base_key(ctx),
                           ctx.layout.tools / 'package_oot.py', ctx.layout.tools / 'pack_shader_cache.py',
                           ctx.layout.tools / 'oot_prepare_textures.py', 'textures-no' if getattr(opt, 'no_textures', False) else '',
                           str(opt.dlls or ''), *([built] if built != compile_base_key(ctx) else []))

    def current(self, ctx, record):
        exe = ctx.dest / GAME_EXE
        return (exe.is_file() and exe.stat().st_size == record.get('exe_size') and
                any((ctx.dest / name).is_file() for name in (INI, OLD_INI)))

    def run(self, ctx):
        layout, rep, opt, build = ctx.layout, ctx.reporter, ctx.options, ctx.temp / 'build'
        # The build's shader cache in the pack layout of this setup (a no-op when it already is: the preparation packs
        # it, and a folder that skipped the preparation holds the pack of the release before).
        if (build / 'shader-cache').is_dir():
            rep.progress(self.id, 'pack', force=True)
            code, log = run_tool(ctx, self.id, layout.tools / 'pack_shader_cache.py', [build / 'shader-cache'])
            if code:
                raise InstallError('oot.package_failed', code=code, log=str(log))
        rep.progress(self.id, 'copy', force=True)
        if opt.dlls:   # the player's own DLSS / FSR / frame generation libraries go in with the build's
            for name in OPTIONAL_DLLS:
                if (Path(opt.dlls) / name).is_file() and not (build / name).is_file():
                    shutil.copy2(Path(opt.dlls) / name, build / name)
        # First settings for this machine's graphics card, for a rekarina.ini the tool writes new (an existing one is the
        # player's and stays). Chosen from what is in the folder now: a preset never switches on a library that
        # could not be downloaded.
        gpu = host_gpu(ctx)
        have = components.usable(ctx.dest) | (components.usable(build) if opt.dlls else set())
        chosen = components.preset(gpu, have)
        new_ini = not any((ctx.dest / name).is_file() for name in (INI, OLD_INI))
        preset_args = ['--preset-note', f'{components.describe(gpu) or "graphics card not detected"}: preset {chosen["id"]}']
        for k, v in chosen['settings'].items():
            preset_args += ['--set', f'{k}={v}']
        code, log = run_tool(ctx, self.id, layout.tools / 'package_oot.py', [
            '--nup', ctx.nup, '--build', build, '--exefs', ctx.dest / 'exefs', '--xlate', layout.xlate, '--out', ctx.dest,
            '--prompts', opt.prompts or 'auto', '--lang', opt.lang or 'en-US', *preset_args,
            *[arg for lib in layout.libs for arg in ('--lib', lib)]])
        exe = ctx.dest / GAME_EXE
        if code or not exe.is_file():
            if code and exe.is_file():
                try:
                    open(exe, 'ab').close()
                except PermissionError:
                    raise InstallError('oot.exe_in_use', path=str(exe)) from None
            raise InstallError('oot.package_failed', code=code, log=str(log))
        # A folder set up by a release before 1.2.0 holds its shader cache as loose files (four per shader, about 250,000),
        # which the copy above leaves beside the new pack: the game would still read them at every start (seconds to
        # a minute of its entry step). Folded into the pack here, once; a folder without loose files is not touched.
        if has_loose_shader_files(ctx.dest / 'shader-cache'):
            rep.progress(self.id, 'pack', force=True)
            code, log = run_tool(ctx, self.id, layout.tools / 'pack_shader_cache.py', [ctx.dest / 'shader-cache'])
            if code:
                raise InstallError('oot.package_failed', code=code, log=str(log))
        # The prepared pipelines, once more where the game is played from: a driver that keys its disk cache to the
        # executable's path (AMD) has none for this folder yet and builds them now instead of during play; one that
        # keys it to the file name (NVIDIA: the preparation ran under this name, tools/oot_prepare_shaders.py) finds
        # them all and is done in seconds. A failure here costs smoothness in the first minutes, not the install.
        rep.progress(self.id, 'warm', force=True)
        logs = ctx.temp / 'logs'
        logs.mkdir(parents=True, exist_ok=True)
        try:
            with open(logs / f'{self.id}-warm.log', 'w', encoding='utf-8', newline='\n') as out:
                proc = subprocess.Popen([str(exe), '--warm-shaders'], cwd=str(ctx.dest), stdout=out, stderr=subprocess.STDOUT,
                                        creationflags=subprocess.CREATE_NO_WINDOW if WIN else 0, start_new_session=not WIN)
                try:
                    warm = proc.wait()
                except BaseException:
                    kill_tree(proc)
                    raise
            rep.log(f'{GAME_EXE} --warm-shaders: exit code {warm}')
        except OSError as e:
            rep.log(f'{GAME_EXE} --warm-shaders did not start: {e}')
        # Every ASTC texture of the game into the texture cache, as play would write it (tools/oot_prepare_textures.py,
        # rekarina.exe --prepare-textures): a new player's first visit of a place no longer decodes and encodes its
        # textures in the frames that draw them. Skipped with --no-textures; a disk without room stops it with a
        # message (the game then fills the cache in play, as before), and a failure costs smoothness, not the install.
        if not getattr(opt, 'no_textures', False):
            rep.progress(self.id, 'textures', force=True)
            code, tlog = run_tool(ctx, self.id, layout.tools / 'oot_prepare_textures.py',
                                  [ctx.dest, '--nup', ctx.nup, '--exe-name', GAME_EXE])
            rep.log(f'texture preparation: exit code {code} ({tlog})')
        write_rebuild_command(ctx)
        return {'exe_size': exe.stat().st_size, 'exe_sha256': file_sha256(exe),
                'ini_preset': chosen['id'] if new_ini else None}


REBUILD_BOOT = ('import runpy, sys; sys.path.insert(0, sys.argv.pop(1)); '
                "runpy.run_module('installer.gui_oot', run_name='__main__', alter_sys=True)")


def rebuild_command(ctx):
    """The command that opens the launcher on this game folder and rebuilds it for its mods (gui_oot.py --rebuild):
    pythonw beside the setup's Python on Windows, so no console opens; in a checkout the Python that runs this."""
    layout = ctx.layout
    python = Path(sys.executable) if layout.dev else layout.python
    if WIN and python.with_name('pythonw.exe').is_file():
        python = python.with_name('pythonw.exe')
    return [str(python), '-c', REBUILD_BOOT, str(layout.root), '--root', str(layout.root), '--rebuild', str(ctx.dest)]


def write_rebuild_command(ctx):
    """build-cache/rebuild.txt in the game folder: rebuild_command, one argument per line. The game runs it at its
    start when a mod's code patches or code cheats are not in the build (runtime/cheats.c rebuild_if_needed)."""
    cache = ctx.dest / 'build-cache'
    cache.mkdir(parents=True, exist_ok=True)
    tmp = cache / 'rebuild.txt.tmp'
    tmp.write_text('\n'.join(rebuild_command(ctx)) + '\n', encoding='utf-8', newline='\n')
    os.replace(tmp, cache / 'rebuild.txt')


def all_steps():
    return [Check(), Extract(), Lift(), Compile(), Components(), Prompts(), Prepare(), Package()]


class TimedReporter(Reporter):
    """The reporter that also keeps each finished step's duration for the estimates of the next run (eta.record)."""

    cases = {}

    def step(self, step, state, **fields):
        if state == 'done' and step in self.cases:
            eta.record(step, self.cases[step], fields.get('seconds'))
        return super().step(step, state, **fields)


class Parser(argparse.ArgumentParser):
    def error(self, message):
        Reporter().error('cli.usage', {'detail': message})
        sys.exit(2)


def cancel(signum, frame):
    raise KeyboardInterrupt


def main(argv=None):
    ap = Parser(prog='python -m installer.oot', description=__doc__.splitlines()[0])
    ap.add_argument('--game', type=Path, help="the dump: the .nup container or the folder that holds it")
    ap.add_argument('--dest', type=Path, required=True, help='install folder')
    ap.add_argument('--lang', help='game language, a Switch language code (en-US, ru, ja, de, ...)')
    ap.add_argument('--prompts', choices=('auto', 'xbox', 'nintendo', 'playstation', 'rekarina'),
                    help='button prompts of a new install (default auto: the controller or keyboard in use)')
    ap.add_argument('--dlls', type=Path, help='a folder with your DLSS / FSR 3.1 / frame generation libraries to put '
                                              'beside the game')
    ap.add_argument('--toolchain', type=Path, help='a folder with llvm/ and bin/ninja (a checkout has none of its own)')
    ap.add_argument('--root', type=Path, help='release folder or checkout (default: the folder above installer/)')
    ap.add_argument('--temp', type=Path, help='temp folder (default: temp/ of the release, build/install-temp of a checkout)')
    ap.add_argument('--until', help='stop after this step (oot_check ... oot_package): for tests')
    ap.add_argument('--all-shaders', action='store_true',
                    help='prepare every variant of every shader (about 35 minutes more); without it a first install '
                         'prepares the variants play met, and a folder that prepared all keeps all')
    ap.add_argument('--shaders-later', action='store_true',
                    help='the launcher foreground run: prepare the variants play met only when the shaders have to be '
                         'prepared again; the launcher prepares the rest in the background (--all-shaders)')
    ap.add_argument('--no-textures', action='store_true',
                    help='do not prepare the textures for play (about 12 GiB on the disk; the game then prepares each one '
                         'at its first use)')
    args = ap.parse_args(argv)
    for name in ('SIGBREAK', 'SIGTERM'):
        if hasattr(signal, name):
            signal.signal(getattr(signal, name), cancel)
    rep, t0 = None, time.monotonic()
    try:
        dest = args.dest.resolve()
        layout = find_layout(args.root, args.toolchain)
        temp, moved = work_folder(layout.temp, args.temp, dest)
        log = temp / 'logs' / time.strftime('install-%Y%m%d-%H%M%S.log')
        rep = TimedReporter(log_path=log)
        if moved:
            rep.log(f'The disk of the setup has {moved} GiB free and the working files need about {NEED_TEMP >> 30}: '
                    f'they go into the game folder, {temp}')
        t0 = time.monotonic()
        try:
            state = statefile.load(dest)
        except statefile.StateError as e:
            rep.error(e.id, {'path': str(e.path)})
            rep.emit('end', result='failed', seconds=0)
            return 1
        ctx = Context(layout, dest, temp, rep, args, state=state)
        steps = all_steps()
        if args.until:
            steps = steps[:[s.id for s in steps].index(args.until) + 1]
        rep.emit('start', release=layout.release, mode=ctx.mode, dev=layout.dev, dest=str(dest), log=str(log),
                 steps=[s.id for s in steps], game='oot')
        # How long each step should take (eta.py): "first" for a step this folder has not finished before, "rerun" for
        # one it has (its caches are there). The window counts down from these; each finished step's real time is kept
        # for the next run on this machine.
        # A step whose output left the temp folder (a setup unpacked again, a cleaned temp folder) does its first run's
        # work again, whatever the record says.
        done = (state or {}).get('steps', {})
        rep.cases = {s.id: 'rerun' if state and (s.always or (s.id in done and s.current(ctx, done[s.id]))) else 'first'
                     for s in steps}
        # the shader step by what it will prepare: nothing, the variants play met, or every variant (an all-or-nothing
        # amount of work whatever the folder had before: its own estimate, no recorded timing)
        if 'oot_prepare' in rep.cases:
            rep.cases['oot_prepare'] = shader_case(ctx)
        rep.emit('eta', **eta.estimates(rep.cases))
        result = run_steps(steps, ctx)
    except Exception as e:
        # A failure outside the steps (run_steps reports its own): the traceback goes to the log, and the window gets
        # an error and the end of the run instead of a process that ended without either ("stopped unexpectedly").
        if rep is None:   # no log yet: stderr, which the window reads with stdout
            rep = TimedReporter()
            traceback.print_exc()
        rep.log(traceback.format_exc())
        rep.error('internal', {'exception': type(e).__name__, 'detail': str(e)})
        result = 'failed'
    rep.emit('end', result=result, seconds=round(time.monotonic() - t0, 1))
    rep.close()
    return EXIT[result]


if __name__ == '__main__':
    sys.exit(main())
