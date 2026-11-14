"""The launcher's view of the game folder's mods: what is there, what each mod holds, which are on and in what order.

The game is what uses the mods (runtime/mods.c: one folder per mod under mods/ beside the exe, unpacked as emulators
take them). This module reads the same folders by the same rules, so the launcher's list says what the game will say,
and writes the two settings the game reads: `mods_order` (names top first, separated by '|') and `mods_disabled` (the
names switched off), in rekarina.ini. A mod the settings do not name yet goes below the named ones, by name, and is on.

    scan(dest)                       -> [Mod], in the game's order
    set_enabled(dest, name, on)      switch one mod
    move(dest, name, delta)          -1 up, +1 down
    add(dest, source)                a folder or a .zip, copied into mods/ under its own name; returns the Mod
    remove(dest, name)               the mod's folder goes to mods-removed/ beside mods/ (nothing is deleted)

A mod's code patches and code cheats are built into the game by the setup (installer/oot.py Lift). The lift writes
build-cache/mods-built.txt, the files it was given with their hashes; a mod with a patch file that list lacks, or
holds with another content, waits for a rebuild (Mod.waits).

Nothing here needs the game's files or a window; installer/mods_test.py runs it on made-up folders.
"""
import hashlib
import os
import re
import shutil
import time
import zipfile
from dataclasses import dataclass, field
from pathlib import Path

TITLE = '0400486024DEC000'   # the game's title id: the folder name in the Atmosphere layouts (runtime/mods.c TITLE)
MODS, REMOVED = 'mods', 'mods-removed'
BUILT = Path('build-cache') / 'mods-built.txt'   # what the last lift was given: "<path under mods/>\t<sha256>" a line
INI, OLD_INI = 'rekarina.ini', 'totk.ini'
KEY_ORDER, KEY_OFF = 'mods_order', 'mods_disabled'
PARTS = ('romfs', 'exefs', 'cheats')
BAD_NAME = re.compile(r'[|<>:"/\\?*\x00-\x1f]')

# what a mod holds (Mod.kinds) and why it cannot be used (Mod.problem), as catalog ids: ui.mods_kind_<id>, ui.mods_problem_<id>
FILES, PATCH, CHEATS = 'files', 'patch', 'cheats'
EMPTY, PLUGIN, NAME = 'empty', 'plugin', 'name'


@dataclass
class Mod:
    name: str
    path: Path
    enabled: bool = True
    kinds: tuple = ()
    files: int = 0             # game files its RomFS folder holds
    patches: int = 0           # code patch files (.ips, .pchtxt)
    cheats: int = 0            # cheat files
    problem: str = ''          # '' or EMPTY, PLUGIN, NAME
    waits: bool = False        # a code patch or cheat file of it is not in the build yet
    roots: dict = field(default_factory=dict)   # part -> its folder


def mods_dir(dest):
    return Path(dest) / MODS


def find_root(mod, part):
    """The folder of one part of a mod: <mod>/<part>, or one of the Atmosphere places of it. None when it has none."""
    for form in (Path(part), Path('atmosphere') / 'contents' / TITLE / part, Path('contents') / TITLE / part, Path(TITLE) / part):
        p = mod / form
        if p.is_dir():
            return p
    return None


def describe(path, enabled=True):
    """One mod folder as the game reads it."""
    path = Path(path)
    m = Mod(name=path.name, path=path, enabled=enabled)
    if '|' in path.name:
        m.problem = NAME
    romfs = find_root(path, 'romfs') or find_root(path, 'romfslite')
    exefs, cheats = find_root(path, 'exefs'), find_root(path, 'cheats')
    kinds, plugin = [], False
    if romfs:
        m.roots['romfs'] = romfs
        m.files = sum(1 for f in romfs.rglob('*') if f.is_file())
        if m.files:
            kinds.append(FILES)
    patch_dirs = [exefs] if exefs else []
    extra = path / 'atmosphere' / 'exefs_patches'
    if extra.is_dir():
        patch_dirs += [d for d in sorted(extra.iterdir()) if d.is_dir()]
    for d in patch_dirs:
        for f in d.iterdir():
            if not f.is_file():
                continue
            low = f.name.lower()
            if low.endswith(('.ips', '.pchtxt')):
                m.patches += 1
            elif d == exefs and (low.startswith('subsdk') or low == 'main'):
                plugin = True   # a code module: the game has no loader for it, and its other files may expect that code
    if exefs:
        m.roots['exefs'] = exefs
    if m.patches:
        kinds.append(PATCH)
    if cheats:
        m.roots['cheats'] = cheats
        m.cheats = sum(1 for f in cheats.iterdir() if f.is_file() and f.suffix.lower() == '.txt')
        if m.cheats:
            kinds.append(CHEATS)
    m.kinds = tuple(kinds)
    if not m.problem:
        m.problem = PLUGIN if plugin else '' if kinds else EMPTY
    return m


# ---- what the build holds

