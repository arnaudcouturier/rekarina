"""Rekarina's mark, icon and logo: a flat cream ocarina on the dark wood of the port's menu, and the mark beside the
name set in capitals and small capitals. No game artwork: the silhouette is tools/data/rekarina-mark.png, an 8-bit
coverage mask (white = the mark) made for this port with an image generator from our own sketch and cleaned to one flat
shape.

    python tools/make_rekarina_logo.py [--font <ttf>] [--out <dir>]

Writes, into <dir> (default docs/media): mark.png (the cream mark alone, transparent), icon.png (the 1024 disc),
logo.png (mark and name, transparent, for dark pages), logo-dark.png (the same on the wood colour, for pages with a
light background), and into runtime/: rekarina.png (256), rekarina.ico (16..256) and ui/tex/logo.a8 (the logo's
coverage for the in-game menu). The launcher's own copies of the logo are tools/make_launcher_art.py's. The small icon sizes are drawn
separately with the mark filling more of the disc, so 16 and 24 pixels still read as an ocarina.
The name needs a bold serif face with capitals; the default is EB Garamond ExtraBold (SIL Open Font License) when it
is installed, else Georgia Bold.
"""
import argparse
import os
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
MARK = ROOT / 'tools' / 'data' / 'rekarina-mark.png'
WOOD = (42, 29, 18, 255)
CREAM = (246, 239, 214, 255)
SS = 4   # supersampling of the tile's rounded corners
LOGO_A8_HEIGHT = 80   # rows of the menu's copy of the logo: under 512 wide, so it fits the smallest font atlas


def mark(width, colour=CREAM):
    """The mark `width` pixels wide in one colour, its coverage as the alpha."""
    m = Image.open(MARK).convert('L')
    m = m.resize((width, max(1, round(m.height * width / m.width))), Image.LANCZOS)
    im = Image.new('RGBA', m.size, colour[:3] + (0,))
    im.putalpha(m)
    return im


def tile(size, fill=0.7, radius=0.5):
    """The icon: a plate of the wood colour with the mark `fill` of its width, centred by its box. radius is the
    corner radius as a share of the side: 0.5 is a disc, smaller a rounded square."""
    s = size * SS
    plate = Image.new('L', (s, s), 0)
    ImageDraw.Draw(plate).rounded_rectangle((0, 0, s - 1, s - 1), radius=int(s * radius), fill=255)
    im = Image.new('RGBA', (size, size), WOOD[:3] + (0,))
    im.putalpha(plate.resize((size, size), Image.LANCZOS))
    m = mark(max(1, round(size * fill)))
    im.alpha_composite(m, ((size - m.width) // 2, (size - m.height) // 2))
    return im


def small_caps(text, font_path, cap_px):
    """The name in capitals and small capitals: [(char, font), ...] with the first letter full height."""
    big = ImageFont.truetype(font_path, cap_px)
    small = ImageFont.truetype(font_path, int(cap_px * 0.76))
    return [(ch.upper(), big if i == 0 else small) for i, ch in enumerate(text)]


def wordmark(text, font_path, cap_px, colour):
    glyphs = small_caps(text, font_path, cap_px)
    track = cap_px * 0.045
    width = sum(f.getlength(c) for c, f in glyphs) + track * (len(glyphs) - 1)
    im = Image.new('RGBA', (int(width) + 8, int(cap_px * 1.2)), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    base = int(cap_px * 0.98)
    x = 2
    for c, f in glyphs:
        d.text((x, base), c, font=f, fill=colour, anchor='ls')
        x += f.getlength(c) + track
    return im.crop(im.getbbox())


def logo(font_path, height, background=None):
    """The mark and the name on one line: the name's capitals as tall as about two thirds of the mark."""
    m = mark(round(height * 1.28))
    wm = wordmark('Rekarina', font_path, int(height * 0.74), CREAM)
    gap = int(height * 0.22)
    pad = int(height * 0.3) if background else 0
    h = max(m.height, wm.height)
    im = Image.new('RGBA', (m.width + gap + wm.width + pad * 2, h + pad * 2), background or (0, 0, 0, 0))
    im.alpha_composite(m, (pad, pad + (h - m.height) // 2))
    im.alpha_composite(wm, (pad + m.width + gap, pad + (h - wm.height) // 2 + int(height * 0.03)))
    return im


def default_font():
    local = Path(os.environ.get('LOCALAPPDATA', '')) / 'Microsoft' / 'Windows' / 'Fonts' / 'EBGaramond-ExtraBold.ttf'
    for p in (local, Path('C:/Windows/Fonts/EBGaramond-ExtraBold.ttf'), Path('C:/Windows/Fonts/georgiab.ttf')):
        if p.is_file():
            return str(p)
    raise SystemExit('no serif face found: pass --font <ttf>')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--font', default=None)
    ap.add_argument('--out', type=Path, default=ROOT / 'docs' / 'media')
    a = ap.parse_args()
    font = a.font or default_font()
    a.out.mkdir(parents=True, exist_ok=True)
    mark(1024).save(a.out / 'mark.png')
    tile(1024).save(a.out / 'icon.png')
    logo(font, 300).save(a.out / 'logo.png')
    logo(font, 300, WOOD).save(a.out / 'logo-dark.png')
    tile(256).save(ROOT / 'runtime' / 'rekarina.png')
    # the menu's band shows the logo (runtime/ui/menu.cpp): its coverage alone, LOGO_A8_HEIGHT rows, after a header of
    # two 16-bit little-endian numbers, the width and the height; the menu tints it
    cover = logo(font, 300).getchannel('A')
    cover = cover.crop(cover.getbbox())
    cover = cover.resize((round(cover.width * LOGO_A8_HEIGHT / cover.height), LOGO_A8_HEIGHT), Image.LANCZOS)
    (ROOT / 'runtime' / 'ui' / 'tex' / 'logo.a8').write_bytes(
        cover.width.to_bytes(2, 'little') + cover.height.to_bytes(2, 'little') + cover.tobytes())
    # each size drawn on its own: the smallest ones with a larger mark
    sizes = [(16, 0.8), (20, 0.78), (24, 0.76), (32, 0.74), (40, 0.72), (48, 0.71), (64, 0.7), (128, 0.7), (256, 0.7)]
    frames = [tile(s, fill) for s, fill in sizes]
    frames[-1].save(ROOT / 'runtime' / 'rekarina.ico', format='ICO', sizes=[(s, s) for s, _ in sizes],
                    append_images=frames[:-1])
    print(f'wrote {a.out}/mark.png, icon.png, logo.png, logo-dark.png and runtime/rekarina.png, .ico (font {font})')


if __name__ == '__main__':
    main()
