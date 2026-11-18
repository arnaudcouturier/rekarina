"""Checks of the launcher's own parts: update.py, components.py, eta.py, net.py and what oot.py builds on them.

    python -m unittest installer.launcher_test        (from the folder that holds installer/)

No game, no graphics driver, no internet: a release record, a setup archive and component files are served from
127.0.0.1 by a server of this file, and the application data folder (update.json, timings.json) is a temp folder.
"""
import hashlib
import http.server
import io
import json
import os
import struct
import tempfile
import threading
import time
import unittest
import zipfile
from pathlib import Path

from . import components, ed25519, eta, net, oot, update

GIB = 1 << 30


class Server:
    """A local HTTP server: routes[path] is (status, headers, body) or a function(handler) returning one. With
    ranges=True a Range header is honoured (206), as GitHub's release storage does."""

    def __init__(self, ranges=False):
        self.routes, self.hits, self.agents = {}, [], []
        outer = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                outer.hits.append(self.path)
                outer.agents.append(self.headers.get('User-Agent'))
                route = outer.routes.get(self.path)
                if route is None:
                    self.send_error(404)
                    return
                status, headers, body = route(self) if callable(route) else route
                span = self.headers.get('Range')
                if ranges and status == 200 and span and span.startswith('bytes='):
                    first, _, last = span[6:].partition('-')
                    first, last = int(first), min(int(last), len(body) - 1)
                    headers = {**headers, 'Content-Range': f'bytes {first}-{last}/{len(body)}'}
                    status, body = 206, body[first:last + 1]
                self.send_response(status)
                for k, v in headers.items():
                    self.send_header(k, v)
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *args):
                pass

        self.httpd = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        self.url = f'http://127.0.0.1:{self.httpd.server_address[1]}'
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def close(self):
        self.httpd.shutdown()
        self.httpd.server_close()


def release_zip(version, top='Rekarina-Setup', extra=None):
    """A stand-in for a release archive: the files update.REQUIRED names and a few more."""
    files = {'VERSION': version + '\n', 'Rekarina-Launcher.exe': 'new launcher', 'LICENSE': 'MIT',
             'python/pythonw.exe': 'new pythonw', 'python/python.exe': 'new python', 'installer/gui_oot.py': '# new',
             'installer/update.py': '# new', 'tools/package_oot.py': '# new', 'temp/should-not-land.txt': 'x'}
    files.update(extra or {})
    data = io.BytesIO()
    with zipfile.ZipFile(data, 'w', zipfile.ZIP_DEFLATED) as z:
        for name, text in files.items():
            z.writestr(f'{top}/{name}' if top else name, text)
    return data.getvalue()


def release_record(version, archive, digest=True, sums=False, base='', sig=False):
    name = update.asset_name(version)
    assets = [{'name': name, 'size': len(archive), 'browser_download_url': f'{base}/download/{name}',
               'digest': 'sha256:' + hashlib.sha256(archive).hexdigest() if digest else None}]
    if sums:
        assets.append({'name': 'SHA256SUMS', 'size': 1, 'browser_download_url': f'{base}/download/SHA256SUMS'})
    if sig:
        assets.append({'name': 'SHA256SUMS.sig', 'size': 1, 'browser_download_url': f'{base}/download/SHA256SUMS.sig'})
    return {'tag_name': 'v' + version, 'html_url': f'https://github.com/{update.REPO}/releases/tag/v{version}',
            'body': 'Notes\r\n- one\r\n- two\r\n', 'assets': assets}


class Sandbox(unittest.TestCase):
    """A temp folder as the application data folder and a local server as GitHub."""

    ranges = False

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        self.server = Server(self.ranges)
        self.saved = {k: os.environ.get(k) for k in ('APPDATA', 'XDG_CONFIG_HOME', 'REKARINA_UPDATE_API', 'REKARINA_REPO',
                                                     'REKARINA_UPDATE_FEED', 'NO_PROXY')}
        self.keys, self.feed = update.SIGNING_KEYS, update.FEED
        update.FEED = ''                        # no test asks the real site: a feed is named per test
        update.SIGNING_KEYS = ()                # and none needs the project's private key: SignedTest sets its own
        os.environ.update(APPDATA=str(self.dir / 'appdata'), XDG_CONFIG_HOME=str(self.dir / 'appdata'),
                          REKARINA_UPDATE_API=self.server.url, NO_PROXY='127.0.0.1,localhost')
        os.environ.pop('REKARINA_REPO', None)
        os.environ.pop('REKARINA_UPDATE_FEED', None)
        self.latest = f'/repos/{update.REPO}/releases/latest'

    def tearDown(self):
        update.SIGNING_KEYS, update.FEED = self.keys, self.feed
        self.server.close()
        for k, v in self.saved.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v
        self.tmp.cleanup()

    def root(self, version='1.0.0', release=True):
        """A folder that looks like an unpacked release (or, without the Python, like a checkout)."""
        root = self.dir / 'release'
        (root / 'installer').mkdir(parents=True, exist_ok=True)
        (root / 'VERSION').write_bytes(version.encode() + b'\n')
        if release:
            (root / 'python').mkdir(exist_ok=True)
            for name in ('python/pythonw.exe', 'python/python.exe', 'Rekarina-Launcher.exe'):
                (root / name).write_bytes(b'old')
        return root

    def serve_release(self, version, archive=None, **kw):
        archive = archive if archive is not None else release_zip(version)
        record = release_record(version, archive, base=self.server.url, **kw)
        self.server.routes[self.latest] = (200, {'Content-Type': 'application/json'}, json.dumps(record).encode())
        self.server.routes[f'/download/{update.asset_name(version)}'] = (200, {}, archive)
        return archive


class VersionTest(unittest.TestCase):
    def test_parse(self):
        self.assertEqual(update.parse_version('v1.2.10'), (1, 2, 10))
        self.assertEqual(update.parse_version('1.0.0-rc1'), (1, 0, 0))
        self.assertEqual(update.parse_version('1.0.0+abc123'), (1, 0, 0))
        self.assertEqual(update.parse_version('nightly'), ())

    def test_newer(self):
        self.assertTrue(update.newer('1.0.1', '1.0.0'))
        self.assertTrue(update.newer('v1.10.0', '1.9.9'))      # numbers, not text: 10 is after 9
        self.assertTrue(update.newer('2', '1.9'))
        self.assertFalse(update.newer('1.0', '1.0.0'))         # the same release
        self.assertFalse(update.newer('1.0.0', '1.0.1'))
        self.assertFalse(update.newer('', '1.0.0'))
        self.assertFalse(update.newer('garbage', '0.1'))
        self.assertTrue(update.newer('1.0.0', '0.3.10-dev'))


