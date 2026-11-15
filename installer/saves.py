"""Bringing a save into the game folder: from an emulator, from another port of the game, from a folder or a .zip.

The game keeps one save per local account, as a console does (runtime/fs_host.c):

    <game folder>/profile/account.uid          16 bytes, the local account
    <game folder>/profile/save/<uid>/          the committed save: account.sav, option.sav, slot_00 .. slot_05 with
                                               caption.sav and progress.sav each
    <game folder>/profile/save-tx/             the running game's working copy and commits not swapped in yet
    <game folder>/profile/save-backup/         the game's own three last saves

An emulator and the other ports keep the same files under a folder of their own, so an import is a copy: the save the
game folder holds now (save/<uid> and save-tx, which may hold a newer commit) moves to
profile/save-before-import/<time>/, the source's files become save/<uid>, and the source is left as it was. restore()
puts a moved save back the same way.

    candidates()                 -> [Save] found in the places emulators and the other ports use, newest first
    find_root(path)              -> the save folder at or below a folder the player chose, or None
    describe(path, source)       -> Save
    current(dest)                -> Save of the game folder, or None
    import_save(dest, path)      -> the folder the replaced save went to (None: there was none)
    import_zip(dest, zip_path)   -> the same, for an archive of a save folder
    backups(dest)                -> [Path] of the saves imports replaced, newest first
    restore(dest, backup)        the save an import replaced comes back; the imported one takes its place in the backups

A place is only a place to look: whatever is found there must be this game's save by its files (is_save) before it is
offered. Nothing here needs the game's files or a window; installer/saves_test.py runs it on made-up folders.
"""
import json
import os
import re
import shutil
import tempfile
import time
import zipfile
from dataclasses import dataclass
from pathlib import Path

TITLE = '0400486024DEC000'   # the game's title id: the save folder's name in the emulators that name it by title
PROFILE, SAVE, TX, UID = 'profile', 'save', 'save-tx', 'account.uid'
BEFORE = 'save-before-import'
SLOT = re.compile(r'^slot_\d\d$')
FILES = ('progress.sav', 'caption.sav')
MAX_BYTES = 64 << 20          # a save is a few hundred kilobytes; anything far beyond that is something else
MAX_FILES = 400
WALK_DIRS = 4000              # folders one place may cost to look through

# where a save came from, as catalog ids: ui.saves_from_<id>
PORT, EMULATOR, FOLDER, ARCHIVE, OWN = 'port', 'emulator', 'folder', 'zip', 'own'

# Emulators by their data folder's name: the yuzu family keeps saves by title id, Ryujinx and its forks by a number.
EMULATORS = ('Ryujinx', 'Ryubing', 'yuzu', 'suyu', 'eden', 'citron', 'sudachi', 'torzu')
# Other ports of the game: (name, the builder's or launcher's settings folder under the local app data, its default
# install folder under the home folder). Their save is <install>/user/save/<account>/, the same files as ours.
PORTS = (('Hookshot', 'HookshotBuilder', Path('Games') / 'Hookshot'),
         ('Longshot', 'LongshotLauncher', Path('Games') / 'Longshot'))


class SaveError(Exception):
    """An import that cannot be done; code is a catalog id (saves.<code>), detail goes into its text."""

    def __init__(self, code, **detail):
        super().__init__(code)
        self.code, self.detail = code, detail


@dataclass
class Save:
    path: Path
    source: str = FOLDER       # PORT, EMULATOR, FOLDER, ARCHIVE or OWN
    name: str = ''             # the emulator's or port's name, '' for a folder
    slots: int = 0             # save slots with a progress file
    files: int = 0
    size: int = 0              # bytes
    modified: float = 0.0      # the newest file's time


# ---- what a save is ---------------------------------------------------------------------------------------------------

def slot_dirs(path):
    try:
        return sorted(p for p in Path(path).iterdir() if p.is_dir() and SLOT.match(p.name) and (p / FILES[0]).is_file())
    except OSError:
        return []


