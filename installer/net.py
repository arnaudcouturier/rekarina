"""HTTP for the launcher: what update.py and components.py ask the network for.

urllib only. Every request carries one header that says what is asking, `User-Agent: Rekarina-Launcher/<version>`
(the VERSION file beside installer/), and nothing about the player or the machine: no account, no folder name, no
cookie, no query string.

    open_url(url, timeout)                 the response, with the header above
    fetch_bytes(url, timeout, log)         a small file's bytes; when Python's certificate check fails, through
                                           Windows' own TLS (windows_fetch): the caller checks what arrives
    fetch_file(url, path, size, sha256)    a file, retried with a growing pause, checked before it gets its name
    RemoteZip(url, size)                   single members of a zip on a server that answers range requests, so a 9 MiB
                                           library does not cost the 276 MiB archive around it
"""
import hashlib
import http.client
import os
import re
import ssl
import struct
import subprocess
import tempfile
import time
import urllib.error
import urllib.request
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CHUNK = 1 << 18
TRIES = 4
# URLError and socket timeouts are OSErrors; a connection cut mid-file raises http.client.IncompleteRead, which is not
NET_ERRORS = (OSError, http.client.HTTPException)
pause = time.sleep   # the wait between two tries; the checks replace it


class DownloadError(Exception):
    """A download that did not arrive, or arrived as another file than the pinned one."""


class Cancelled(Exception):
    """The caller's cancel() said to stop."""


def local_version(root=None):
    """The release string of this folder: its VERSION file (a release folder's root, a checkout's src)."""
    try:
        return (Path(root or ROOT) / 'VERSION').read_text(encoding='utf-8').strip() or '0'
    except OSError:
        return '0'


def user_agent(root=None):
    return 'Rekarina-Launcher/' + (re.sub(r'[^0-9A-Za-z.+_-]', '', local_version(root)) or '0')


def open_url(url, timeout=30, headers=None):
    request = urllib.request.Request(url, headers={'User-Agent': user_agent(), **(headers or {})})
    return urllib.request.urlopen(request, timeout=timeout)


def cert_failure(error):
    """Whether error is Python's TLS certificate check failing: an antivirus or a proxy that scans HTTPS with a root of
    its own, or a server chain without its intermediate (Windows fetches a missing one, Python's OpenSSL does not).
    A player's setup stopped there at its first download (CERTIFICATE_VERIFY_FAILED, unable to get local issuer
    certificate)."""
    e = getattr(error, 'reason', error)
    return isinstance(e, ssl.SSLCertVerificationError) or (isinstance(e, ssl.SSLError) and
                                                           'CERTIFICATE_VERIFY_FAILED' in str(e))


# PowerShell's Invoke-WebRequest: .NET on SChannel, which checks the certificate against the Windows store the way
# every other program of the PC does. The address, the target file and the header go in through the environment, so
# nothing is quoted into the command.
WIN_FETCH = ("$ProgressPreference = 'SilentlyContinue'; "
             "[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor "
             "[Net.SecurityProtocolType]::Tls12; "
             "Invoke-WebRequest -UseBasicParsing -Uri $env:REKARINA_FETCH_URL -OutFile $env:REKARINA_FETCH_OUT "
             "-UserAgent $env:REKARINA_FETCH_UA -TimeoutSec ([int]$env:REKARINA_FETCH_TIMEOUT)")


def windows_fetch(url, timeout=60):
    """The bytes at url through Windows' own TLS stack and certificate store (WIN_FETCH). OSError when that fails too
    or there is no Windows."""
    if os.name != 'nt':
        raise OSError('no other TLS stack than Python\'s here')
    fd, out = tempfile.mkstemp(prefix='rekarina-fetch-')
    os.close(fd)
    try:
        env = dict(os.environ, REKARINA_FETCH_URL=url, REKARINA_FETCH_OUT=out, REKARINA_FETCH_UA=user_agent(),
                   REKARINA_FETCH_TIMEOUT=str(int(timeout)))
        try:
            r = subprocess.run(['powershell.exe', '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-Command',
                                WIN_FETCH], env=env, capture_output=True, text=True, errors='replace', timeout=timeout + 30,
                               creationflags=0x08000000)   # CREATE_NO_WINDOW
        except subprocess.TimeoutExpired:
            raise OSError(f'Windows did not fetch {url} within {timeout + 30} s') from None
        if r.returncode:
            said = [line.strip() for line in (r.stderr or r.stdout or '').splitlines() if line.strip()]
            raise OSError(f'Windows could not fetch {url} either: {said[0] if said else "exit code " + str(r.returncode)}')
        return Path(out).read_bytes()
    finally:
        try:
            os.unlink(out)
        except OSError:
            pass


def fetch_bytes(url, timeout=30, log=None, context=None, windows=windows_fetch):
    """(the bytes at url, 'python' or 'windows'). Python's urllib first; when its certificate check fails, Windows'
    own TLS (windows_fetch), and log(text) says so. Only for what the caller checks itself (a checksum it trusts), as
    it does for anything it downloads. context: urllib's SSL context (a test's)."""
    request = urllib.request.Request(url, headers={'User-Agent': user_agent()})
    try:
        with urllib.request.urlopen(request, timeout=timeout, context=context) as r:
            return r.read(), 'python'
    except NET_ERRORS as e:
        if not cert_failure(e):
            raise
        if log:
            log(f'{url}: Python\'s certificate check failed ({getattr(e, "reason", e)}): fetched through Windows\' own '
                f'TLS instead')
        return windows(url, timeout * 2), 'windows'


def permanent(error):
    """An answer that the same request will get again: a missing file, a refused one. An HTTP error is a response of
    its own: it is closed here, where every failed request passes."""
    if not isinstance(error, urllib.error.HTTPError):
        return False
    error.close()
    return 400 <= error.code < 500 and error.code not in (408, 429)


