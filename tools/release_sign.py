"""The signing key of the Rekarina releases, and the signature of a release's SHA256SUMS.

    python tools/release_sign.py keygen <key file>             a new private key; prints the public key
    python tools/release_sign.py public <key file>             prints the public key of a private key
    python tools/release_sign.py sign <key file> <SHA256SUMS>  writes <SHA256SUMS>.sig beside the file
    python tools/release_sign.py verify <SHA256SUMS> [<public key hex>...]
                                                               checks <SHA256SUMS>.sig; without a key on the command
                                                               line, against installer/update.py's SIGNING_KEYS

The launcher installs an update only when its archive's SHA-256 stands in a SHA256SUMS that one of the public keys in
installer/update.py (SIGNING_KEYS) signed. The private key is a file of 64 hex digits (an Ed25519 seed, RFC 8032,
installer/ed25519.py). It belongs on the maintainer's machine alone, outside every repository and backup that others
can read: whoever has it can publish updates that every installed launcher accepts. keygen refuses to write over an
existing file and inside this checkout. Losing the key is not fatal as long as a second public key was put into
SIGNING_KEYS beforehand (keep that one's private half offline): a release signed with it is accepted too, and can
bring a launcher with new keys.

The signature file is the 64 signature bytes as 128 hex digits and a line feed, over the exact bytes of SHA256SUMS.
"""
import argparse
import secrets
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
from installer import ed25519  # noqa: E402


def read_seed(path):
    try:
        seed = bytes.fromhex(Path(path).read_text(encoding='ascii').strip())
    except (OSError, ValueError) as e:
        sys.exit(f'release_sign: {path}: {e}')
    if len(seed) != 32:
        sys.exit(f'release_sign: {path} is not a private key (64 hex digits)')
    return seed


def sign_file(key_file, sums):
    """Write <sums>.sig with the key's signature over the file's bytes; returns the signature file's path."""
    sums = Path(sums)
    signature = ed25519.sign(read_seed(key_file), sums.read_bytes())
    out = sums.with_name(sums.name + '.sig')
    out.write_bytes(signature.hex().encode('ascii') + b'\n')
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest='what', required=True)
    for name in ('keygen', 'public'):
        sub.add_parser(name).add_argument('key', type=Path)
    s = sub.add_parser('sign')
    s.add_argument('key', type=Path)
    s.add_argument('sums', type=Path)
    v = sub.add_parser('verify')
    v.add_argument('sums', type=Path)
    v.add_argument('public', nargs='*')
    a = ap.parse_args()
    if a.what == 'keygen':
        key = a.key.resolve()
        if key.exists():
            sys.exit(f'release_sign: {key} exists: not replaced')
        if ROOT in key.parents or ROOT.parent in key.parents and (ROOT.parent / '.git').exists():
            sys.exit(f'release_sign: {key} is inside the checkout: keep the private key outside it')
        seed = secrets.token_bytes(32)
        key.parent.mkdir(parents=True, exist_ok=True)
        key.write_bytes(seed.hex().encode('ascii') + b'\n')
        print(f'release_sign: private key written to {key}: keep it to yourself')
        print(f'public key, for SIGNING_KEYS in installer/update.py: {ed25519.public_key(seed).hex()}')
    elif a.what == 'public':
        print(ed25519.public_key(read_seed(a.key)).hex())
    elif a.what == 'sign':
        print(f'release_sign: {sign_file(a.key, a.sums)}')
    else:
        keys = a.public
        if not keys:
            from installer import update
            keys = list(update.SIGNING_KEYS)
        if not keys:
            sys.exit('release_sign: no public key: none on the command line, none in installer/update.py')
        try:
            signature = bytes.fromhex(a.sums.with_name(a.sums.name + '.sig').read_text(encoding='ascii').strip())
        except (OSError, ValueError) as e:
            sys.exit(f'release_sign: the signature file: {e}')
        good = any(ed25519.verify(bytes.fromhex(k), a.sums.read_bytes(), signature) for k in keys)
        print('release_sign: the signature is ' + ('good' if good else 'NOT one of these keys\' over this file'))
        return 0 if good else 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