class CheckTest(Sandbox):
    def test_available_then_cached(self):
        root = self.root('1.0.0')
        self.serve_release('1.0.1')
        info = update.check(root, now=1000)
        self.assertEqual((info['status'], info['available'], info['version'], info['current']), ('available', True, '1.0.1', '1.0.0'))
        self.assertEqual(info['notes'], 'Notes\n- one\n- two')
        self.assertEqual(info['can_apply'], os.name == 'nt')                      # a release folder, not this checkout
        self.assertEqual(info['asset']['name'], 'Rekarina-Setup-1.0.1-windows.zip')
        self.assertEqual(self.server.agents[-1], net.user_agent())               # the one thing a request says
        self.assertRegex(net.user_agent(), r'^Rekarina-Launcher/[0-9A-Za-z.+_-]+$')
        # the file the game reads
        stored = json.loads(update.state_path().read_text(encoding='utf-8'))
        self.assertEqual((stored['latest'], stored['checked']), ('1.0.1', 1000))
        self.assertNotIn(b'\r', update.state_path().read_bytes())
        # within six hours the answer is reused, after them asked again; force asks at once
        self.assertEqual(update.check(root, now=1000 + 6 * 3600 - 1)['version'], '1.0.1')
        self.assertEqual(len(self.server.hits), 1)
        update.check(root, now=1000 + 6 * 3600 + 1)
        self.assertEqual(len(self.server.hits), 2)
        update.check(root, force=True, now=1000 + 6 * 3600 + 2)
        self.assertEqual(len(self.server.hits), 3)

    def test_current(self):
        root = self.root('1.0.1')
        self.serve_release('1.0.1')
        info = update.check(root, now=5)
        self.assertEqual((info['status'], info['available']), ('current', False))

    def test_other_version_asks_again(self):
        self.serve_release('1.0.1')
        update.check(self.root('1.0.0'), now=10)
        info = update.check(self.root('1.0.1'), now=20)   # the launcher was updated: the old answer is not its own
        self.assertEqual((info['status'], len(self.server.hits)), ('current', 2))

    def test_quiet_failures(self):
        root = self.root('1.0.0')
        self.assertEqual(update.check(root, now=1)['reason'], 'no_release')          # 404: nothing published yet
        self.server.routes[self.latest] = (200, {}, b'<html>not json')
        self.assertEqual(update.check(root, now=2)['reason'], 'bad_reply')
        self.server.routes[self.latest] = (200, {}, json.dumps({'tag_name': 'nightly'}).encode())
        self.assertEqual(update.check(root, now=3)['reason'], 'bad_reply')
        self.server.routes[self.latest] = (500, {}, b'')
        self.assertEqual(update.check(root, now=4)['status'], 'unknown')
        os.environ['REKARINA_UPDATE_API'] = 'http://127.0.0.1:9'                     # nothing listens: offline
        info = update.check(root, now=5)
        self.assertEqual((info['status'], info['reason'], info['available']), ('unknown', 'offline', False))

    def test_rate_limit_waits(self):
        root = self.root('1.0.0')
        self.server.routes[self.latest] = (403, {'X-RateLimit-Reset': '2000'}, b'{}')
        self.assertEqual(update.check(root, now=1000)['reason'], 'rate_limit')
        self.assertEqual(update.check(root, now=1500)['reason'], 'rate_limit')
        self.assertEqual(len(self.server.hits), 1)             # the second check did not ask
        self.serve_release('1.0.1')
        self.assertEqual(update.check(root, now=2001)['version'], '1.0.1')

    def test_failure_keeps_known_release(self):
        root = self.root('1.0.0')
        self.serve_release('1.0.1')
        update.check(root, now=10)
        self.server.routes[self.latest] = (500, {}, b'')
        info = update.check(root, force=True, now=20)
        self.assertEqual((info['status'], info['version']), ('available', '1.0.1'))   # the release is still out
        self.assertEqual(json.loads(update.state_path().read_text(encoding='utf-8'))['latest'], '1.0.1')

    def test_repo_override(self):
        os.environ['REKARINA_REPO'] = 'someone/fork'
        self.assertEqual(update.repo(), 'someone/fork')
        os.environ['REKARINA_REPO'] = 'not a repo; rm -rf'
        self.assertEqual(update.repo(), update.REPO)

    def test_checkout_only_reports(self):
        root = self.root('1.0.0', release=False)
        self.serve_release('1.0.1')
        info = update.check(root, now=1)
        self.assertEqual((info['available'], info['can_apply']), (True, False))
        with self.assertRaises(update.UpdateError) as e:
            update.install(info, root)
        self.assertEqual(e.exception.id, 'update.dev_only')
        release = self.root('1.0.0')
        (release / '.git').mkdir()
        self.assertFalse(update.can_apply(release))             # a checkout that also has a bundled Python


class DownloadTest(Sandbox):
    def info(self, version='1.0.1', **kw):
        archive = self.serve_release(version, **kw)
        return update.check(self.root('1.0.0'), force=True, now=1), archive

    def test_digest(self):
        info, archive = self.info()
        seen = []
        path = update.download(info, self.dir / 'dl', progress=lambda done, total: seen.append((done, total)))
        self.assertEqual(path.read_bytes(), archive)
        self.assertEqual(seen[-1], (len(archive), len(archive)))
        self.assertFalse(list((self.dir / 'dl').glob('*.part')))

    def test_wrong_digest_fails_after_four_tries(self):
        info, archive = self.info()
        name = f'/download/{update.asset_name("1.0.1")}'
        self.server.routes[name] = (200, {}, archive[:-1] + b'X')      # same size, other bytes
        waits = []
        with self.assertRaises(update.UpdateError) as e:
            update.download(info, self.dir / 'dl', sleep=waits.append)
        self.assertEqual(e.exception.id, 'update.download_failed')
        self.assertEqual(self.server.hits.count(name), 4)
        self.assertEqual(waits, [1, 2, 4])
        self.assertFalse(list((self.dir / 'dl').glob('*')))            # nothing under the archive's name, no .part

    def test_retry_then_success(self):
        info, archive = self.info()
        name = f'/download/{update.asset_name("1.0.1")}'
        calls = []

        def flaky(handler):
            calls.append(1)
            return (503, {}, b'') if len(calls) < 3 else (200, {}, archive)
        self.server.routes[name] = flaky
        path = update.download(info, self.dir / 'dl', sleep=lambda s: None)
        self.assertEqual((path.read_bytes(), len(calls)), (archive, 3))

    def test_sha256sums(self):
        info, archive = self.info(digest=False, sums=True)
        good = f'{hashlib.sha256(b"other").hexdigest()}  other.zip\n{hashlib.sha256(archive).hexdigest()} *{info["asset"]["name"]}\n'
        self.server.routes['/download/SHA256SUMS'] = (200, {}, good.encode())
        self.assertEqual(update.download(info, self.dir / 'dl').read_bytes(), archive)
        self.server.routes['/download/SHA256SUMS'] = (200, {}, f'{"0" * 64}  {info["asset"]["name"]}\n'.encode())
        with self.assertRaises(update.UpdateError):
            update.download(info, self.dir / 'dl2', sleep=lambda s: None)

    def test_unverified_is_refused(self):
        info, _ = self.info(digest=False)
        with self.assertRaises(update.UpdateError) as e:
            update.download(info, self.dir / 'dl')
        self.assertEqual(e.exception.id, 'update.unverified')

    def test_cancel(self):
        info, _ = self.info()
        with self.assertRaises(net.Cancelled):
            update.download(info, self.dir / 'dl', cancel=lambda: True)
        self.assertFalse(list((self.dir / 'dl').glob('*')))


class Ed25519Test(unittest.TestCase):
    # RFC 8032, section 7.1: private seed, public key, message, signature
    VECTORS = (
        ('9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60',
         'd75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a', '',
         'e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b'),
        ('4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb',
         '3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c', '72',
         '92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00'),
        ('c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7',
         'fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025', 'af82',
         '6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a'),
    )

    def test_rfc_vectors(self):
        for seed, public, message, signature in self.VECTORS:
            seed, public, message, signature = (bytes.fromhex(x) for x in (seed, public, message, signature))
            self.assertEqual(ed25519.public_key(seed), public)
            self.assertEqual(ed25519.sign(seed, message), signature)
            self.assertTrue(ed25519.verify(public, message, signature))

    def test_refusals(self):
        seed, public, message, signature = (bytes.fromhex(x) for x in self.VECTORS[1])
        self.assertFalse(ed25519.verify(public, message + b'x', signature))                       # another message
        self.assertFalse(ed25519.verify(public, message, signature[:5] + b'\0' + signature[6:]))  # a changed signature
        self.assertFalse(ed25519.verify(bytes.fromhex(self.VECTORS[0][1]), message, signature))   # another key
        self.assertFalse(ed25519.verify(public, message, signature[:63]))                         # a short one
        high = signature[:32] + (int.from_bytes(signature[32:], 'little') + ed25519.ORDER).to_bytes(32, 'little')
        self.assertFalse(ed25519.verify(public, message, high))                                   # s not below the order
        self.assertFalse(ed25519.verify(b'\xff' * 32, message, signature))                        # no point of the curve