def is_save(path):
    """A folder that holds this game's save: at least one slot_NN with its progress file."""
    return bool(slot_dirs(path))


def _tree(path):
    """(files, bytes, newest time) of a folder; links are not followed."""
    files = size = 0
    newest = 0.0
    for root, dirs, names in os.walk(path):
        dirs[:] = [d for d in dirs if not os.path.islink(os.path.join(root, d))]
        for n in names:
            p = os.path.join(root, n)
            if os.path.islink(p):
                continue
            try:
                st = os.stat(p)
            except OSError:
                continue
            files, size, newest = files + 1, size + st.st_size, max(newest, st.st_mtime)
    return files, size, newest


def describe(path, source=FOLDER, name=''):
    path = Path(path)
    files, size, newest = _tree(path)
    return Save(path, source, name, len(slot_dirs(path)), files, size, newest)


def find_root(path, depth=5):
    """The save folder at `path` or the nearest one below it (a chosen emulator folder, an unpacked export)."""
    path = Path(path)
    level, seen = [path], 0
    for _ in range(depth + 1):
        below = []
        for p in level:
            if is_save(p):
                return p
            try:
                kids = sorted(k for k in p.iterdir() if k.is_dir() and not k.is_symlink())
            except OSError:
                continue
            seen += len(kids)
            if seen > WALK_DIRS:
                return None
            below += kids
        level = below
    return None


# ---- the places to look -----------------------------------------------------------------------------------------------

def _roots(env, home):
    """(name, data folder) of every emulator data folder that exists."""
    out = []
    bases = []
    for key in ('APPDATA', 'LOCALAPPDATA'):
        if env.get(key):
            bases.append(Path(env[key]))
    config = Path(env.get('XDG_CONFIG_HOME') or home / '.config')
    data = Path(env.get('XDG_DATA_HOME') or home / '.local' / 'share')
    bases += [config, data]
    try:
        for app in sorted((home / '.var' / 'app').iterdir()):   # Flatpak: every app has its own config and data
            bases += [app / 'config', app / 'data']
    except OSError:
        pass
    for base in bases:
        for name in EMULATORS:
            for folder in {name, name.lower()}:
                p = base / folder
                if p.is_dir():
                    out.append((name, p))
    return out


def _emulator_saves(name, root):
    found = []
    try:   # Ryujinx: bis/user/save/<number>/0 (and /1, the copy being written)
        for d in sorted((root / 'bis' / 'user' / 'save').iterdir()):
            found += [d / '0']
    except OSError:
        pass
    try:   # the yuzu family: nand/user/save/0000000000000000/<account>/<title id>
        for account in sorted((root / 'nand' / 'user' / 'save' / '0000000000000000').iterdir()):
            found += [account / TITLE, account / TITLE.lower()]
    except OSError:
        pass
    return [(name, p) for p in found if is_save(p)]


def _port_saves(env, home):
    found = []
    local = Path(env['LOCALAPPDATA']) if env.get('LOCALAPPDATA') else None
    for name, settings, default in PORTS:
        installs = [home / default]
        if local:
            try:   # a settings file of theirs may name the folder the player built the port into
                for f in sorted((local / settings).glob('*.json')):
                    installs += _paths_in(json.loads(f.read_text(encoding='utf-8', errors='replace')))
            except (OSError, ValueError):
                pass
        for install in installs:
            try:
                for account in sorted((Path(install) / 'user' / 'save').iterdir()):
                    if is_save(account):
                        found.append((name, account))
            except OSError:
                pass
    return found


def _paths_in(value, depth=0):
    """Every string of a settings value that names an existing folder."""
    if isinstance(value, str):
        return [Path(value)] if 2 < len(value) < 600 and os.path.isabs(value) and os.path.isdir(value) else []
    if depth > 4:
        return []
    kids = value.values() if isinstance(value, dict) else value if isinstance(value, list) else ()
    return [p for k in kids for p in _paths_in(k, depth + 1)]