def patch_files(mod):
    """The files of a mod that change the game's code: its cheat files and its ExeFS patch files (.ips, .pchtxt in its
    exefs folder and in atmosphere/exefs_patches/<name>/), as tools/cheats.py and tools/exefs_patches.py find them.
    All of them, whatever build they are for: the tools sort that out."""
    cheats, exefs = mod.roots.get('cheats'), mod.roots.get('exefs')
    folders = [exefs] if exefs else []
    extra = mod.path / 'atmosphere' / 'exefs_patches'
    folders += [d for d in sorted(extra.iterdir()) if d.is_dir()] if extra.is_dir() else []
    out = [f for f in (sorted(cheats.iterdir()) if cheats else ()) if f.is_file() and f.suffix.lower() == '.txt']
    return out + [f for d in folders for f in sorted(d.iterdir()) if f.is_file() and f.suffix.lower() in ('.ips', '.pchtxt')]


def _stamp(dest, f):
    return f.relative_to(mods_dir(dest)).as_posix(), hashlib.sha256(f.read_bytes()).hexdigest()


def read_built(dest):
    """{path under mods/: sha256} of the files the last lift was given; empty when it had none."""
    try:
        lines = (Path(dest) / BUILT).read_text(encoding='utf-8').splitlines()
    except OSError:
        return {}
    return dict(line.split('\t', 1) for line in lines if '\t' in line)


def write_built(dest, files):
    """The lift's record of the mods' files it was given (installer/oot.py Lift.run). No files: no record."""
    path = Path(dest) / BUILT
    if not files:
        path.unlink(missing_ok=True)
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + '.tmp')
    tmp.write_text(''.join('\t'.join(_stamp(dest, f)) + '\n' for f in files), encoding='utf-8', newline='\n')
    os.replace(tmp, path)


def waits(dest, mod, built):
    try:
        return any(built.get(rel) != digest for rel, digest in (_stamp(dest, f) for f in patch_files(mod)))
    except OSError:
        return True


# ---- the two settings

def ini_path(dest):
    dest = Path(dest)
    return dest / INI if (dest / INI).is_file() or not (dest / OLD_INI).is_file() else dest / OLD_INI


def _line(key):
    return re.compile(r'^(\s*)(?:totk_)?' + key + r'(\s*=\s*)(.*?)\s*$', re.IGNORECASE)


def read_keys(dest):
    """(order, off): the names `mods_order` lists, top first, and the set `mods_disabled` names."""
    try:
        text = ini_path(dest).read_text(encoding='utf-8-sig', errors='replace')
    except OSError:
        return [], set()
    found = {}
    for raw in text.splitlines():
        for key in (KEY_ORDER, KEY_OFF):
            m = _line(key).match(raw)
            if m and not raw.lstrip().startswith(('#', ';')):
                found[key] = m.group(3)   # the last line of a key counts, as in the game
    names = lambda v: [n for n in (v or '').split('|') if n]   # noqa: E731
    return names(found.get(KEY_ORDER)), set(names(found.get(KEY_OFF)))


def write_keys(dest, order, off):
    """The two keys into the ini file, every other line kept: a key's line is replaced where it stands, a key the file
    lacks goes into its [mods] section (made at the end when there is none). Written through a temporary file."""
    path = ini_path(dest)
    try:
        raw = path.read_bytes()
    except OSError:
        raw = b''
    bom = raw.startswith(b'\xef\xbb\xbf')
    text = raw[3:].decode('utf-8', errors='replace') if bom else raw.decode('utf-8', errors='replace')
    eol = '\r\n' if '\r\n' in text else '\n'
    lines = text.splitlines()
    values = {KEY_ORDER: '|'.join(order), KEY_OFF: '|'.join(sorted(off))}
    for key, value in values.items():
        at = [i for i, raw_line in enumerate(lines) if _line(key).match(raw_line) and not raw_line.lstrip().startswith(('#', ';'))]
        new = f'{key} = {value}'
        if at:
            lines[at[-1]] = new
            for i in reversed(at[:-1]):
                del lines[i]
            continue
        section = next((i for i, raw_line in enumerate(lines) if raw_line.strip().lower() == '[mods]'), None)
        if section is None:
            if lines and lines[-1].strip():
                lines.append('')
            lines += ['[mods]', '# The order of the mods folder (top first) and the mods switched off. The launcher and the game\'s',
                      '# Mods tab write these two lines.']
            section = len(lines) - 1
            while section and lines[section].strip().lower() != '[mods]':
                section -= 1
        end = section + 1
        while end < len(lines) and not lines[end].lstrip().startswith('['):
            end += 1
        while end > section + 1 and not lines[end - 1].strip():
            end -= 1
        lines.insert(end, new)
    data = (eol.join(lines) + eol).encode('utf-8')
    tmp = path.with_name(path.name + '.tmp')
    tmp.write_bytes((b'\xef\xbb\xbf' if bom else b'') + data)
    os.replace(tmp, path)


# ---- the list