class FeedTest(Sandbox):
    """The project's own site as the first source (update.FEED / REKARINA_UPDATE_FEED), GitHub behind it."""

    def serve_feed(self, version, archive=None):
        archive = archive if archive is not None else release_zip(version)
        record = release_record(version, archive, base=self.server.url)
        self.server.routes['/releases/latest.json'] = (200, {'Content-Type': 'application/json'}, json.dumps(record).encode())
        self.server.routes[f'/download/{update.asset_name(version)}'] = (200, {}, archive)
        os.environ['REKARINA_UPDATE_FEED'] = self.server.url + '/releases/latest.json'
        return archive

    def test_feed_first(self):
        root = self.root('1.0.0')
        self.serve_release('1.0.1')             # what GitHub has
        archive = self.serve_feed('1.0.2')      # what the site has
        info = update.check(root, now=10)
        self.assertEqual((info['version'], info['available']), ('1.0.2', True))
        self.assertEqual(self.server.hits, ['/releases/latest.json'])            # GitHub was not asked
        self.assertEqual(update.download(info, self.dir / 'dl').read_bytes(), archive)

    def test_falls_back_to_github(self):
        root = self.root('1.0.0')
        self.serve_release('1.0.1')
        os.environ['REKARINA_UPDATE_FEED'] = self.server.url + '/releases/latest.json'   # 404: the site has no record
        self.assertEqual(update.check(root, now=10)['version'], '1.0.1')
        self.server.routes['/releases/latest.json'] = (200, {}, b'<html>a parked domain')
        self.assertEqual(update.check(root, force=True, now=20)['version'], '1.0.1')
        os.environ['REKARINA_UPDATE_FEED'] = 'http://127.0.0.1:9/latest.json'            # the site is down
        self.assertEqual(update.check(root, force=True, now=30)['version'], '1.0.1')

    def test_no_source_answers(self):
        os.environ['REKARINA_UPDATE_FEED'] = self.server.url + '/releases/latest.json'
        info = update.check(self.root('1.0.0'), now=10)
        self.assertEqual((info['status'], info['reason']), ('unknown', 'no_release'))


class SignedTest(Sandbox):
    """With a key in update.SIGNING_KEYS only a release whose SHA256SUMS that key signed is downloaded."""
    SEED = bytes(range(32))

    def setUp(self):
        super().setUp()
        update.SIGNING_KEYS = (ed25519.public_key(self.SEED).hex(),)

    def release(self, sums=None, signature=None, seed=None, **kw):
        archive = self.serve_release('1.0.1', sums=True, sig=True, **kw)
        text = (f'{hashlib.sha256(archive).hexdigest()}  {update.asset_name("1.0.1")}\n'.encode() if sums is None else sums)
        self.server.routes['/download/SHA256SUMS'] = (200, {}, text)
        if signature is None:
            signature = ed25519.sign(seed or self.SEED, text).hex().encode() + b'\n'
        self.server.routes['/download/SHA256SUMS.sig'] = (200, {}, signature)
        return update.check(self.root('1.0.0'), force=True, now=1), archive

    def refused(self, info, id='update.bad_signature'):
        with self.assertRaises(update.UpdateError) as e:
            update.download(info, self.dir / 'dl', sleep=lambda s: None)
        self.assertEqual(e.exception.id, id)
        self.assertNotIn(f'/download/{update.asset_name("1.0.1")}', self.server.hits)   # the archive was not even asked for

    def test_signed_release(self):
        info, archive = self.release()
        self.assertEqual(update.download(info, self.dir / 'dl').read_bytes(), archive)

    def test_second_key(self):
        update.SIGNING_KEYS = (ed25519.public_key(b'k' * 32).hex(), ed25519.public_key(self.SEED).hex())
        info, archive = self.release()
        self.assertEqual(update.download(info, self.dir / 'dl').read_bytes(), archive)

    def test_other_key(self):
        self.refused(self.release(seed=b'x' * 32)[0])

    def test_sums_changed_after_signing(self):
        good = f'{"1" * 64}  {update.asset_name("1.0.1")}\n'.encode()
        signature = ed25519.sign(self.SEED, good).hex().encode()
        self.refused(self.release(sums=good.replace(b'1', b'2', 1), signature=signature)[0])

    def test_garbage_signature(self):
        self.refused(self.release(signature=b'not hex')[0])
        self.refused(self.release(signature=b'')[0])

    def test_unsigned_release(self):
        archive = self.serve_release('1.0.1', sums=True)    # a digest and SHA256SUMS, no signature
        self.server.routes['/download/SHA256SUMS'] = (
            200, {}, f'{hashlib.sha256(archive).hexdigest()}  {update.asset_name("1.0.1")}\n'.encode())
        self.refused(update.check(self.root('1.0.0'), force=True, now=1))

    def test_archive_not_in_signed_sums(self):
        info, _ = self.release(sums=f'{"1" * 64}  other.zip\n'.encode())
        self.refused(info, 'update.unverified')

    def test_other_archive_than_signed(self):
        info, archive = self.release()
        name = f'/download/{update.asset_name("1.0.1")}'
        self.server.routes[name] = (200, {}, archive[:-1] + b'X')
        with self.assertRaises(update.UpdateError) as e:
            update.download(info, self.dir / 'dl', sleep=lambda s: None)
        self.assertEqual(e.exception.id, 'update.download_failed')
        self.assertFalse(list((self.dir / 'dl').glob('*')))


