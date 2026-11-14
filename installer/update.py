"""The launcher's update: is there a newer Rekarina release, and getting it into this release folder.

    check()      asks for the newest release (the site's record, else GitHub) and compares it with this folder's VERSION
    download()   the release's setup archive, checked against the release's signed checksum list
    stage()      unpacks it beside the running files, ready for the swap
    restart()    starts Rekarina-Launcher.exe, which swaps the staged files in and starts the new window

Nothing of the player's goes to the network: the one request is a GET of the public release record, with the header
`User-Agent: Rekarina-Launcher/<version>` (net.py). The game dump is never downloaded or uploaded: an update brings
the tools, and the setup steps then rebuild the game from the dump the player already has.

The file the game reads
-----------------------
check() keeps its last answer in update.json in the user's application data (%APPDATA%\\Rekarina\\update.json; off
Windows $XDG_CONFIG_HOME/Rekarina/update.json), UTF-8, one JSON object. The game's runtime reads two keys of it to
show its "a newer version is out" notice, and these two keep their name, type and meaning in every later version:

    "latest"   string   the newest release's version without a leading "v" ("1.0.1"); "" when none is known
    "checked"  integer  when that was learned, seconds since 1970 (UTC)

The other keys are the launcher's own and may change: "format" (1), "current" (the version that asked), "available",
"notes" (the release text, shortened), "page" (the release's web page), "asset" ({"name", "url", "size", "digest"} of
the setup archive, or null), "sums_url" (the SHA256SUMS asset, or ""), "retry_after" (after a rate limit answer: no
request before this time). An answer is reused for six hours; a check that fails (offline, rate limit) leaves the
file's "latest" as it was.

Where things are
----------------
The repository is REPO below; REKARINA_REPO ("owner/name") names another one (a fork, a test). REKARINA_UPDATE_API
replaces https://api.github.com (tests serve a release record from localhost).

FEED names the project's own site: the address of a small JSON file there, written by tools/make_rekarina_release.py
(--site-url) in the shape of GitHub's release record (tag_name, body, html_url, assets with name,
browser_download_url, size and digest), so one reader serves both. With a feed, check() asks the site first and
GitHub only when the site does not answer or answers nonsense; without one (FEED empty) GitHub is the only source.
REKARINA_UPDATE_FEED names another feed (a test, a mirror).
docs/distribution.md has the site's file layout.

Who vouches for a download
--------------------------
SIGNING_KEYS holds the project's Ed25519 public keys (hex). With one in it, an archive is installed only when the
release carries SHA256SUMS and SHA256SUMS.sig, the signature is one of these keys' over the bytes of SHA256SUMS
(ed25519.py), and the archive's SHA-256 is its line there: a site, a mirror or a release page that somebody else got
to write to cannot hand out another program. The key's private half signs with tools/release_sign.py and never
leaves the maintainer's machine. Without a key (the tuple empty) the archive is checked against the digest GitHub
lists for it or the unsigned SHA256SUMS, as before. An older signed release is never offered: check() only names a
release newer than this folder's VERSION, and stage() refuses an archive that holds another version than the one
asked for.

How the swap works
------------------
The window that downloads the update runs on the release folder's own Python, and Windows does not let a running
program's files be replaced. So nothing is replaced while it runs:

 1. stage() unpacks the archive into <release>/update-staging/new and, last of all, writes update-staging/ready.txt.
 2. restart() starts Rekarina-Launcher.exe and the window closes.
 3. The launcher exe (installer/launcher/rekarina.c, apply_update) sees ready.txt before it starts any Python: it
    moves each top-level entry of the release that the new one replaces into <release>/update-old-<n> and the staged
    entry into its place (renames on one disk: quick, and each either happens or does not). A running exe may be
    renamed, so the launcher moves itself too. When a move fails, because some program holds a file of the old
    release open, it moves everything back, says so, and starts the old version: the folder is never left half
    new. (Windows 11 also renames the folder of an exe that is merely running: a second Rekarina window then goes
    on from update-old-<n> until it is closed, and what it still holds is removed at a later start.)
 4. It starts the new Rekarina-Launcher.exe with --after-update. That window removes update-old-* (cleanup) and runs
    the setup steps on the remembered game folder: a step whose inputs did not change is skipped (oot.py's keys), so
    only what the release changed is rebuilt, and the shaders are prepared and warmed again when the translator or
    the runtime changed.

Only entries the archive brings are touched: temp/ (the generated code and the objects of the last build), the game
folder, its caches, rekarina.ini and the settings in the application data stay as they are.

A source checkout (no bundled Python beside VERSION, or a .git folder) is never updated this way: check() reports,
can_apply is False.
"""
import binascii
import json
import os
import re
import shutil
import subprocess
import time
import urllib.error
import zipfile
from pathlib import Path