def candidates(env=None, home=None):
    """The saves found on this machine, newest first; one entry per folder."""
    env = os.environ if env is None else env
    home = Path(home) if home else Path.home()
    out, seen = [], set()
    pairs = [(PORT, n, p) for n, p in _port_saves(env, home)]
    for name, root in _roots(env, home):
        pairs += [(EMULATOR, n, p) for n, p in _emulator_saves(name, root)]
    for source, name, path in pairs:
        key = os.path.normcase(os.path.realpath(path))
        if key not in seen:
            seen.add(key)
            out.append(describe(path, source, name))
    return sorted(out, key=lambda s: -s.modified)


# ---- the game folder's own save -----------------------------------------------------------------------------------------

def profile_dir(dest):
    return Path(dest) / PROFILE


def account(dest, create=False):
    """The save folder's name: the local account's 16 bytes as the game prints them (high half first). None when the
    game folder has no account yet; with create a new one is made, as the game would at its first start."""
    f = profile_dir(dest) / UID
    try:
        raw = f.read_bytes()
    except OSError:
        raw = b''
    if len(raw) != 16 or not any(raw):
        if raw or not create:
            if raw:
                raise SaveError('bad_account', path=str(f))
            return None
        raw = os.urandom(16)
        while not any(raw):
            raw = os.urandom(16)
        f.parent.mkdir(parents=True, exist_ok=True)
        tmp = f.with_name(f.name + '.new')
        tmp.write_bytes(raw)
        os.replace(tmp, f)
    return raw[8:][::-1].hex() + raw[:8][::-1].hex()


def save_dir(dest, create=False):
    uid = account(dest, create)
    return profile_dir(dest) / SAVE / uid if uid else None


def current(dest):
    d = save_dir(dest)
    return describe(d, OWN) if d and is_save(d) else None


def backups(dest):
    try:
        found = [p for p in (profile_dir(dest) / BEFORE).iterdir() if p.is_dir()]
    except OSError:
        return []
    return sorted(found, key=lambda p: p.name, reverse=True)


# ---- import and restore -------------------------------------------------------------------------------------------------

def _check(source):
    source = Path(source)
    if not is_save(source):
        raise SaveError('not_a_save', path=str(source))
    files, size, _ = _tree(source)
    if size > MAX_BYTES or files > MAX_FILES:
        raise SaveError('too_large', path=str(source))
    return source


def _stamp(root):
    name = time.strftime('%Y%m%d-%H%M%S')
    n, out = 1, root / name
    while out.exists():
        n, out = n + 1, root / f'{name}-{n}'
    return out


def _set_aside(dest):
    """Move the game folder's save (and what the running game left in save-tx) to a new backup folder; None when there
    is nothing to keep. A file the game or another program holds open stops it before anything has moved."""
    profile = profile_dir(dest)
    target = save_dir(dest, create=True)
    tx = profile / TX
    keep_save = target.is_dir() and any(target.iterdir())
    keep_tx = tx.is_dir() and any(tx.iterdir())
    if not keep_save and not keep_tx:
        return None
    backup = _stamp(profile / BEFORE)
    backup.mkdir(parents=True)
    moved = []
    try:
        for src, name in ((target, SAVE), (tx, TX)):
            if src.is_dir() and any(src.iterdir()):
                os.rename(src, backup / name)
                moved.append((src, backup / name))
    except OSError as e:
        for src, at in reversed(moved):
            os.rename(at, src)
        shutil.rmtree(backup, ignore_errors=True)
        raise SaveError('busy', detail=str(e)) from e
    return backup


def _put_back(dest, backup):
    """A backup's save and save-tx go back to their places (which must be free)."""
    profile = profile_dir(dest)
    target = save_dir(dest, create=True)
    for name, to in ((SAVE, target), (TX, profile / TX)):
        if (backup / name).is_dir():
            to.parent.mkdir(parents=True, exist_ok=True)
            if to.is_dir() and not any(to.iterdir()):
                to.rmdir()
            os.rename(backup / name, to)
    try:
        backup.rmdir()
        backup.parent.rmdir()
    except OSError:
        pass