class StageTest(Sandbox):
    def stage(self, archive, version='1.0.1'):
        root = self.root('1.0.0')
        (root / 'temp' / 'oot-12345678').mkdir(parents=True, exist_ok=True)
        (root / 'temp' / 'oot-12345678' / 'keep.txt').write_bytes(b'generated code')
        path = self.dir / 'a.zip'
        path.write_bytes(archive)
        return root, path, version

    def test_stage(self):
        root, path, version = self.stage(release_zip('1.0.1'))
        new = update.stage(path, root, version)
        self.assertEqual(new, root / 'update-staging' / 'new')
        self.assertEqual((new / 'VERSION').read_text().strip(), '1.0.1')            # the top folder is dropped
        self.assertEqual((new / 'python' / 'pythonw.exe').read_bytes(), b'new pythonw')
        self.assertFalse((new / 'temp').exists())                                  # the player's build files are not an update's
        self.assertEqual(update.staged_version(root), '1.0.1')
        self.assertEqual((root / 'VERSION').read_text().strip(), '1.0.0')           # nothing of the running release moved
        self.assertEqual((root / 'temp' / 'oot-12345678' / 'keep.txt').read_bytes(), b'generated code')
        update.cleanup(root)
        self.assertTrue((root / 'update-staging' / 'new').is_dir())                # a ready update survives a cleanup

    def test_flat_archive(self):
        root, path, version = self.stage(release_zip('1.0.1', top=''))
        self.assertEqual((update.stage(path, root, version) / 'VERSION').read_text().strip(), '1.0.1')

    def test_refused(self):
        data = io.BytesIO()
        with zipfile.ZipFile(data, 'w') as z:   # an archive without the Python that would start the window
            z.writestr('Rekarina-Setup/VERSION', '1.0.1')
            z.writestr('Rekarina-Setup/installer/gui_oot.py', '')
        for archive, why in ((release_zip('1.0.2'), 'version'), (b'not a zip', 'zip'),
                             (release_zip('1.0.1', extra={'../outside.txt': 'x'}), 'leaves'), (data.getvalue(), 'lacks')):
            root, path, version = self.stage(archive)
            with self.assertRaises(update.UpdateError, msg=why) as e:
                update.stage(path, root, version)
            self.assertEqual(e.exception.id, 'update.bad_archive', why)
            self.assertEqual(update.staged_version(root), '', why)                 # never marked ready
            self.assertFalse((self.dir / 'outside.txt').exists())

    @unittest.skipUnless(os.name == 'nt', 'the launcher updates itself on Windows only')
    def test_install(self):
        root = self.root('1.0.0')
        archive = self.serve_release('1.0.1')
        info = update.check(root, now=1)
        steps = []
        new = update.install(info, root, lambda step, state, done, total: steps.append((step, state)))
        self.assertEqual((new / 'Rekarina-Launcher.exe').read_bytes(), b'new launcher')
        self.assertEqual([s for s in steps if s[1] != 'progress'],
                         [('update_download', 'started'), ('update_download', 'done'), ('update_unpack', 'started'),
                          ('update_unpack', 'done')])
        self.assertFalse((root / 'update-staging' / 'download').exists())           # the archive is gone once unpacked
        self.assertEqual(update.staged_version(root), '1.0.1')
        self.assertGreater(len(archive), 0)


class GpuTest(unittest.TestCase):
    def test_rtx_generation(self):
        for name, gen in (('NVIDIA GeForce RTX 5070', 50), ('NVIDIA GeForce RTX 4070 SUPER', 40),
                          ('NVIDIA GeForce RTX 3080 Ti', 30), ('NVIDIA GeForce RTX 2060', 20),
                          ('NVIDIA GeForce RTX 3050 Ti Laptop GPU', 30), ('NVIDIA GeForce GTX 1660 Ti', None),
                          ('NVIDIA GeForce GTX 1080', None), ('NVIDIA Quadro RTX 4000', 20), ('NVIDIA TITAN RTX', 20),
                          ('NVIDIA RTX A4000', 30), ('NVIDIA RTX 4000 Ada Generation', 40),
                          ('NVIDIA RTX 2000 Ada Generation Laptop GPU', 40), ('NVIDIA RTX PRO 6000 Blackwell', 50),
                          ('NVIDIA GeForce RTX(TM) 4060', 40)):
            self.assertEqual(components.rtx_generation(name), gen, name)

    def test_vendor_and_integrated(self):
        self.assertEqual(components.vendor_of('Whatever', r'PCI\VEN_10DE&DEV_2F04'), 'nvidia')
        self.assertEqual(components.vendor_of('AMD Radeon RX 7800 XT'), 'amd')
        self.assertEqual(components.vendor_of('Intel(R) Arc(TM) A770 Graphics'), 'intel')
        self.assertEqual(components.vendor_of('Microsoft Basic Display Adapter'), 'other')
        self.assertTrue(components.is_integrated('AMD Radeon(TM) Graphics', 'amd'))
        self.assertTrue(components.is_integrated('AMD Radeon(TM) 780M', 'amd'))
        self.assertFalse(components.is_integrated('AMD Radeon RX 7800 XT', 'amd'))
        self.assertTrue(components.is_integrated('Intel(R) UHD Graphics 770', 'intel'))
        self.assertTrue(components.is_integrated('Intel(R) Arc(TM) Graphics', 'intel'))
        self.assertFalse(components.is_integrated('Intel(R) Arc(TM) B580 Graphics', 'intel'))

    def test_combine_and_pick(self):
        cim = components.parse_cim(json.dumps([
            {'Name': 'AMD Radeon(TM) Graphics', 'PNPDeviceID': r'PCI\VEN_1002&DEV_164E', 'AdapterRAM': 2147483648},
            {'Name': 'NVIDIA GeForce RTX 5070', 'PNPDeviceID': r'PCI\VEN_10DE&DEV_2F04', 'AdapterRAM': 4293918720}]))
        registry = [('NVIDIA GeForce RTX 5070', r'pci\ven_10de&dev_2f04', 12 * GIB),
                    ('NVIDIA GeForce GTX 970', r'pci\ven_10de&dev_13c2', 4 * GIB)]   # a card that was taken out
        gpus = components.combine(cim, registry, {})
        self.assertEqual([g.name for g in gpus], ['AMD Radeon(TM) Graphics', 'NVIDIA GeForce RTX 5070'])
        best = components.pick(gpus)
        self.assertEqual((best.name, best.vram, best.vram_gb, best.rtx), ('NVIDIA GeForce RTX 5070', 12 * GIB, 12, 50))
        # WMI's 32-bit memory value alone says nothing for a card of 4 GiB or more; nvidia-smi does
        gpus = components.combine(cim, [], components.parse_nvidia_smi('NVIDIA GeForce RTX 5070, 12227\n'))
        self.assertEqual(components.pick(gpus).vram, 12227 << 20)
        self.assertEqual(components.combine(cim, [], {})[1].vram, 0)
        # one object, not a list, for a machine with one adapter; no WMI at all: the registry's rows
        one = components.parse_cim(json.dumps({'Name': 'Intel(R) UHD Graphics 770', 'PNPDeviceID': r'PCI\VEN_8086', 'AdapterRAM': 1 << 30}))
        self.assertEqual(components.pick(components.combine(one, [], {})).vendor, 'intel')
        self.assertEqual(components.pick(components.combine([], registry, {})).name, 'NVIDIA GeForce RTX 5070')
        self.assertIsNone(components.pick([]))
        self.assertEqual(components.parse_cim('garbage'), [])

    def gpu(self, name, gb):
        vendor = components.vendor_of(name)
        return components.Gpu(name, vendor, int(gb * GIB), components.is_integrated(name, vendor))

    def test_groups(self):
        self.assertEqual(components.groups_for(self.gpu('NVIDIA GeForce RTX 5070', 12)), ['fsr', 'dlss', 'dlssg'])
        self.assertEqual(components.groups_for(self.gpu('NVIDIA GeForce RTX 3060', 12)), ['fsr', 'dlss'])
        self.assertEqual(components.groups_for(self.gpu('NVIDIA GeForce GTX 1660', 6)), ['fsr'])
        self.assertEqual(components.groups_for(self.gpu('AMD Radeon RX 7800 XT', 16)), ['fsr'])
        self.assertEqual(components.groups_for(None), ['fsr'])
        self.assertEqual([f.name for f in components.wanted(self.gpu('AMD Radeon RX 6600', 8))],
                         ['amd_fidelityfx_loader_vk.dll', 'amd_fidelityfx_upscaler_vk.dll',
                          'amd_fidelityfx_framegeneration_vk.dll'])
        self.assertEqual(len(components.wanted(self.gpu('NVIDIA GeForce RTX 4090', 24))), len(components.FILES))

    def test_presets(self):
        def p(name, gb, have=components.GROUPS):
            r = components.preset(self.gpu(name, gb) if name else None, have)
            return r['id'], r['tex'], r['settings']

        # every card starts in base mode; an RTX card's menu row would switch on DLSS (taa = dlss)
        self.assertEqual(p('NVIDIA GeForce RTX 5070', 11.94), ('rtx_base', 'auto', {
            'dlaa': '0', 'taa': 'dlss', 'framegen': 'off', 'tex_quality': 'auto'}))
        self.assertEqual(p('NVIDIA GeForce RTX 4090', 24)[0], 'rtx_base')
        self.assertEqual(p('NVIDIA GeForce RTX 4060', 8)[0], 'rtx_base')
        self.assertEqual(p('NVIDIA GeForce RTX 3080', 10)[0], 'rtx_base')
        self.assertEqual(p('NVIDIA GeForce RTX 2060', 6)[:2], ('rtx_base', 'half'))
        self.assertEqual(p('NVIDIA GeForce RTX 3050 Laptop GPU', 4), ('rtx_base', 'quarter', {
            'dlaa': '1', 'taa': 'fsr1', 'framegen': 'off', 'dlss': 'quality', 'tex_quality': 'quarter'}))
        self.assertEqual(p('AMD Radeon RX 7800 XT', 16), ('base', 'auto', {
            'dlaa': '0', 'taa': 'fsr3', 'framegen': 'off', 'tex_quality': 'auto'}))
        self.assertEqual(p('Intel(R) Arc(TM) A770 Graphics', 16)[0], 'base')
        self.assertEqual(p('NVIDIA GeForce GTX 1660 Ti', 6)[:2], ('base', 'half'))
        self.assertEqual(p('AMD Radeon RX 570', 4), ('base', 'quarter', {
            'dlaa': '1', 'taa': 'fsr1', 'framegen': 'off', 'dlss': 'quality', 'tex_quality': 'quarter'}))
        self.assertEqual(p('AMD Radeon RX 6600', 7.98)[1], 'auto')                  # an "8 GB" card
        # a processor's own graphics: the scene at half of the game's picture, scaled by FSR 1
        self.assertEqual(p('AMD Radeon(TM) Graphics', 4), ('handheld', 'quarter', {
            'dlaa': '1', 'taa': 'fsr1', 'dlss': 'performance', 'framegen': 'off', 'tex_quality': 'quarter'}))
        self.assertEqual(p('AMD Custom GPU 0405', 1)[0], 'handheld')                # Steam Deck
        self.assertEqual(p('AMD Radeon 780M Graphics', 6)[:2], ('handheld', 'half'))
        self.assertEqual(p('Intel(R) Arc(TM) Graphics', 8)[0], 'handheld')
        self.assertEqual(p(None, 0), ('base', 'auto', {'dlaa': '0', 'taa': 'fsr3', 'framegen': 'off', 'tex_quality': 'auto'}))
        self.assertEqual(p('NVIDIA GeForce RTX 5070', 0)[0], 'rtx_base')            # memory unknown
        # a preset never switches on a library that is not in the folder
        self.assertEqual(p('NVIDIA GeForce RTX 5070', 12, {'fsr', 'dlss'})[0], 'rtx_base')
        self.assertEqual(p('NVIDIA GeForce RTX 5070', 12, {'fsr'})[0], 'base')
        self.assertEqual(p('NVIDIA GeForce RTX 5070', 12, set())[0], 'base')

    def test_preset_keys_are_runtime_keys(self):
        """Every key and value a preset writes is one runtime/cfg.c's table lists (skipped without the sources)."""
        cfg = net.ROOT / 'runtime' / 'cfg.c'
        if not cfg.is_file():
            self.skipTest('no runtime/cfg.c')
        import re
        text = cfg.read_text(encoding='utf-8', errors='replace')
        table = dict(re.findall(r'\{"([A-Z0-9_]+)",\s*"[^"]*",\s*"([^"]*)"', text))
        for name, gb in (('NVIDIA GeForce RTX 5070', 12), ('NVIDIA GeForce RTX 3050', 4), ('AMD Radeon RX 570', 4), ('', 0)):
            for key, value in components.preset(self.gpu(name, gb) if name else None)['settings'].items():
                self.assertIn(key.upper(), table, key)
                self.assertIn(value, table[key.upper()].split('|'), f'{key} = {value}')

    def test_pins(self):
        names = [f.name for f in components.FILES]
        self.assertEqual(len(names), len(set(names)))
        for f in components.FILES:
            self.assertRegex(f.sha256, r'^[0-9a-f]{64}$')
            self.assertIn(f.source, components.SOURCES)
            self.assertRegex(f.url, r'^https://(raw\.githubusercontent\.com|github\.com)/(NVIDIA|NVIDIA-RTX|GPUOpen-LibrariesAndSDKs)/')
        self.assertNotEqual(components.pins_digest(), components.pins_digest(components.FILES[:-1]))


