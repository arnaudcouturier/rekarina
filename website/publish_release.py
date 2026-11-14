"""Publish a Rekarina release to the site: the files to the download host, then the record and the page.

    python publish_release.py <dist folder> [--dry-run] [--force] [--deploy]

<dist folder> is what tools/make_rekarina_release.py wrote with
    --dist <dist> --single <dist>/Rekarina.exe --site-url https://dl.rekarina.dev/releases --notes <file> --sign <key>
that is Rekarina.exe, Rekarina-Setup-<version>-windows.zip, SHA256SUMS, SHA256SUMS.sig and latest.json.

Where things go (docs/distribution.md):
    https://dl.rekarina.dev/releases/<version>/<file>   the four files: an R2 bucket (BUCKET) behind that domain. The
                                                        site itself cannot hold them: its files stop at 25 MiB.
    https://rekarina.dev/releases/latest.json           the record the launcher reads (installer/update.py FEED)
    https://rekarina.dev/download                       a redirect to the newest Rekarina.exe (public/_redirects)
    the front page                                      the download button, the version and the file's SHA-256

The order is the one a launcher in the field needs: nothing is uploaded before the folder is proved to be a release
the launchers will take (every file's SHA-256 is its line in SHA256SUMS, and SHA256SUMS.sig is one of the project
keys' signature over it), the files go up and are read back before the record names them, and the page is deployed
last. A version that is already on the download host is refused (--force replaces it): its files are cached for good.

--dry-run checks the folder and says what would happen. Without --deploy the site's files are changed and the
deploy command is printed; with it the site is deployed at the end.
"""
import argparse
import hashlib
import html
import json
import os
import re
import shutil
import subprocess
import sys
import urllib.error
import urllib.request
from pathlib import Path

SITE = Path(__file__).resolve().parent
PUBLIC = SITE / 'public'
SRC = SITE.parent
BUCKET = 'rekarina-releases'
DOWNLOADS = 'https://dl.rekarina.dev/releases'
HOME = 'https://rekarina.dev/'
WRANGLER = ['npx', '--yes', 'wrangler@4']
TYPES = {'.exe': 'application/vnd.microsoft.portable-executable', '.zip': 'application/zip', '.sig': 'text/plain',
         '': 'text/plain'}


def fail(text):
    sys.exit(f'publish_release: {text}')


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def check(dist):
    """The release in the folder: its version, its files and their checksums; exits when it is not one the launchers
    would take."""
    record_path = dist / 'latest.json'
    if not record_path.is_file():
        fail(f'{record_path} is missing: make the release with --site-url {DOWNLOADS}')
    record = json.loads(record_path.read_text(encoding='utf-8'))
    version = str(record.get('tag_name', '')).lstrip('vV')
    if not re.fullmatch(r'\d+(\.\d+)*', version):
        fail(f'latest.json names no version (tag_name {record.get("tag_name")!r})')
    names = ['Rekarina.exe', f'Rekarina-Setup-{version}-windows.zip', 'SHA256SUMS', 'SHA256SUMS.sig']
    for name in names:
        if not (dist / name).is_file():
            fail(f'{dist / name} is missing' + (': sign the release (--sign)' if name.endswith('.sig') else ''))
    sums_bytes = (dist / 'SHA256SUMS').read_bytes()
    lines = dict(reversed(m.groups()) for m in re.finditer(r'^([0-9a-f]{64})\s+\*?(\S.*?)\s*$',
                                                           sums_bytes.decode('utf-8'), re.M))
    sums = {}
    for name in names[:2]:
        sums[name] = sha256(dist / name)
        if lines.get(name) != sums[name]:
            fail(f'{name} is not the file SHA256SUMS lists ({sums[name][:16]}... against {str(lines.get(name))[:16]}...)')
    sys.path.insert(0, str(SRC))
    from installer import update  # the keys every launcher carries
    if not update.signing_keys():
        fail('installer/update.py carries no signing key')
    if not update.signed_by_us(sums_bytes, (dist / 'SHA256SUMS.sig').read_text(encoding='ascii', errors='replace')):
        fail('SHA256SUMS.sig is not the signature of a project key over SHA256SUMS: no launcher would install this')
    for asset in record.get('assets') or []:
        want = f'{DOWNLOADS}/{version}/{asset.get("name")}'
        if asset.get('browser_download_url') != want:
            fail(f'latest.json points {asset.get("name")} at {asset.get("browser_download_url")}, not {want}: '
                 f'make the release with --site-url {DOWNLOADS}')
    listed = {a.get('name') for a in record.get('assets') or []}
    if not set(names) <= listed:
        fail(f'latest.json does not list {sorted(set(names) - listed)}')
    return version, names, sums, record


def head(url):
    """The size the address answers with, or None when it is not there."""
    try:
        with urllib.request.urlopen(urllib.request.Request(url, method='HEAD'), timeout=30) as r:
            return int(r.headers.get('Content-Length') or 0)
    except urllib.error.HTTPError as e:
        e.close()
        return None
    except OSError as e:
        fail(f'{url} cannot be asked ({e}): is the download domain attached to the bucket?')