def game_running(dest):
    """Whether the game of this folder runs now (Windows: its exe cannot be opened for writing then). A running game
    holds the save in memory and writes it over whatever an import puts there."""
    if os.name != 'nt':
        return False
    for name in ('Rekarina.exe', 'totk.exe'):
        exe = Path(dest) / name
        if exe.is_file():
            try:
                with open(exe, 'r+b'):
                    pass
            except PermissionError:
                return True
            except OSError:
                pass
    return False


def import_save(dest, source):
    """The save in folder `source` becomes the game folder's save. Returns the backup folder of the save it replaced,
    or None when the game folder had none. The source is not changed."""
    source = _check(source)
    if game_running(dest):
        raise SaveError('running')
    target = save_dir(dest, create=True)
    if os.path.normcase(os.path.realpath(source)) == os.path.normcase(os.path.realpath(target)):
        raise SaveError('same', path=str(source))
    target.parent.mkdir(parents=True, exist_ok=True)
    staging = target.with_name(target.name + '.importing')
    shutil.rmtree(staging, ignore_errors=True)
    try:
        shutil.copytree(source, staging, symlinks=False, ignore=_links)
    except (OSError, shutil.Error) as e:
        shutil.rmtree(staging, ignore_errors=True)
        raise SaveError('copy_failed', detail=str(e)) from e
    if not is_save(staging):
        shutil.rmtree(staging, ignore_errors=True)
        raise SaveError('copy_failed', detail=str(source))
    try:
        backup = _set_aside(dest)
    except SaveError:
        shutil.rmtree(staging, ignore_errors=True)
        raise
    try:
        if target.is_dir():
            target.rmdir()   # empty: _set_aside moved a save that held anything
        os.rename(staging, target)
    except OSError as e:
        if backup:
            _put_back(dest, backup)
        shutil.rmtree(staging, ignore_errors=True)
        raise SaveError('busy', detail=str(e)) from e
    return backup


def _links(folder, names):
    return [n for n in names if os.path.islink(os.path.join(folder, n))]


def import_zip(dest, archive):
    """An archive of a save folder (a save manager's export, a backup the player zipped)."""
    archive = Path(archive)
    with tempfile.TemporaryDirectory(prefix='rekarina-save-') as tmp:
        try:
            with zipfile.ZipFile(archive) as z:
                infos = z.infolist()
                if len(infos) > MAX_FILES * 4 or sum(i.file_size for i in infos) > MAX_BYTES:
                    raise SaveError('too_large', path=str(archive))
                base = os.path.realpath(tmp)
                for i in infos:
                    to = os.path.realpath(os.path.join(tmp, i.filename))
                    if os.path.commonpath([base, to]) != base:
                        raise SaveError('bad_archive', path=str(archive))
                z.extractall(tmp)
        except (zipfile.BadZipFile, OSError, ValueError) as e:
            raise SaveError('bad_archive', path=str(archive)) from e
        root = find_root(tmp)
        if not root:
            raise SaveError('not_a_save', path=str(archive))
        return import_save(dest, root)


def restore(dest, backup):
    """The save in a backup folder comes back; the save it replaces goes into the backups in its turn."""
    backup = Path(backup)
    if not (backup / SAVE).is_dir() and not (backup / TX).is_dir():
        raise SaveError('no_backup', path=str(backup))
    if game_running(dest):
        raise SaveError('running')
    holding = backup.with_name(backup.name + '.restoring')
    os.rename(backup, holding)   # out of the way of the new backup's name
    try:
        _set_aside(dest)
    except SaveError:
        os.rename(holding, backup)
        raise
    _put_back(dest, holding)
    try:
        holding.rmdir()
    except OSError:
        pass