class ComponentsInstallTest(Sandbox):
    ranges = True

    def table(self):
        """Stand-ins for the pinned files: one served as it is, two inside an archive."""
        raw, a, b = b'fsr library' * 1000, b'streamline one' * 500, b'streamline two' * 700
        data = io.BytesIO()
        with zipfile.ZipFile(data, 'w', zipfile.ZIP_DEFLATED) as z:
            z.writestr('readme.txt', 'x' * 5000)
            z.writestr('bin/x64/sl.one.dll', a)
            z.writestr('bin/x64/sl.two.dll', b)
        archive = data.getvalue()
        self.server.routes['/raw/fsr.dll'] = (200, {}, raw)
        self.server.routes['/sdk.zip'] = (200, {}, archive)
        sha = lambda d: hashlib.sha256(d).hexdigest()   # noqa: E731
        member = lambda name, d: components.File(name, 'fsr', 'streamline', self.server.url + '/sdk.zip', len(d), sha(d),  # noqa: E731
                                                 f'bin/x64/{name}', len(archive), sha(archive))
        return (components.File('fsr.dll', 'fsr', 'fsr', self.server.url + '/raw/fsr.dll', len(raw), sha(raw)),
                member('sl.one.dll', a), member('sl.two.dll', b)), (raw, a, b)

    def test_install_with_ranges(self):
        files, (raw, a, b) = self.table()
        dest, seen, lines = self.dir / 'game', [], []
        result = components.install(dest, None, self.dir / 't', progress=lambda g, d, t: seen.append((d, t)),
                                    log=lines.append, files=files)
        self.assertEqual((result['installed'], result['failed']), (['fsr.dll', 'sl.one.dll', 'sl.two.dll'], {}))
        self.assertEqual(((dest / 'fsr.dll').read_bytes(), (dest / 'sl.one.dll').read_bytes(), (dest / 'sl.two.dll').read_bytes()),
                         (raw, a, b))
        self.assertEqual(seen[-1], (len(raw) + len(a) + len(b),) * 2)
        self.assertTrue(any('MIT License' in line for line in lines))              # the log names each license
        self.assertTrue(all(agent == net.user_agent() for agent in self.server.agents))
        self.assertFalse(list(dest.glob('*.part')))
        # a second run downloads nothing; a file the player replaced is kept
        hits = len(self.server.hits)
        (dest / 'sl.one.dll').write_bytes(b'the player put a newer one here')
        again = components.install(dest, None, self.dir / 't', files=files)
        self.assertEqual((again['installed'], again['present'], again['kept']), ([], ['fsr.dll', 'sl.two.dll'], ['sl.one.dll']))
        self.assertEqual(len(self.server.hits), hits)
        self.assertEqual(components.missing(dest, None, files), [])

    def test_wrong_file_is_not_installed(self):
        files, _ = self.table()
        self.server.routes['/raw/fsr.dll'] = (200, {}, b'another build' * 846 + b'x' * 2)   # the pinned size, other bytes
        self.assertEqual(len(b'another build' * 846 + b'x' * 2), files[0].size)
        net.pause = lambda s: None
        try:
            result = components.install(self.dir / 'game', None, self.dir / 't', files=files[:1])
        finally:
            net.pause = time.sleep
        self.assertEqual(result['installed'], [])
        self.assertIn('SHA-256', result['failed']['fsr.dll'])
        self.assertFalse((self.dir / 'game' / 'fsr.dll').exists())

    def test_offline_is_a_result_not_an_error(self):
        files, _ = self.table()
        self.server.close()
        self.server = Server()
        dead = tuple(components.File(f.name, f.group, f.source, f.url.replace(f.url.split('/')[2], '127.0.0.1:9'), f.size,
                                     f.sha256, f.member, f.archive_size, f.archive_sha256) for f in files)
        net.pause = lambda s: None
        try:
            result = components.install(self.dir / 'game', None, self.dir / 't', files=dead)
        finally:
            net.pause = time.sleep
        self.assertEqual(sorted(result['failed']), ['fsr.dll', 'sl.one.dll', 'sl.two.dll'])
        self.assertEqual(components.missing(self.dir / 'game', None, dead), ['fsr.dll', 'sl.one.dll', 'sl.two.dll'])
        self.assertEqual(components.usable(self.dir / 'game', dead), set())