def wrangler(*args):
    npx = shutil.which('npx')
    if not npx:
        fail('npx is not on PATH (Node.js)')
    env = dict(os.environ, WRANGLER_SEND_METRICS='false')
    r = subprocess.run([npx, *WRANGLER[1:], *args], cwd=SITE, env=env)
    if r.returncode:
        fail(f'wrangler {" ".join(args[:3])} ... failed ({r.returncode})')


def upload(dist, version, names):
    for name in names:
        suffix = Path(name).suffix if '.' in name else ''
        wrangler('r2', 'object', 'put', f'{BUCKET}/releases/{version}/{name}', '--file', str(dist / name), '--remote',
                 '--content-type', TYPES.get(suffix, 'application/octet-stream'),
                 '--cache-control', 'public, max-age=31536000, immutable')
    for name in names:
        size, there = (dist / name).stat().st_size, head(f'{DOWNLOADS}/{version}/{name}')
        if there != size:
            fail(f'{DOWNLOADS}/{version}/{name} answers {there} bytes, the file has {size}')
    with urllib.request.urlopen(f'{DOWNLOADS}/{version}/SHA256SUMS', timeout=30) as r:
        if r.read() != (dist / 'SHA256SUMS').read_bytes():
            fail('the SHA256SUMS on the download host is not the one in the folder')


def between(text, mark, new):
    """text with what stands between <!-- release:mark --> and <!-- /release:mark --> replaced."""
    pattern = re.compile(rf'(<!-- release:{mark} -->).*?(<!-- /release:{mark} -->)', re.S)
    if not pattern.search(text):
        fail(f'public/index.html has no release:{mark} block')
    return pattern.sub(lambda m: f'{m.group(1)}\n{new}\n        {m.group(2)}', text, count=1)


def write_site(dist, version, sums, record):
    exe = dist / 'Rekarina.exe'
    megabytes = round(exe.stat().st_size / 1e6)
    page = (PUBLIC / 'index.html').read_text(encoding='utf-8')
    v = html.escape(version)
    page = between(page, 'hero',
                   '        <a class="plate play cursor" href="/download">Download for Windows</a>\n'
                   f'        <span class="meta">Rekarina.exe &middot; version {v} &middot; {megabytes} MB &middot; '
                   'Windows 10 or 11, 64-bit</span>')
    page = between(page, 'sums',
                   '        <p>Rekarina.exe has no code-signing certificate, so Windows SmartScreen shows an '
                   '&ldquo;unknown publisher&rdquo; page until enough people have run the file. To be sure the file is '
                   'the one published here, compare its SHA-256 with the line below.</p>\n'
                   f'        <div class="sum"><b>SHA-256 of Rekarina.exe {v}</b><code>{sums["Rekarina.exe"]}</code></div>\n'
                   '        <p class="how">In a terminal: <code>certutil -hashfile Rekarina.exe SHA256</code></p>')
    with open(PUBLIC / 'index.html', 'w', encoding='utf-8', newline='\n') as f:
        f.write(page)
    redirects = (PUBLIC / '_redirects').read_text(encoding='utf-8').splitlines()
    redirects = [line for line in redirects if not line.startswith('/download ')]
    redirects.append(f'/download {DOWNLOADS}/{version}/Rekarina.exe 302')
    with open(PUBLIC / '_redirects', 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(redirects) + '\n')
    record = dict(record, html_url=HOME)   # the launcher's "release page" link: the site, not a folder of the bucket
    (PUBLIC / 'releases').mkdir(exist_ok=True)
    with open(PUBLIC / 'releases' / 'latest.json', 'w', encoding='utf-8', newline='\n') as f:
        json.dump(record, f, indent=1, ensure_ascii=False)
        f.write('\n')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('dist', type=Path)
    ap.add_argument('--dry-run', action='store_true', help='check the folder and say what would be done')
    ap.add_argument('--force', action='store_true', help='replace a version that is already on the download host')
    ap.add_argument('--deploy', action='store_true', help='deploy the site at the end (wrangler deploy)')
    a = ap.parse_args()
    version, names, sums, record = check(a.dist)
    print(f'publish_release: Rekarina {version}: the folder is a signed release '
          f'(Rekarina.exe {sums["Rekarina.exe"][:16]}...)')
    if a.dry_run:
        for name in names:
            print(f'  would upload {name} ({(a.dist / name).stat().st_size / 1e6:.1f} MB) to '
                  f'{DOWNLOADS}/{version}/{name}')
        print(f'  would write public/releases/latest.json, the /download redirect and the page for {version}')
        return
    if head(f'{DOWNLOADS}/{version}/SHA256SUMS') is not None and not a.force:
        fail(f'{version} is already on the download host; a published version is not replaced (--force does it)')
    upload(a.dist, version, names)
    write_site(a.dist, version, sums, record)
    print('publish_release: the files are up and the site folder names them')
    if a.deploy:
        wrangler('deploy')
        print(f'publish_release: {HOME} now offers {version}')
    else:
        print(f'publish_release: look at public/index.html, then deploy from {SITE}: npx wrangler deploy')


if __name__ == '__main__':
    main()
