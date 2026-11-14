"""Draw Rekarina's icon: a royal-blue ocarina with a gold band on a forest-green medallion tile with a gold rim.

    python tools/make_rekarina_icon.py [--out runtime/rekarina.ico] [--png runtime/rekarina.png]

The picture is drawn at 1024 px from shapes (no game art) and reduced to the sizes Windows asks for (256 ... 16).
The instrument is a transverse ocarina seen from above: a tilted egg with the mouthpiece on its upper side, a gold band
around the mouthpiece and six finger holes, lit from the upper left. Behind it are a thin gold ring and three
four-point sparkles; a gold line runs along the tile's edge. The small sizes are drawn from a simpler layer (no ring,
rim or sparkles, a larger instrument with larger holes) so the shape still reads at 16 px.
"""
import argparse
import math
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFilter

S = 1024
FOREST_TOP, FOREST_BOTTOM, FOREST_GLOW = (26, 86, 64), (5, 24, 24), (86, 176, 128)
BLUE_LIGHT, BLUE, BLUE_DARK = (124, 184, 255), (40, 92, 210), (12, 30, 104)
GOLD_LIGHT, GOLD, GOLD_DARK = (255, 232, 150), (240, 190, 84), (150, 100, 30)
HOLE = (6, 16, 56)


def mix(a, b, t):
    return tuple(round(x + (y - x) * t) for x, y in zip(a, b))


def vgrad(size, stops):
    """A vertical gradient through (position, colour) stops, as an RGBA image."""
    col = Image.new('RGB', (1, size))
    for y in range(size):
        t = y / (size - 1)
        for (p0, c0), (p1, c1) in zip(stops, stops[1:]):
            if t <= p1:
                u = 0 if p1 == p0 else (t - p0) / (p1 - p0)
                col.putpixel((0, y), mix(c0, c1, u * u * (3 - 2 * u)))
                break
        else:
            col.putpixel((0, y), stops[-1][1])
    return col.resize((size, size)).convert('RGBA')


def tint(img, colour, mask):
    """img with `colour` laid over it where mask (L) is bright."""
    return Image.composite(Image.new('RGBA', img.size, colour + (255,)), img, mask)


def blob(size, box, level, blur):
    m = Image.new('L', (size, size), 0)
    ImageDraw.Draw(m).ellipse(box, fill=level)
    return m.filter(ImageFilter.GaussianBlur(blur))


def tile(size, rim):
    """The rounded forest-green tile: lighter in the middle, darker in the corners, a gold line along its edge."""
    k = size / 1024
    img = vgrad(size, [(0, FOREST_TOP), (1, FOREST_BOTTOM)])
    img = tint(img, FOREST_GLOW, blob(size, (150 * k, 170 * k, 874 * k, 860 * k), 150, 120 * k))
    corners = ImageChops.invert(blob(size, (-40 * k, -40 * k, 1064 * k, 1064 * k), 255, 90 * k)).point(lambda v: v * 3 // 5)
    img = tint(img, (2, 12, 14), corners)
    if rim:
        line = Image.new('L', (size, size), 0)
        d = ImageDraw.Draw(line)
        d.rounded_rectangle((40 * k, 40 * k, 984 * k, 984 * k), radius=188 * k, outline=255, width=round(14 * k))
        d.rounded_rectangle((70 * k, 70 * k, 954 * k, 954 * k), radius=162 * k, outline=120, width=max(1, round(4 * k)))
        img = Image.composite(vgrad(size, [(0, GOLD_LIGHT), (.5, GOLD), (1, GOLD_DARK)]), img, line)
    mask = Image.new('L', (size, size), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, size - 1, size - 1), radius=round(size * .22), fill=255)
    img.putalpha(mask)
    return img