class ComponentsNoRangeTest(Sandbox):
    """A server that answers a range request with the whole file: the archive is downloaded once instead."""

    def test_fallback_to_whole_archive(self):
        helper = ComponentsInstallTest.table
        files, (raw, a, b) = helper(self)
        dest = self.dir / 'game'
        result = components.install(dest, None, self.dir / 't', files=files)
        self.assertEqual((sorted(result['installed']), result['failed']), (['fsr.dll', 'sl.one.dll', 'sl.two.dll'], {}))
        self.assertEqual(((dest / 'sl.one.dll').read_bytes(), (dest / 'sl.two.dll').read_bytes()), (a, b))
        self.assertFalse(list((self.dir / 't').glob('*.zip')))                     # the archive does not stay


class RemoteZipTest(Sandbox):
    ranges = True

    def test_members(self):
        data = io.BytesIO()
        with zipfile.ZipFile(data, 'w') as z:
            z.writestr('stored.bin', b'abc' * 100, zipfile.ZIP_STORED)
            z.writestr('dir/packed.bin', b'xyz' * 10000, zipfile.ZIP_DEFLATED)
        archive = data.getvalue()
        self.server.routes['/a.zip'] = (200, {}, archive)
        remote = net.RemoteZip(self.server.url + '/a.zip', len(archive))
        self.assertEqual(remote.read('dir/packed.bin'), b'xyz' * 10000)
        self.assertEqual(remote.read('stored.bin'), b'abc' * 100)
        self.assertEqual(len(self.server.hits), 3)             # the directory once, then one request per member
        with self.assertRaises(net.DownloadError):
            remote.read('absent.bin')
        self.assertEqual(struct.unpack('<I', archive[:4])[0], 0x04034b50)