from . import ed25519, net

REPO = 'hooxielooxie/Rekarina'
API = 'https://api.github.com'
FEED = 'https://rekarina.dev/releases/latest.json'   # the site's release record; '' leaves GitHub as the only source
# hex Ed25519 public keys of tools/release_sign.py keygen; () takes unsigned releases. The first signs the releases;
# the second is the spare whose private half is kept offline, for a release after the first is lost or leaked.
SIGNING_KEYS = ('3d2c5774c8f60fc0067af2a9f51480a7ca11f29f01c71bfdca15abbc5426a2e5',
                'e69d2622597a0ceb2cd48cf44b1db3f424518329f67e7a2bcdb7b4656858c646')
CACHE_SECONDS = 6 * 3600
CHECK_TIMEOUT = 6
NOTES_CHARS = 4000
FORMAT = 1
STAGING, READY, NEW = 'update-staging', 'ready.txt', 'new'
LAUNCHER = 'Rekarina-Launcher.exe'
# what a staged release must hold to be swapped in: without these the launcher could not start again
REQUIRED = ('VERSION', LAUNCHER, 'python/pythonw.exe', 'python/python.exe', 'installer/gui_oot.py', 'installer/update.py')
# top-level names an archive never replaces: the player's build files and the update's own folders
PROTECTED = ('temp', STAGING)


class UpdateError(Exception):
    """A failure with a message id (installer/text/en.json, "update.*") and its parameters."""

    def __init__(self, id, **params):
        super().__init__(id)
        self.id, self.params = id, params


def repo():
    name = os.environ.get('REKARINA_REPO', '').strip()
    return name if re.fullmatch(r'[\w.-]+/[\w.-]+', name) else REPO


def api():
    return (os.environ.get('REKARINA_UPDATE_API', '').strip() or API).rstrip('/')


def feed():
    return os.environ.get('REKARINA_UPDATE_FEED', '').strip() or FEED


def signing_keys():
    """The public keys as bytes; a text that is not 32 bytes of hex is a mistake in this file, not a missing key."""
    return [bytes.fromhex(k) for k in SIGNING_KEYS]


def state_dir():
    base = os.environ.get('APPDATA') if os.name == 'nt' else os.environ.get('XDG_CONFIG_HOME')
    return Path(base or Path.home() / '.config') / 'Rekarina'


def state_path():
    return state_dir() / 'update.json'


def parse_version(text):
    """'v1.2.10' -> (1, 2, 10): the leading numbers of a version, whatever follows them ('1.0.0-rc1', '1.0.0+abc').
    () for a text without one."""
    m = re.match(r'\s*[vV]?(\d+(?:\.\d+)*)', text or '')
    return tuple(int(n) for n in m.group(1).split('.')) if m else ()


def newer(candidate, current):
    """True when version text candidate is a later release than current (1.0 equals 1.0.0; no version is never newer)."""
    a, b = parse_version(candidate), parse_version(current)
    if not a:
        return False
    n = max(len(a), len(b))
    return a + (0,) * (n - len(a)) > b + (0,) * (n - len(b))