def scan(dest):
    """The mods of the game folder in the game's order: the names `mods_order` lists first, as listed, then the rest by
    name. Folders whose name starts with a dot are not mods."""
    root = mods_dir(dest)
    order, off = read_keys(dest)
    try:
        folders = [p for p in root.iterdir() if p.is_dir() and not p.name.startswith('.')]
    except OSError:
        return []
    rank = {name: i for i, name in enumerate(order)}
    folders.sort(key=lambda p: (rank.get(p.name, len(rank)), p.name if p.name not in rank else ''))   # as strcmp orders them
    found, built = [describe(p, p.name not in off) for p in folders], read_built(dest)
    for m in found:
        m.waits = not m.problem and waits(dest, m, built)
    return found


def _save(dest, mods):
    write_keys(dest, [m.name for m in mods if '|' not in m.name], {m.name for m in mods if not m.enabled and '|' not in m.name})


def set_enabled(dest, name, on):
    mods = scan(dest)
    for m in mods:
        if m.name == name:
            m.enabled = bool(on)
    _save(dest, mods)
    return mods


def move(dest, name, delta):
    mods = scan(dest)
    i = next((k for k, m in enumerate(mods) if m.name == name), None)
    if i is None or not 0 <= i + delta < len(mods):
        return mods
    mods[i], mods[i + delta] = mods[i + delta], mods[i]
    _save(dest, mods)
    return mods


# ---- adding and removing

class ModError(Exception):
    """id: a catalog id (mods.<id>); params: its values."""

    def __init__(self, id, **params):
        super().__init__(id)
        self.id, self.params = id, params


def clean_name(name):
    name = BAD_NAME.sub(' ', name).strip(' .')
    return re.sub(r'\s+', ' ', name)[:80] or 'mod'


def free_name(root, name):
    """name, or name with a number, as a folder mods/ does not hold yet."""
    if not (root / name).exists():
        return name
    for n in range(2, 1000):
        if not (root / f'{name} ({n})').exists():
            return f'{name} ({n})'
    raise ModError('mods.no_name', name=name)


def _is_mod_root(p):
    return any(find_root(p, part) for part in PARTS + ('romfslite',)) or (p / 'atmosphere' / 'exefs_patches').is_dir()


def _mod_root(top):
    """The folder inside an unpacked archive that is the mod: the top itself, or the one folder that holds its parts
    (archives often wrap the mod in a folder of its name, or two)."""
    p = top
    for _ in range(3):
        if _is_mod_root(p):
            return p
        inside = [c for c in p.iterdir() if not c.name.startswith('.') and c.name != '__MACOSX']
        if len(inside) != 1 or not inside[0].is_dir():
            return None
        p = inside[0]
    return p if _is_mod_root(p) else None


def add(dest, source):
    """Copies a mod into mods/: source is the mod's folder or a .zip of it. The mod keeps the folder's (or the
    archive's) name; a second one of that name gets a number. Returns the new Mod."""
    source = Path(source)
    root = mods_dir(dest)
    root.mkdir(parents=True, exist_ok=True)
    if source.is_dir():
        inner = _mod_root(source)
        if inner is None:
            raise ModError('mods.not_a_mod', path=str(source))
        name = free_name(root, clean_name(source.name))
        shutil.copytree(inner, root / name)
        return describe(root / name)
    if not source.is_file() or not zipfile.is_zipfile(source):
        raise ModError('mods.not_a_mod', path=str(source))
    tmp = root / f'.adding-{os.getpid()}-{int(time.time())}'
    try:
        with zipfile.ZipFile(source) as z:
            base = tmp.resolve()
            for info in z.infolist():
                target = (tmp / info.filename).resolve()
                if base != target and base not in target.parents:   # a member that would land outside the folder
                    raise ModError('mods.bad_archive', path=str(source))
            z.extractall(tmp)
        inner = _mod_root(tmp)
        if inner is None:
            raise ModError('mods.not_a_mod', path=str(source))
        name = free_name(root, clean_name(source.stem))
        shutil.move(str(inner), str(root / name))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return describe(root / name)


def remove(dest, name):
    """Takes a mod out of mods/: its folder moves to mods-removed/<name> (with the time when that name is taken), so
    nothing is lost. Its name leaves the two settings."""
    root = mods_dir(dest)
    src = root / name
    if not src.is_dir():
        raise ModError('mods.missing', name=name)
    bin_ = Path(dest) / REMOVED
    bin_.mkdir(exist_ok=True)
    target = bin_ / name
    if target.exists():
        target = bin_ / f'{name} {time.strftime("%Y%m%d-%H%M%S")}'
    shutil.move(str(src), str(target))
    _save(dest, scan(dest))
    return target


def summary(mods):
    """(mods, on, waiting for a rebuild) for the home screen's line and the page's total. A mod that cannot be used
    is neither on nor waiting; one that is switched off does not wait: the game asks for no rebuild for it."""
    live = [m for m in mods if m.enabled and not m.problem]
    return len(mods), len(live), sum(1 for m in live if m.waits)