def fetch_file(url, path, size=None, sha256=None, progress=None, cancel=None, tries=TRIES, timeout=30, sleep=None):
    """Download url to path. The bytes go to <path>.part and get the name only when the size and the SHA-256 (each
    checked when given) are right, so a file under its name is always whole. A failed try is repeated after 1, 2, 4...
    seconds, tries times in all; an answer that will not change (404) is not. progress(done, total) and cancel() are
    called per block. Returns the SHA-256 of the file; DownloadError when it could not be had."""
    path = Path(path)
    part = path.with_name(path.name + '.part')
    path.parent.mkdir(parents=True, exist_ok=True)
    reason = 'no try was made'
    for attempt in range(tries):
        if attempt:
            (sleep or pause)(2 ** (attempt - 1))
        try:
            h, done = hashlib.sha256(), 0
            with open_url(url, timeout) as r, open(part, 'wb') as f:
                total = size or int(r.headers.get('Content-Length') or 0) or None
                while block := r.read(CHUNK):
                    if cancel and cancel():
                        raise Cancelled
                    f.write(block)
                    h.update(block)
                    done += len(block)
                    if progress:
                        progress(done, total)
            if size is not None and done != size:
                raise DownloadError(f'{done} bytes arrived, {size} expected')
            if sha256 and h.hexdigest() != sha256.lower():
                raise DownloadError(f'the SHA-256 is {h.hexdigest()}, not the expected {sha256.lower()}')
            part.replace(path)
            return h.hexdigest()
        except Cancelled:
            part.unlink(missing_ok=True)
            raise
        except (DownloadError, *NET_ERRORS) as e:
            part.unlink(missing_ok=True)
            reason = str(e) or type(e).__name__
            if permanent(e):
                break
    raise DownloadError(f'{url}: {reason}')


class RemoteZip:
    """Members of a zip file read with range requests: the directory at its end first, then one member at a time.
    RangeUnsupported when the server answers a range with the whole file, or the archive is a zip64 one (the caller
    then downloads the archive)."""

    class RangeUnsupported(Exception):
        pass

    def __init__(self, url, size, timeout=30, tries=TRIES, sleep=None):
        self.url, self.size, self.timeout, self.tries, self.sleep = url, size, timeout, tries, sleep
        self.members = None

    def span(self, first, last):
        """Bytes first..last of the archive; a failed request is repeated like fetch_file's."""
        for attempt in range(self.tries):
            if attempt:
                (self.sleep or pause)(2 ** (attempt - 1))
            try:
                with open_url(self.url, self.timeout, {'Range': f'bytes={first}-{last}'}) as r:
                    if getattr(r, 'status', None) != 206:
                        raise self.RangeUnsupported('the server sent no partial content')
                    data = r.read()
                if len(data) == last - first + 1:
                    return data
                reason = f'{len(data)} bytes of a {last - first + 1} byte range arrived'
            except NET_ERRORS as e:
                reason = str(e) or type(e).__name__
                if permanent(e):
                    break
        raise DownloadError(f'{self.url}: {reason}')

    def directory(self):
        """{member name: (method, compressed size, size, offset of its local header)}"""
        if self.members is None:
            tail_at = max(0, self.size - 65557)   # the end record and the longest comment it may carry
            tail = self.span(tail_at, self.size - 1)
            end = tail.rfind(b'PK\x05\x06')
            if end < 0:
                raise DownloadError(f'{self.url}: no zip directory at its end')
            count, cd_size, cd_at = struct.unpack_from('<HII', tail, end + 10)
            if count == 0xffff or cd_at == 0xffffffff or cd_size == 0xffffffff:
                raise self.RangeUnsupported('a zip64 archive')
            cd = tail[cd_at - tail_at:cd_at - tail_at + cd_size] if cd_at >= tail_at else self.span(cd_at, cd_at + cd_size - 1)
            members, p = {}, 0
            while p + 46 <= len(cd) and cd[p:p + 4] == b'PK\x01\x02':
                method, = struct.unpack_from('<H', cd, p + 10)
                packed, size = struct.unpack_from('<II', cd, p + 20)
                name_len, extra_len, comment_len = struct.unpack_from('<HHH', cd, p + 28)
                header_at, = struct.unpack_from('<I', cd, p + 42)
                members[cd[p + 46:p + 46 + name_len].decode('utf-8', 'replace')] = (method, packed, size, header_at)
                p += 46 + name_len + extra_len + comment_len
            self.members = members
        return self.members

    def read(self, name):
        """The bytes of one member."""
        if name not in self.directory():
            raise DownloadError(f'{self.url}: no member {name}')
        method, packed, size, at = self.members[name]
        # the local header repeats the name and has an extra field of its own length: read past both in one request
        slack = 30 + len(name.encode()) + 1024
        blob = self.span(at, min(self.size - 1, at + slack + packed - 1))
        if blob[:4] != b'PK\x03\x04':
            raise DownloadError(f'{self.url}: {name} has no local header where the directory points')
        name_len, extra_len = struct.unpack_from('<HH', blob, 26)
        start = 30 + name_len + extra_len
        if start + packed > len(blob):
            blob += self.span(at + len(blob), at + start + packed - 1)
        raw = blob[start:start + packed]
        if method == 0:
            data = raw
        elif method == 8:
            try:
                data = zlib.decompressobj(-15).decompress(raw)
            except zlib.error as e:
                raise DownloadError(f'{self.url}: {name} does not unpack: {e}') from None
        else:
            raise DownloadError(f'{self.url}: {name} is packed with method {method}')
        if len(data) != size:
            raise DownloadError(f'{self.url}: {name} unpacked to {len(data)} bytes, not {size}')
        return data