class EtaTest(Sandbox):
    def test_defaults_scale_with_cores(self):
        self.assertEqual(eta.estimate('oot_compile', 'first', 16, {}), (720.0, 'default'))
        self.assertEqual(eta.estimate('oot_compile', 'first', 8, {}), (1440.0, 'default'))    # half the cores, twice the time
        self.assertEqual(eta.estimate('oot_compile', 'first', 32, {}), (360.0, 'default'))
        self.assertEqual(eta.estimate('oot_compile', 'first', 2, {})[0], 720.0 * 4)           # the factor stops at 4
        self.assertEqual(eta.estimate('oot_compile', 'first', 128, {})[0], 360.0)             # and at a half
        self.assertEqual(eta.estimate('oot_prepare', 'first', 8, {})[0], 3600.0)
        self.assertEqual(eta.estimate('oot_prepare', 'rerun', 16, {})[0], 60.0)
        self.assertEqual(eta.estimate('oot_extract', 'first', 4, {})[0], 20.0)                # not a processor's step
        self.assertEqual(eta.estimate('oot_components', 'first', 4, {})[0], 60.0)
        self.assertEqual(eta.estimate('something_new', 'first', 16, {}), (30.0, 'default'))

    def test_measured_overrides(self):
        timings = {'oot_compile': {'first': 501.5}}
        self.assertEqual(eta.estimate('oot_compile', 'first', 4, timings), (501.5, 'measured'))   # this machine's, unscaled
        self.assertEqual(eta.estimate('oot_compile', 'rerun', 16, timings), (240.0, 'default'))   # the other case is not known
        event = eta.estimates({'oot_compile': 'first', 'oot_check': 'rerun'}, 16, timings)
        self.assertEqual(event['steps']['oot_compile'], {'case': 'first', 'seconds': 501.5, 'source': 'measured'})
        self.assertEqual(event['steps']['oot_check'], {'case': 'rerun', 'seconds': 10.0, 'source': 'default'})

    def test_record_and_load(self):
        self.assertEqual(eta.load_timings(), {})
        eta.record('oot_compile', 'first', 702.44)
        eta.record('oot_compile', 'rerun', 163)
        eta.record('oot_prepare', 'first', 1500.0)
        eta.record('oot_prepare', 'first', 0)            # a step that reports no time is not kept
        eta.record('oot_prepare', 'other', 5)
        self.assertEqual(eta.load_timings(), {'oot_compile': {'first': 702.4, 'rerun': 163.0}, 'oot_prepare': {'first': 1500.0}})
        self.assertEqual(eta.timings_path().parent, update.state_path().parent)   # beside update.json
        self.assertNotIn(b'\r', eta.timings_path().read_bytes())
        self.assertEqual(eta.estimates({'oot_prepare': 'first'}, 8)['steps']['oot_prepare']['source'], 'measured')
        eta.timings_path().write_bytes(b'{broken')
        self.assertEqual(eta.load_timings(), {})

    def run_to_compile(self, estimate=720.0):
        t = eta.Tracker(clock=lambda: 0.0)
        steps = ['oot_check', 'oot_extract', 'oot_compile', 'oot_prepare', 'oot_package']
        t.feed({'type': 'start', 'steps': steps}, now=0)
        t.feed({'type': 'eta', 'cores': 16, 'steps': {'oot_check': {'seconds': 10}, 'oot_extract': {'seconds': 20},
                                                      'oot_compile': {'seconds': estimate}, 'oot_prepare': {'seconds': 1800},
                                                      'oot_package': {'seconds': 360}}}, now=0)
        t.feed({'type': 'plan', 'run': steps[:3] + steps[4:], 'skip': {'oot_prepare': 'current'}}, now=0)
        t.feed({'type': 'step', 'step': 'oot_check', 'state': 'started'}, now=0)
        t.feed({'type': 'step', 'step': 'oot_check', 'state': 'done', 'seconds': 9.0}, now=9)
        t.feed({'type': 'step', 'step': 'oot_extract', 'state': 'started'}, now=9)
        t.feed({'type': 'step', 'step': 'oot_extract', 'state': 'done', 'seconds': 21.0}, now=30)
        t.feed({'type': 'step', 'step': 'oot_compile', 'state': 'started'}, now=30)
        return t

    def test_view(self):
        t = self.run_to_compile()
        v = t.view(now=30)
        rows = {r['id']: r for r in v['steps']}
        self.assertEqual([r['state'] for r in v['steps']], ['done', 'done', 'running', 'skipped', 'pending'])
        self.assertEqual((rows['oot_check']['seconds_taken'], rows['oot_extract']['seconds_taken']), (9.0, 21.0))
        self.assertEqual((rows['oot_compile']['remaining'], rows['oot_compile']['fraction'], rows['oot_compile']['counter']),
                         (720.0, None, None))
        self.assertEqual(rows['oot_package']['remaining'], 360.0)
        self.assertIsNone(rows['oot_prepare']['remaining'])                        # a skipped step costs nothing
        self.assertEqual((v['elapsed'], v['remaining'], v['index'], v['count'], v['running']), (30, 1080.0, 3, 5, 'oot_compile'))

    def test_clock_only_counts_down_and_never_reaches_zero(self):
        t = self.run_to_compile()
        t.view(now=30)
        self.assertAlmostEqual(t.view(now=130)['steps'][2]['remaining'], 620.0)
        late = t.view(now=30 + 3000)['steps'][2]['remaining']                      # far over the estimate, still running
        self.assertEqual(late, 36.0)                                               # 5 % of the estimate, not 0 or less

    def test_progress_refines(self):
        t = self.run_to_compile(estimate=720.0)
        t.view(now=30)
        self.assertAlmostEqual(t.view(now=329.9)['steps'][2]['remaining'], 420.1)   # the window asks ten times a second
        # 300 s in, 412 of 681 objects: the counter says about 222 s for the rest, the estimate said 420
        t.feed({'type': 'progress', 'step': 'oot_compile', 'phase': 'compile', 'done': 412, 'total': 681, 'unit': 'objects'}, now=330)
        row = t.view(now=330)['steps'][2]
        self.assertAlmostEqual(row['fraction'], 0.95 * 412 / 681, places=6)
        self.assertEqual(row['counter'], (412, 681, 'objects'))
        target = 300 * (1 - row['fraction']) / row['fraction']
        self.assertAlmostEqual(target, 222.0, delta=1.0)
        # ... but the shown figure does not jump there from 420: it moves at a bounded speed and arrives
        self.assertGreater(row['remaining'], 400)
        shown, now = row['remaining'], 330
        while now < 330 + 120:
            now += 0.5
            new = t.view(now=now)['steps'][2]['remaining']
            self.assertLessEqual(shown - new, 0.5 * max(4.0, 0.02 * shown) + 1e-6)   # at most 4 s (or 2 %) per second down
            self.assertLessEqual(new - shown, 0.5 * max(2.0, 0.01 * shown) + 1e-6)
            shown = new
        self.assertLess(abs(shown - (now - 30) * (1 - row['fraction']) / row['fraction']), 5)

    def test_early_counter_is_mixed_with_the_estimate(self):
        t = self.run_to_compile(estimate=720.0)
        # 10 s in, 2 of 681: by the counter alone the rest would take 56 minutes
        t.feed({'type': 'progress', 'step': 'oot_compile', 'phase': 'compile', 'done': 2, 'total': 681, 'unit': 'objects'}, now=40)
        row = t.view(now=40)['steps'][2]
        alone = 10 * (1 - row['fraction']) / row['fraction']
        self.assertGreater(alone, 3000)
        self.assertLess(row['remaining'], 760)                                     # close to the estimate's 718
        self.assertGreater(row['remaining'], 700)

    def test_slow_step_raises_slowly(self):
        t = self.run_to_compile(estimate=100.0)
        t.view(now=30)
        self.assertEqual(t.view(now=229.9)['steps'][2]['remaining'], 5.0)          # over its estimate: the floor
        t.feed({'type': 'progress', 'step': 'oot_compile', 'phase': 'compile', 'done': 100, 'total': 400, 'unit': 'objects'}, now=230)
        first = t.view(now=230)['steps'][2]['remaining']                           # the counter says 640 s; shown: the floor
        self.assertLess(first, 20)
        second = t.view(now=231)['steps'][2]['remaining']
        self.assertLessEqual(second - first, 2.0 + 1e-6)                           # up by at most 2 s per second
        self.assertGreater(second, first)

    def test_phases(self):
        f = eta.step_fraction
        self.assertIsNone(f('oot_compile', 'compile', None, None, ['compile']))    # no counter yet, nothing behind it
        self.assertAlmostEqual(f('oot_compile', 'link', None, None, ['compile', 'link']), 0.95)
        self.assertAlmostEqual(f('oot_prepare', 'libraries', 500, 1000, []), 0.03 + 0.02 + 0.15 + 0.40)
        self.assertAlmostEqual(f('oot_prepare', 'translate', 50, 100, []), 0.05 + 0.075)
        self.assertAlmostEqual(f('oot_components', 'dlssg', 60, 100, []), 0.05 + 0.95 * 0.6)   # one counter over the groups
        self.assertAlmostEqual(f('oot_package', 'warm', None, None, []), 0.15)
        self.assertAlmostEqual(f('some_step', 'x', 1, 4, []), 0.25)                 # no table: the counter is the step
        self.assertIsNone(f('some_step', 'x', None, None, []))
        self.assertIsNone(f('oot_compile', 'unknown phase', 1, 2, []))
        self.assertEqual(f('oot_compile', 'compile', 900, 681, []), 0.95)           # a counter that overshoots

    def test_done_and_end(self):
        t = self.run_to_compile()
        t.feed({'type': 'step', 'step': 'oot_compile', 'state': 'done', 'seconds': 640.0}, now=670)
        t.feed({'type': 'step', 'step': 'oot_prepare', 'state': 'skipped', 'reason': 'current'}, now=670)
        t.feed({'type': 'step', 'step': 'oot_package', 'state': 'started'}, now=670)
        t.feed({'type': 'step', 'step': 'oot_package', 'state': 'done'}, now=1000)   # no seconds: by the clock
        t.feed({'type': 'end', 'result': 'ok'}, now=1000)
        v = t.view(now=5000)
        self.assertEqual((v['elapsed'], v['remaining'], v['running']), (1000, 0.0, None))
        self.assertEqual([r['seconds_taken'] for r in v['steps']], [9.0, 21.0, 640.0, None, 330])

    def test_words(self):
        words = {'eta.about_min': 'about {n} min', 'eta.about_under': 'under a minute', 'eta.about_hours': 'about {n} h',
                 'eta.left_min': 'about {n} minutes left', 'eta.left_one': 'about a minute left',
                 'eta.left_under': 'less than a minute left', 'eta.left_hours': 'about {n} hours left',
                 'eta.counter': '{done} of {total}', 'eta.counter_mb': '{done} of {total} MB',
                 'eta.step_of': 'Step {index} of {count}', 'eta.elapsed': '{time} elapsed'}
        cat = lambda id, **p: words[id].format(**p)   # noqa: E731
        self.assertEqual((eta.clock_text(21), eta.clock_text(432), eta.clock_text(3723), eta.clock_text(0)),
                         ('0:21', '7:12', '1:02:03', '0:00'))
        row = {'state': 'done', 'seconds_taken': 21.2, 'remaining': None, 'counter': None}
        self.assertEqual(eta.step_text(cat, row), '0:21')
        row = {'state': 'running', 'seconds_taken': 300, 'remaining': 290, 'counter': (412, 681, 'objects')}
        self.assertEqual(eta.step_text(cat, row), '412 of 681 · about 5 min')
        row = {'state': 'running', 'seconds_taken': 30, 'remaining': 20, 'counter': (31_000_000, 79_000_000, 'bytes')}
        self.assertEqual(eta.step_text(cat, row), '31 of 79 MB · under a minute')
        self.assertEqual(eta.step_text(cat, {'state': 'pending', 'remaining': 60, 'counter': None}), 'about 1 min')
        self.assertEqual(eta.step_text(cat, {'state': 'skipped', 'remaining': None, 'counter': None}), '')
        view = {'index': 4, 'count': 7, 'running': 'oot_compile', 'remaining': 37 * 60, 'elapsed': 432}
        self.assertEqual(eta.total_text(cat, view), 'Step 4 of 7 · about 37 minutes left · 7:12 elapsed')
        self.assertEqual(eta.total_text(cat, {**view, 'remaining': 70}), 'Step 4 of 7 · about a minute left · 7:12 elapsed')
        self.assertEqual(eta.total_text(cat, {**view, 'remaining': 7200}), 'Step 4 of 7 · about 2.0 hours left · 7:12 elapsed')
        self.assertEqual(eta.total_text(cat, {**view, 'running': None, 'remaining': 0}), '7:12 elapsed')

    def test_catalogs_have_the_words(self):
        from .guilogic import Catalog
        for lang in ('en', 'ru'):
            cat = Catalog(lang)
            for step in oot.all_steps():
                self.assertTrue(cat.has('step.' + step.id), step.id)
            for id in ('eta.about_min', 'eta.left_min', 'eta.left_one', 'eta.counter', 'eta.step_of', 'eta.elapsed',
                       'ui.oot_update_available', 'ui.oot_hw_line', 'ui.oot_components_missing', 'update.unverified',
                       'oot.components_failed', 'preset.rtx_base', 'preset.rtx_framegen', 'preset.rtx_dlss', 'preset.base', 'comp.dlssg'):
                self.assertIn(id, cat.texts, f'{lang}: {id}')
            for group in components.GROUPS:
                self.assertIn(f'phase.oot_components.{group}', cat.texts)
            for name in ('ui.oot_home_next', 'ui.oot_home_prev'):
                self.assertNotIn(name, cat.texts)

    def test_every_language_has_the_launcher_words(self):
        """The window never falls back to English in the middle of a screen: each of the thirteen catalogs has every
        id the English one has for the launcher, with the same parameters."""
        import re
        from .guilogic import UI_LANGUAGES, load_texts
        english = load_texts('en')
        ids = [i for i in english if i.startswith(('ui.oot_', 'eta.', 'step.oot_', 'step.update_', 'phase.oot_',
                                                    'phase.update_', 'preset.', 'comp.', 'update.', 'oot.'))]
        self.assertIn('ui.oot_fact_gpu', ids)
        for lang in UI_LANGUAGES:
            texts = load_texts(lang)
            for i in ids:
                self.assertIn(i, texts, f'{lang}: {i}')
                self.assertEqual(set(re.findall(r'\{(\w+)\}', texts[i])), set(re.findall(r'\{(\w+)\}', english[i])),
                                 f'{lang}: {i}')


