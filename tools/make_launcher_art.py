"""The pictures of Rekarina's launcher window (installer/gui_oot.py), written to installer/art/.

    python tools/make_launcher_art.py

The window is drawn from pictures, not from the toolkit's flat colours: a painted forest behind everything, the brown
band of the in-game menu across the top, khaki plates with the menu's grain for what is pressed, an amber plate for
the one thing to press next, a parchment sheet for what is read. Nothing here comes from the game: the painting
(tools/data/launcher-bg.jpg) and the parchment (tools/data/launcher-parchment.jpg) were made for this port with an
image generator, the grain is the menu's own (runtime/ui/tex/grain.a8, tools/make_menu_textures.py) and the logo is
tools/make_rekarina_logo.py's.

The plates are strips 600 pixels wide; the window cuts one to the width a button needs and keeps the strip's right
end as its edge (gui_oot.py plate_image). Sizes are those of the 1120 x 700 window (STAGE in gui_oot.py).
"""
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFilter

import make_rekarina_logo as logo_tool

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'installer' / 'art'
W, H, BAND = 1120, 700, 62
STRIP = 600
CREAM, INK = (246, 239, 214), (58, 42, 24)
KHAKI, KHAKI_HOT, KHAKI_EDGE = ((111, 90, 53), (98, 79, 46)), ((128, 106, 64), (113, 92, 54)), (140, 118, 72)
AMBER, AMBER_HOT, AMBER_EDGE = ((217, 149, 31), (190, 124, 22)), ((232, 166, 46), (206, 138, 30)), (240, 184, 74)