def ring(size):
    """A thin gold ring behind the instrument, with a soft glow."""
    k = size / 1024
    m = Image.new('L', (size, size), 0)
    d = ImageDraw.Draw(m)
    d.ellipse((182 * k, 215 * k, 842 * k, 875 * k), outline=235, width=round(12 * k))
    d.ellipse((214 * k, 247 * k, 810 * k, 843 * k), outline=110, width=max(1, round(4 * k)))
    layer = vgrad(size, [(0, GOLD_LIGHT), (.55, GOLD), (1, GOLD_DARK)])
    layer.putalpha(m)
    glow = Image.new('RGBA', (size, size), GOLD + (0,))
    glow.putalpha(m.filter(ImageFilter.GaussianBlur(16 * k)).point(lambda v: v // 2))
    glow.alpha_composite(layer)
    return glow


def egg(cx, cy, a, b, taper, steps=360):
    """An egg outline: an ellipse whose half-height shrinks towards +x by `taper`."""
    pts = []
    for i in range(steps):
        t = 2 * math.pi * i / steps
        x = math.cos(t)
        pts.append((cx + a * x, cy + b * math.sin(t) * (1 - taper * (x + 1) / 2)))
    return pts


def ocarina(size, simple):
    """The instrument on a transparent layer, drawn level and rotated afterwards."""
    k = size / 1024
    cx, cy = 512 * k, 560 * k
    body = egg(cx, cy, 335 * k, 208 * k, .42)
    # mouthpiece: a short spout on the upper side, left of the middle
    mx, my = cx - 70 * k, cy - 150 * k
    spout = [(mx - 80 * k, my + 60 * k), (mx - 52 * k, my - 150 * k), (mx + 64 * k, my - 150 * k), (mx + 98 * k, my + 60 * k)]
    shape = Image.new('L', (size, size), 0)
    sd = ImageDraw.Draw(shape)
    sd.polygon(body, fill=255)
    sd.polygon(spout, fill=255)
    sd.ellipse((mx - 60 * k, my - 190 * k, mx + 70 * k, my - 112 * k), fill=255)
    y0, y1 = (my - 190 * k) / size, (cy + 208 * k) / size
    layer = vgrad(size, [(0, BLUE_LIGHT), (y0, BLUE_LIGHT), (y0 + (y1 - y0) * .45, BLUE), (y1, BLUE_DARK), (1, BLUE_DARK)])
    # light from the upper left: a broad sheen and a small bright streak
    sheen = ImageChops.multiply(blob(size, (cx - 300 * k, cy - 190 * k, cx + 150 * k, cy - 10 * k), 150, 34 * k), shape)
    layer = tint(layer, (214, 236, 255), sheen)
    streak = ImageChops.multiply(blob(size, (cx - 230 * k, cy - 150 * k, cx - 40 * k, cy - 96 * k), 230, 12 * k), shape)
    layer = tint(layer, (255, 255, 255), streak)
    # light bounced from below along the lower edge, and a dark line around the silhouette
    inner = shape.filter(ImageFilter.GaussianBlur(14 * k))
    low = Image.new('L', (size, size), 0)
    low.paste(inner, (0, round(-26 * k)))
    bounce = ImageChops.multiply(ImageChops.subtract(shape, low), blob(size, (cx - 300 * k, cy + 40 * k, cx + 330 * k, cy + 260 * k), 150, 30 * k))
    layer = tint(layer, (92, 150, 255), bounce)
    edge = ImageChops.subtract(shape, shape.filter(ImageFilter.MinFilter(2 * round(4 * k) + 1))).point(lambda v: v * 2 // 3)
    layer = tint(layer, (6, 18, 70), edge)
    layer.putalpha(shape)
    d = ImageDraw.Draw(layer)
    # the gold band around the mouthpiece
    band = [(mx - 68 * k, my - 18 * k), (mx - 57 * k, my - 82 * k), (mx + 73 * k, my - 82 * k), (mx + 86 * k, my - 18 * k)]
    bm = Image.new('L', (size, size), 0)
    ImageDraw.Draw(bm).polygon(band, fill=255)
    gold = vgrad(size, [(0, GOLD_LIGHT), ((my - 82 * k) / size, GOLD_LIGHT), ((my - 40 * k) / size, GOLD), ((my - 18 * k) / size, GOLD_DARK), (1, GOLD_DARK)])
    gold = tint(gold, (255, 250, 220), ImageChops.multiply(blob(size, (mx - 50 * k, my - 84 * k, mx + 0 * k, my - 16 * k), 200, 8 * k), bm))
    layer.paste(gold, (0, 0), bm)
    d = ImageDraw.Draw(layer)
    d.line([band[0], band[3]], fill=GOLD_DARK, width=max(1, round(7 * k)))
    d.line([band[1], band[2]], fill=GOLD_DARK, width=max(1, round(5 * k)))
    # finger holes: two rows of three, larger towards the wide end
    r = (42 if simple else 31) * k
    holes = [(-215, -40, 1.15), (-80, -62, 1.0), (55, -52, .9), (-170, 78, 1.1), (-30, 70, 1.0), (105, 52, .85)]
    for hx, hy, s in holes:
        x, y, rr = cx + hx * k, cy + hy * k, r * s
        d.ellipse((x - rr, y - rr, x + rr, y + rr), fill=HOLE)
        if not simple:
            d.arc((x - rr, y - rr, x + rr, y + rr), 20, 160, fill=(150, 200, 255), width=max(1, round(5 * k)))
            d.arc((x - rr, y - rr, x + rr, y + rr), 200, 340, fill=(2, 8, 36), width=max(1, round(5 * k)))
    return layer.rotate(-24, resample=Image.BICUBIC, center=(cx, cy))


def sparkle(d, x, y, r):
    """A four-point star."""
    w = r * .2
    d.polygon([(x, y - r), (x + w, y - w), (x + r, y), (x + w, y + w), (x, y + r), (x - w, y + w), (x - r, y), (x - w, y - w)],
              fill=(255, 244, 200, 255))


def sparkles(size):
    k = size / 1024
    layer = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    for x, y, r in ((772, 236, 74), (872, 352, 40), (668, 168, 34)):
        sparkle(d, x * k, y * k, r * k)
    glow = Image.new('RGBA', (size, size), GOLD + (0,))
    glow.putalpha(layer.split()[3].filter(ImageFilter.GaussianBlur(18 * k)).point(lambda v: min(255, v * 2)))
    glow.alpha_composite(layer)
    return glow


def draw(simple=False):
    img = tile(S, not simple)
    if not simple:
        img.alpha_composite(ring(S))
    oc = ocarina(S, simple)
    if simple:   # larger in the small sizes
        oc = oc.resize((round(S * 1.16), round(S * 1.16)), Image.LANCZOS).crop((round(S * .08), round(S * .1), round(S * .08) + S, round(S * .1) + S))
    shadow = oc.split()[3].filter(ImageFilter.GaussianBlur(24)).point(lambda v: v // 2)
    off = Image.new('L', (S, S), 0)
    off.paste(shadow, (16, 34))
    img = Image.composite(Image.new('RGBA', (S, S), (0, 0, 0, 255)), img, ImageChops.multiply(off, img.split()[3]))
    img.alpha_composite(oc)
    if not simple:
        img.alpha_composite(sparkles(S))
    return img


def main():
    ap = argparse.ArgumentParser(description="Draw Rekarina's icon.")
    root = Path(__file__).resolve().parent.parent
    ap.add_argument('--out', type=Path, default=root / 'runtime' / 'rekarina.ico')
    ap.add_argument('--png', type=Path, default=root / 'runtime' / 'rekarina.png')
    a = ap.parse_args()
    full, simple = draw(False), draw(True)
    frames = [(full if n >= 48 else simple).resize((n, n), Image.LANCZOS) for n in (256, 128, 64, 48, 32, 24, 16)]
    frames[0].save(a.out, format='ICO', append_images=frames[1:], sizes=[(f.width, f.height) for f in frames])
    full.resize((256, 256), Image.LANCZOS).save(a.png)
    print(f'{a.out}: {a.out.stat().st_size} bytes; {a.png}')


if __name__ == '__main__':
    main()
