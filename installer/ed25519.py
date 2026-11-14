"""Ed25519 signatures (RFC 8032) in plain Python: the launcher checks a release's SHA256SUMS with it before an update
is installed (update.py, SIGNING_KEYS), and tools/release_sign.py makes the key and the signature.

    public_key(seed)                     the 32-byte public key of a 32-byte private seed
    sign(seed, message)                  the 64-byte signature
    verify(public, message, signature)   True when the signature is this key's over this message

Integers and hashlib only, so the bundled Python needs no package for it. A verification takes a few milliseconds'
worth of big-number arithmetic a dozen times over (about 20 ms); the arithmetic is not constant-time, which matters
for signing on a shared machine and not for verifying public data. The curve is -x^2 + y^2 = 1 + d x^2 y^2 over the
field of 2^255 - 19; points are kept in extended coordinates (X, Y, Z, T) with x = X/Z, y = Y/Z, xy = T/Z.
"""
import hashlib

P = 2 ** 255 - 19
ORDER = 2 ** 252 + 27742317777372353535851937790883648493
D = -121665 * pow(121666, P - 2, P) % P
ROOT_M1 = pow(2, (P - 1) // 4, P)   # a square root of -1


def _x_of(y, odd):
    """The x of the curve point with this y whose lowest bit is `odd`, or None when there is none."""
    if y >= P:
        return None
    x2 = (y * y - 1) * pow(D * y * y + 1, P - 2, P) % P
    if x2 == 0:
        return None if odd else 0
    x = pow(x2, (P + 3) // 8, P)
    if (x * x - x2) % P:
        x = x * ROOT_M1 % P
    if (x * x - x2) % P:
        return None
    return P - x if (x & 1) != odd else x


def _add(p, q):
    a = (p[1] - p[0]) * (q[1] - q[0]) % P
    b = (p[1] + p[0]) * (q[1] + q[0]) % P
    c = 2 * p[3] * q[3] * D % P
    d = 2 * p[2] * q[2] % P
    e, f, g, h = b - a, d - c, d + c, b + a
    return e * f % P, g * h % P, f * g % P, e * h % P


def _mul(n, p):
    q = (0, 1, 1, 0)   # the neutral point
    while n:
        if n & 1:
            q = _add(q, p)
        p = _add(p, p)
        n >>= 1
    return q


def _same(p, q):
    return (p[0] * q[2] - q[0] * p[2]) % P == 0 and (p[1] * q[2] - q[1] * p[2]) % P == 0


_BY = 4 * pow(5, P - 2, P) % P
_BX = _x_of(_BY, 0)
BASE = (_BX, _BY, 1, _BX * _BY % P)


def _pack(p):
    inv = pow(p[2], P - 2, P)
    x, y = p[0] * inv % P, p[1] * inv % P
    return (y | (x & 1) << 255).to_bytes(32, 'little')


def _unpack(b):
    y = int.from_bytes(b, 'little')
    x = _x_of(y & (1 << 255) - 1, y >> 255)
    y &= (1 << 255) - 1
    return None if x is None else (x, y, 1, x * y % P)


def _secret(seed):
    if len(seed) != 32:
        raise ValueError('an Ed25519 private seed is 32 bytes')
    h = hashlib.sha512(seed).digest()
    return int.from_bytes(h[:32], 'little') & (1 << 254) - 8 | 1 << 254, h[32:]


def _number(*parts):
    return int.from_bytes(hashlib.sha512(b''.join(parts)).digest(), 'little') % ORDER


def public_key(seed):
    return _pack(_mul(_secret(seed)[0], BASE))


def sign(seed, message):
    a, prefix = _secret(seed)
    public = _pack(_mul(a, BASE))
    r = _number(prefix, message)
    point = _pack(_mul(r, BASE))
    return point + ((r + _number(point, public, message) * a) % ORDER).to_bytes(32, 'little')


def verify(public, message, signature):
    if len(public) != 32 or len(signature) != 64:
        return False
    a, r = _unpack(public), _unpack(signature[:32])
    s = int.from_bytes(signature[32:], 'little')
    if a is None or r is None or s >= ORDER:
        return False
    return _same(_mul(s, BASE), _add(r, _mul(_number(signature[:32], public, message), a)))