class WindowPartsTest(unittest.TestCase):
    """The parts of the window (gui_oot.py) that need no window."""

    def setUp(self):
        try:
            from . import gui_oot
        except ImportError as e:   # a Python without Tk
            self.skipTest(str(e))
        self.gui = gui_oot

    def test_notes_text(self):
        notes = self.gui.notes_text('## Rekarina 1.0.1\r\n\r\n\r\n- **Shorter** loading\n* the `ripple` patch\n'
                                    '+ see [the page](https://example.org/x)\n\n\nPlain __words__ stay. 2 * 3 - 1\n')
        self.assertEqual(notes, 'Rekarina 1.0.1\n\n• Shorter loading\n• the ripple patch\n• see the page\n\n'
                                'Plain words stay. 2 * 3 - 1')
        self.assertEqual(self.gui.notes_text(''), '')
        self.assertEqual(self.gui.notes_text('\n\n  \n'), '')

    def test_step_share(self):
        share = self.gui.step_share
        self.assertEqual(share(None), 0.0)
        self.assertEqual(share({'fraction': 0.4, 'seconds_taken': 900, 'seconds_estimated': 10}), 0.4)   # the counter decides
        self.assertEqual(share({'fraction': 1.7}), 1.0)
        self.assertEqual(share({'fraction': None, 'seconds_taken': 30, 'seconds_estimated': 120}), 0.25)   # by the clock
        self.assertEqual(share({'fraction': None, 'seconds_taken': 500, 'seconds_estimated': 120}), 0.95)  # never full
        self.assertEqual(share({'fraction': None, 'seconds_taken': 5, 'seconds_estimated': 0}), 0.0)

    def test_colours_are_the_menus(self):
        """The window's colours are the in-game menu's (skipped without the runtime's sources)."""
        import re
        sources = [net.ROOT / 'runtime' / 'ui' / name for name in ('menu.cpp', 'option_rows.cpp')]
        if not all(p.is_file() for p in sources):
            self.skipTest('no runtime/ui sources')
        text = '\n'.join(p.read_text(encoding='utf-8', errors='replace') for p in sources)
        menu = {name: '#%02x%02x%02x' % tuple(int(v) for v in rgb.split(',')[:3])
                for name, rgb in re.findall(r'\b(k[A-Z]\w+) = IM_COL32\(([\d, ]+)\)', text)}
        g = self.gui
        for ours, theirs in ((g.CREAM, 'kMenuCream'), (g.SOFT, 'kMenuDim'), (g.INSET, 'kMenuInset'), (g.ROW_EDGE, 'kMenuEdge'),
                             (g.NOTE, 'kMenuNote'), (g.ROW, 'kRow'), (g.ROW_HOT, 'kRowHot'), (g.ROW_EDGE, 'kRowEdge'),
                             (g.INSET, 'kInset'), (g.CREAM, 'kCream')):
            self.assertEqual(ours, menu[theirs], theirs)

    def test_amount_of_a_download(self):
        from .guilogic import Catalog, amount
        cat = Catalog('en')
        self.assertEqual(amount(cat, 31_000_000, 79_000_000, 'bytes'), '31 of 79 MB')
        self.assertEqual(amount(cat, None, 561_000_000, 'bytes'), '0 of 561 MB')
        self.assertEqual(amount(cat, 6e9, 16e9, 'bytes'), '6.0 / 16.0 GB')
        self.assertEqual(amount(cat, 412, 681, 'objects'), '412 / 681 files')


class SetupTest(Sandbox):
    def test_steps_and_estimates(self):
        ids = [s.id for s in oot.all_steps()]
        self.assertEqual(ids, ['oot_check', 'oot_extract', 'oot_lift', 'oot_compile', 'oot_components', 'oot_prepare',
                               'oot_package'])
        for i in ids:
            self.assertIn(i, eta.DEFAULTS)

    def test_tree_stamp_is_by_content(self):
        tree = self.dir / 'tree'
        (tree / 'sub').mkdir(parents=True)
        (tree / 'a.c').write_bytes(b'int a;\n')
        (tree / 'sub' / 'b.h').write_bytes(b'int b;\n')
        before = oot.tree_stamp(tree)
        os.utime(tree / 'a.c', (time.time() + 5000, time.time() + 5000))           # unpacked on another day
        self.assertEqual(oot.tree_stamp(tree), before)
        (tree / 'a.c').write_bytes(b'int A;\n')                                    # the same size, other bytes
        self.assertNotEqual(oot.tree_stamp(tree), before)
        (tree / 'a.c').write_bytes(b'int a;\n')
        (tree / 'sub' / 'b.h').rename(tree / 'b.h')                                # the same file in another place
        self.assertNotEqual(oot.tree_stamp(tree), before)

    def test_timed_reporter_records(self):
        rep = oot.TimedReporter(out=io.StringIO())
        rep.cases = {'oot_lift': 'first'}
        rep.step('oot_lift', 'started')
        rep.step('oot_lift', 'done', seconds=98.3)
        rep.step('oot_other', 'done', seconds=5.0)
        self.assertEqual(eta.load_timings(), {'oot_lift': {'first': 98.3}})


if __name__ == '__main__':
    unittest.main()