def can_apply(root=None):
    """True in a release folder: its own Python and launcher beside VERSION, and no checkout around it."""
    root = Path(root or net.ROOT)
    return (os.name == 'nt' and (root / 'VERSION').is_file() and (root / 'python' / 'pythonw.exe').is_file()
            and (root / LAUNCHER).is_file() and not (root / '.git').exists() and not (root.parent / '.git').exists())


def load_cache():
    try:
        data = json.loads(state_path().read_text(encoding='utf-8'))
        return data if isinstance(data, dict) else {}
    except (OSError, ValueError):
        return {}


def save_cache(data):
    """Replace update.json in one step, so the game never reads half a file. A profile that cannot be written only
    costs the reuse of the answer."""
    path = state_path()
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        tmp = path.with_name(path.name + '.tmp')
        with open(tmp, 'w', encoding='utf-8', newline='\n') as f:
            json.dump(data, f, indent=1, ensure_ascii=False)
            f.write('\n')
        os.replace(tmp, path)
    except OSError:
        pass


def trim_notes(body):
    text = (body or '').replace('\r\n', '\n').replace('\r', '\n').strip()
    if len(text) > NOTES_CHARS:
        cut = text.rfind('\n', 0, NOTES_CHARS)
        text = text[:cut if cut > NOTES_CHARS // 2 else NOTES_CHARS].rstrip() + '\n...'
    return text


def asset_name(version):
    return f'Rekarina-Setup-{version}-windows.zip'


def read_release(record):
    """The cache fields of a GitHub release record; ValueError when it is not one."""
    if not isinstance(record, dict) or not isinstance(record.get('tag_name'), str):
        raise ValueError('no tag_name')
    version = record['tag_name'].strip().lstrip('vV')
    if not parse_version(version):
        raise ValueError(f'tag {record["tag_name"]!r} is not a version')
    asset, sums, sig = None, '', ''
    for a in record.get('assets') or []:
        if not isinstance(a, dict):
            continue
        if a.get('name') == asset_name(version) and a.get('browser_download_url'):
            asset = {'name': a['name'], 'url': a['browser_download_url'], 'size': int(a.get('size') or 0),
                     'digest': a.get('digest') or ''}
        elif a.get('name') == 'SHA256SUMS' and a.get('browser_download_url'):
            sums = a['browser_download_url']
        elif a.get('name') == 'SHA256SUMS.sig' and a.get('browser_download_url'):
            sig = a['browser_download_url']
    return {'latest': version, 'notes': trim_notes(record.get('body')), 'page': record.get('html_url') or '',
            'asset': asset, 'sums_url': sums, 'sig_url': sig}


def ask_release():
    """The newest release's cache fields: from the site's feed when there is one and it answers with a release
    record, else from GitHub. The errors of the GitHub request are the caller's to sort."""
    url = feed()
    if url:
        try:
            with net.open_url(url, CHECK_TIMEOUT, {'Accept': 'application/json'}) as r:
                return {**read_release(json.loads(r.read(4 << 20).decode('utf-8'))), 'source': 'feed'}
        except net.NET_ERRORS as e:
            if isinstance(e, urllib.error.HTTPError):
                e.close()
        except (ValueError, TypeError, UnicodeDecodeError):
            pass
    with net.open_url(f'{api()}/repos/{repo()}/releases/latest', CHECK_TIMEOUT,
                      {'Accept': 'application/vnd.github+json', 'X-GitHub-Api-Version': '2022-11-28'}) as r:
        return {**read_release(json.loads(r.read(4 << 20).decode('utf-8'))), 'source': 'github'}


def result(cache, current, status, reason='', root=None):
    latest = cache.get('latest') or ''
    available = newer(latest, current)   # also from an older answer when this check failed: the release is still out
    return {'status': 'available' if available else status, 'available': available, 'current': current,
            'version': latest, 'notes': cache.get('notes') or '', 'page': cache.get('page') or '',
            'asset': cache.get('asset'), 'sums_url': cache.get('sums_url') or '', 'sig_url': cache.get('sig_url') or '',
            'checked': cache.get('checked') or 0,
            'can_apply': can_apply(root), 'reason': reason}


def check(root=None, force=False, now=None):
    """Is a newer release out? Never raises and never waits longer than CHECK_TIMEOUT per source (the site's feed
    when there is one, then GitHub).

    Returns {'status': 'available' | 'current' | 'unknown', 'available', 'current', 'version' (the newest known, ''
    when none), 'notes', 'page', 'asset', 'sums_url', 'sig_url', 'checked', 'can_apply', 'reason'}. 'unknown' (offline, rate
    limit, no release published yet, an answer that makes no sense) carries its reason for the log, not for the
    player: the window shows nothing alarming. A failed check still says 'available' when an earlier answer named a
    newer release. force skips the six-hour reuse and a rate limit's waiting time."""
    current = net.local_version(root)
    now = int(time.time() if now is None else now)
    cache = load_cache()
    if not force:
        if cache.get('current') == current and cache.get('latest') and 0 <= now - int(cache.get('checked') or 0) < CACHE_SECONDS:
            return result(cache, current, 'current', root=root)
        if now < int(cache.get('retry_after') or 0):
            return result(cache, current, 'unknown', 'rate_limit', root=root)
    try:
        fields = ask_release()
    except urllib.error.HTTPError as e:
        e.close()
        if e.code in (403, 429):   # the hourly limit of requests without an account, shared by the player's address
            reset = e.headers.get('X-RateLimit-Reset', '') if e.headers else ''
            wait_until = int(reset) if reset.isdigit() else now + 3600
            save_cache({**cache, 'format': FORMAT, 'latest': cache.get('latest') or '', 'checked': int(cache.get('checked') or 0),
                        'retry_after': min(wait_until, now + 3600)})
            return result(cache, current, 'unknown', 'rate_limit', root=root)
        return result(cache, current, 'unknown', 'no_release' if e.code == 404 else f'http_{e.code}', root=root)
    except net.NET_ERRORS:
        return result(cache, current, 'unknown', 'offline', root=root)
    except (ValueError, TypeError, UnicodeDecodeError):
        return result(cache, current, 'unknown', 'bad_reply', root=root)
    cache = {'format': FORMAT, **fields, 'checked': now, 'current': current, 'available': newer(fields['latest'], current)}
    save_cache(cache)
    return result(cache, current, 'current', root=root)


# ---- download -----------------------------------------------------------------------------------------------------

def small_file(url):
    """A release's checksum list or its signature: at most 1 MiB. UpdateError when it does not arrive."""
    try:
        with net.open_url(url, 30) as r:
            return r.read(1 << 20)
    except net.NET_ERRORS as e:
        if isinstance(e, urllib.error.HTTPError):
            e.close()
        raise UpdateError('update.download_failed', reason=str(e) or type(e).__name__) from None


def signed_by_us(sums, signature_text):
    """True when the text of SHA256SUMS.sig (128 hex digits) is the signature of one of SIGNING_KEYS over sums."""
    try:
        signature = binascii.unhexlify(signature_text.strip())
    except (binascii.Error, ValueError):
        return False
    return any(ed25519.verify(key, sums, signature) for key in signing_keys())


def expected_sha256(info):
    """The SHA-256 the archive must have. With SIGNING_KEYS: its line in the release's SHA256SUMS, after that file's
    signature was checked (the module text, "Who vouches for a download"); UpdateError update.bad_signature when
    the release has no signature or another one. Without keys: the digest GitHub lists for the asset, else its line
    in SHA256SUMS; UpdateError when the release offers neither: an archive nobody vouches for is not installed."""
    asset = info.get('asset') or {}
    signed = bool(signing_keys())
    digest = (asset.get('digest') or '').strip().lower()
    if not signed and digest.startswith('sha256:') and re.fullmatch(r'[0-9a-f]{64}', digest[7:]):
        return digest[7:]
    if signed and not (info.get('sums_url') and info.get('sig_url')):
        raise UpdateError('update.bad_signature', name=asset.get('name') or '')
    if info.get('sums_url'):
        sums = small_file(info['sums_url'])
        if signed and not signed_by_us(sums, small_file(info['sig_url']).decode('ascii', 'replace')):
            raise UpdateError('update.bad_signature', name=asset.get('name') or '')
        text = sums.decode('utf-8', 'replace')
        for line in text.splitlines():
            m = re.fullmatch(r'\s*([0-9a-fA-F]{64})\s+\*?(.+?)\s*', line)
            if m and m.group(2) == asset.get('name'):
                return m.group(1).lower()
    raise UpdateError('update.unverified', name=asset.get('name') or '')


def download(info, folder, progress=None, cancel=None, sleep=None):
    """The setup archive of check()'s answer into folder: four tries with a growing pause, then the size and the
    SHA-256 must be the release's. Returns its path. UpdateError; net.Cancelled when cancel() said so."""
    asset = info.get('asset')
    if not asset or not asset.get('url'):
        raise UpdateError('update.no_asset', version=info.get('version') or '', name=asset_name(info.get('version') or ''))
    sha256 = expected_sha256(info)
    target = Path(folder) / asset['name']
    try:
        net.fetch_file(asset['url'], target, asset.get('size') or None, sha256, progress, cancel, tries=4, sleep=sleep)
    except net.DownloadError as e:
        raise UpdateError('update.download_failed', reason=str(e)) from None
    except OSError as e:
        raise UpdateError('update.write_failed', path=str(folder), reason=str(e)) from None
    return target


# ---- stage --------------------------------------------------------------------------------------------------------

def safe_parts(name):
    """The path parts of a zip member, or None for one that would leave the folder it is unpacked into."""
    parts = [p for p in name.replace('\\', '/').split('/') if p not in ('', '.')]
    if not parts or name.startswith(('/', '\\')) or any(p == '..' or ':' in p for p in parts):
        return None
    return parts


def stage(archive, root, version, progress=None, cancel=None):
    """Unpack the release archive into <root>/update-staging/new and mark it ready for the launcher exe's swap (the
    module text, step 1). The archive's one top folder (the release's name) is dropped. Checked before the mark is
    written: the archive holds the version it is named for and everything a launcher needs to start. Returns the
    staging folder. UpdateError."""
    root, archive = Path(root), Path(archive)
    staging = root / STAGING
    new = staging / NEW
    try:
        (staging / READY).unlink(missing_ok=True)   # first: a half-unpacked folder must never look ready
        shutil.rmtree(new, ignore_errors=True)
        if new.exists():
            raise UpdateError('update.write_failed', path=str(new), reason='the old staging folder could not be removed')
        with zipfile.ZipFile(archive) as z:
            members = [(m, safe_parts(m.filename)) for m in z.infolist() if not m.is_dir()]
            if any(parts is None for _, parts in members):
                raise UpdateError('update.bad_archive', reason='a member path leaves the folder')
            tops = {parts[0] for _, parts in members}
            strip = 1 if len(tops) == 1 and all(len(parts) > 1 for _, parts in members) else 0
            total = sum(m.file_size for m, _ in members)
            free = shutil.disk_usage(root).free
            if free < total + (64 << 20):
                raise UpdateError('update.no_space', path=str(root), need=round((total + (64 << 20)) / 1e9, 1),
                                  free=round(free / 1e9, 1))
            done = 0
            for m, parts in members:
                if cancel and cancel():
                    raise net.Cancelled
                rel = parts[strip:]
                if not rel or rel[0].lower() in PROTECTED or rel[0].lower().startswith('update-old'):
                    continue
                target = new.joinpath(*rel)
                target.parent.mkdir(parents=True, exist_ok=True)
                with z.open(m) as src, open(target, 'wb') as out:
                    shutil.copyfileobj(src, out, 1 << 20)
                try:   # the archive's file times, as an unpacker would set them
                    stamp = time.mktime(m.date_time + (0, 0, -1))
                    os.utime(target, (stamp, stamp))
                except (OverflowError, ValueError, OSError):
                    pass
                done += m.file_size
                if progress:
                    progress(done, total)
        lacking = [r for r in REQUIRED if not (new / r).is_file()]
        if lacking:
            raise UpdateError('update.bad_archive', reason='it lacks ' + ', '.join(lacking))
        staged = (new / 'VERSION').read_text(encoding='utf-8').strip()
        if parse_version(staged) != parse_version(version):
            raise UpdateError('update.bad_archive', reason=f'it holds version {staged}, not {version}')
        with open(staging / READY, 'w', encoding='utf-8', newline='\n') as f:
            f.write(staged + '\n')
    except zipfile.BadZipFile as e:
        raise UpdateError('update.bad_archive', reason=str(e)) from None
    except OSError as e:
        raise UpdateError('update.write_failed', path=str(staging), reason=str(e)) from None
    return new


def staged_version(root=None):
    """The version waiting in update-staging for the launcher exe's swap, or ''."""
    root = Path(root or net.ROOT)
    try:
        return (root / STAGING / READY).read_text(encoding='utf-8').strip() if (root / STAGING / NEW).is_dir() else ''
    except OSError:
        return ''


def install(info, root=None, report=None, cancel=None):
    """download() and stage() for the window: report(step, state, done, total) with the steps of STEPS. Returns the
    staging folder. UpdateError when the folder is not a release (a checkout only reports); net.Cancelled."""
    root = Path(root or net.ROOT)
    report = report or (lambda *a: None)
    if not can_apply(root):
        raise UpdateError('update.dev_only')
    folder = root / STAGING / 'download'
    try:
        shutil.rmtree(folder, ignore_errors=True)
        folder.mkdir(parents=True, exist_ok=True)
    except OSError as e:
        raise UpdateError('update.write_failed', path=str(folder), reason=str(e)) from None
    report('update_download', 'started', None, None)
    archive = download(info, folder, lambda done, total: report('update_download', 'progress', done, total), cancel)
    report('update_download', 'done', None, None)
    report('update_unpack', 'started', None, None)
    try:
        new = stage(archive, root, info['version'], lambda done, total: report('update_unpack', 'progress', done, total), cancel)
    finally:
        shutil.rmtree(folder, ignore_errors=True)
    report('update_unpack', 'done', None, None)
    return new


STEPS = ('update_download', 'update_unpack', 'update_restart')


def restart(root=None, arguments=()):
    """Start the launcher exe, which swaps a staged update in before it opens the window. The caller closes its own
    window right after: the swap waits a few seconds for this Python to end. False when the exe did not start."""
    exe = Path(root or net.ROOT) / LAUNCHER
    try:
        subprocess.Popen([str(exe), *arguments], cwd=str(exe.parent), close_fds=True,
                         creationflags=subprocess.DETACHED_PROCESS if os.name == 'nt' else 0)
        return True
    except OSError:
        return False


def cleanup(root=None):
    """Remove what a finished swap left: the old release's files in update-old-* and an empty staging folder. Files
    still in use (the launcher exe that did the swap, for a moment) stay for the next start."""
    root = Path(root or net.ROOT)
    for old in root.glob('update-old*'):
        shutil.rmtree(old, ignore_errors=True)
    if not staged_version(root):
        shutil.rmtree(root / STAGING, ignore_errors=True)