def grain(size, strength):
    """The menu's grain as a dark tint over `size`, its coverage scaled by strength (0..255)."""
    g = Image.frombytes('L', (256, 256), (ROOT / 'runtime' / 'ui' / 'tex' / 'grain.a8').read_bytes())
    tile = Image.new('L', size)
    for y in range(0, size[1], 256):
        for x in range(0, size[0], 256):
            tile.paste(g, (x, y))
    out = Image.new('RGBA', size, (18, 10, 4, 0))
    out.putalpha(tile.point(lambda v: v * strength // 255))
    return out


def cover(im, size):
    k = max(size[0] / im.width, size[1] / im.height)
    im = im.resize((round(im.width * k) + 1, round(im.height * k) + 1), Image.LANCZOS)
    x, y = (im.width - size[0]) // 2, (im.height - size[1]) // 2
    return im.crop((x, y, x + size[0], y + size[1]))


def vgrad(size, top, bottom):
    im = Image.new('RGBA', size)
    d = ImageDraw.Draw(im)
    for y in range(size[1]):
        t = y / max(1, size[1] - 1)
        d.line((0, y, size[0], y), fill=tuple(round(a + (b - a) * t) for a, b in zip(top, bottom)))
    return im


def plate(size, colours, edge, alpha=255, strength=46):
    """A plate: a soft gradient with the grain over it and a hairline around it."""
    im = vgrad(size, colours[0] + (alpha,), colours[1] + (alpha,))
    im.alpha_composite(grain(size, strength))
    ImageDraw.Draw(im).rectangle((0, 0, size[0] - 1, size[1] - 1), outline=edge + (255,))
    return im


def soft_shadow(size, box, blur, alpha, offset):
    sh = Image.new('RGBA', size, (0, 0, 0, 0))
    ImageDraw.Draw(sh).rectangle((box[0] + offset[0], box[1] + offset[1], box[2] + offset[0], box[3] + offset[1]),
                                 fill=(0, 0, 0, alpha))
    return sh.filter(ImageFilter.GaussianBlur(blur))


def cursor(size, margin=14):
    """The game's cursor for a box of `size`: a white frame with a pale glow outside it, on a picture `margin` larger
    on every side."""
    im = Image.new('RGBA', (size[0] + margin * 2, size[1] + margin * 2), (0, 0, 0, 0))
    box = (margin, margin, margin + size[0], margin + size[1])
    ImageDraw.Draw(im).rectangle((box[0] - 5, box[1] - 5, box[2] + 5, box[3] + 5), outline=(190, 215, 255, 190), width=6)
    im = im.filter(ImageFilter.GaussianBlur(5))
    ImageDraw.Draw(im).rectangle((box[0] - 3, box[1] - 3, box[2] + 2, box[3] + 2), outline=(255, 255, 255, 255), width=2)
    return im


def tinted_logo(height, colour):
    font = logo_tool.default_font()
    im = logo_tool.logo(font, 300)
    im = im.crop(im.getbbox())
    im = im.resize((round(im.width * height / im.height), height), Image.LANCZOS)
    out = Image.new('RGBA', im.size, colour + (0,))
    out.putalpha(im.getchannel('A'))
    return out


def background():
    """The painting, lighter than it was made (it was painted to sit behind panels, darker than a window wants), with
    the band across the top: the menu's brown gradient and grain, a dark line and a soft shadow under it."""
    bg = cover(Image.open(ROOT / 'tools' / 'data' / 'launcher-bg.jpg').convert('RGB'), (W, H))
    bg = Image.eval(bg, lambda v: min(255, int(v * 1.35 + 6))).convert('RGBA')
    bg.alpha_composite(vgrad((W, H), (10, 6, 3, 60), (10, 6, 3, 0)))
    bg.alpha_composite(vgrad((W, 16), (0, 0, 0, 130), (0, 0, 0, 0)), (0, BAND + 2))
    band = vgrad((W, BAND), (84, 66, 44, 240), (104, 83, 54, 240))
    band.alpha_composite(grain((W, BAND), 70))
    bg.alpha_composite(band, (0, 0))
    ImageDraw.Draw(bg).rectangle((0, BAND, W, BAND + 2), fill=(26, 17, 10, 255))
    return bg.convert('RGB')


def sheet(size, margin=24):
    """The parchment sheet with its shadow, on a picture `margin` larger on every side."""
    paper = cover(Image.open(ROOT / 'tools' / 'data' / 'launcher-parchment.jpg').convert('RGB'), size).convert('RGBA')
    paper = ImageChops.multiply(paper, Image.new('RGBA', size, (236, 222, 196, 255)))
    edge = Image.new('L', size, 0)
    ImageDraw.Draw(edge).rectangle((0, 0, size[0] - 1, size[1] - 1), outline=255, width=26)
    dark = Image.new('RGBA', size, (70, 40, 10, 0))
    dark.putalpha(edge.filter(ImageFilter.GaussianBlur(22)).point(lambda v: v * 120 // 255))
    paper.alpha_composite(dark)
    full = (size[0] + margin * 2, size[1] + margin * 2)
    out = soft_shadow(full, (margin, margin, margin + size[0], margin + size[1]), 14, 190, (0, 6))
    out.alpha_composite(paper, (margin, margin))
    return out


def step_row(width, cell, dim):
    """A step of the list: the khaki strip with the dark value cell at its right."""
    im = plate((width, 46), KHAKI, KHAKI_EDGE, 190 if dim else 240)
    ImageDraw.Draw(im).rectangle((width - cell - 3, 3, width - 4, 42), fill=(30, 26, 18, 255))
    return im


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    pictures = {
        'bg': background(),
        'sheet': sheet((748, 542)),
        'logo': tinted_logo(30, CREAM),
        'logo-ink': tinted_logo(46, INK),
        'plate46': plate((STRIP, 46), KHAKI, KHAKI_EDGE, 235), 'plate46-hot': plate((STRIP, 46), KHAKI_HOT, CREAM, 245),
        'plate40': plate((STRIP, 40), KHAKI, KHAKI_EDGE, 235), 'plate40-hot': plate((STRIP, 40), KHAKI_HOT, CREAM, 245),
        'play76': plate((STRIP, 76), AMBER, AMBER_EDGE, 255, 40), 'play76-hot': plate((STRIP, 76), AMBER_HOT, CREAM, 255, 40),
        'go40': plate((STRIP, 40), AMBER, AMBER_EDGE, 255, 40), 'go40-hot': plate((STRIP, 40), AMBER_HOT, CREAM, 255, 40),
        'row': step_row(1052, 327, False), 'row-dim': step_row(1052, 327, True),
        'fill': vgrad((321, 34), (217, 149, 31, 255), (196, 130, 27, 255)),
        'total': Image.new('RGBA', (1052, 44), (49, 35, 24, 235)),
        'cursor-play': cursor((266, 76)),
        'cursor-cell': cursor((329, 46)),
    }
    for name, im in pictures.items():
        im.save(OUT / f'{name}.png')
    print(f'wrote {len(pictures)} pictures to {OUT}')


if __name__ == '__main__':
    main()
