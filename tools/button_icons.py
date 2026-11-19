# Adapted from MacNativeGames games/totk/scripts/make_button_icons.py @ 933b6a0 (MIT, Copyright (c) 2026
# MacNativeGames contributors).
"""Xbox and keyboard-and-mouse button icons for the game's prompts, drawn on the player's machine for
tools/make_romfs_patches.py --prompts.
The outlines are our own shapes with letters cut out of them in the game's own text fonts: nothing of the game is in
the repository.

Every prompt except the button guide in play is text in the button font, scft/External.bfotf of each
Font/Font*.Nin_NX_NVN.bfarc.zs: private-use code points from U+E040 (A, B, X, Y, then L, R, ZL, ZR, the d-pad, Plus and
Minus, the sticks). A .bfotf is an OpenType (CFF) font after an 8-byte header; from offset 4 on, every byte is XORed
with a 4-byte key, which the size in the header's second word gives away (bfotf_decode). redraw_font draws the shoulder
buttons, the triggers, Plus, Minus and the d-pad as an Xbox controller names them (LB RB, LT RT, Menu and View, a
cross), or every button as the key or mouse button the keyboard presses it with (pc_icons), can swap the glyphs of
the face buttons (A with B, X with Y), and has fontTools write the font again. The archive then grows by a kilobyte or
two, more than the game's resource size table allows it: grow_restbl raises those entries (without it the game stops at
start, MacNativeGames found).

The button guide in play (bottom right) draws its buttons, L and ZL among them, from images of the common layout
archive instead (timg/__Combined.bntx, Nt_KeyTexL_00 and Nt_KeyTexZL_00): bntx_redraw paints the same icons into them,
the size unchanged. The Xbox prompts redraw L and ZL, the keyboard and mouse ones every image (PC_HUD_TEXTURES: A, B, X,
Y, L, ZL and the stick's two). Prompts drawn as fixed images (some tutorial animations) keep the Switch icons.

Needs fontTools (tools/requirements-installer.txt, tools/requirements-py314.txt).
"""
import io
import math
import struct
import zlib

from fontTools.pens.basePen import BasePen
from fontTools.pens.boundsPen import BoundsPen
from fontTools.pens.recordingPen import RecordingPen
from fontTools.pens.reverseContourPen import ReverseContourPen
from fontTools.pens.t2CharStringPen import T2CharStringPen
from fontTools.pens.transformPen import TransformPen
from fontTools.ttLib import TTFont
from fontTools.ttLib.tables.DefaultTable import DefaultTable

FONT = 'scft/External.bfotf'
# The letters on the keys come from the text fonts of the default archive; the regional archives lack them.
TEXT_ARCHIVE = 'Font/Font.Nin_NX_NVN.bfarc.zs'
TEXT_FONTS = ('scft/ninP_RodinNTLG-B_DH_003_subset.bfotf', 'scft/nintendo_NTLG-DB_DH_002.bfotf')
# The button guide's images and the glyph each one shows.
HUD_TEXTURES = {'Nt_KeyTexL_00': 0xE044, 'Nt_KeyTexZL_00': 0xE046}


def xor(data, key):
    """data with byte i XORed with key[i % 4]."""
    pad = (key * (len(data) // 4 + 1))[:len(data)]
    return (int.from_bytes(data, 'little') ^ int.from_bytes(pad, 'little')).to_bytes(len(data), 'little')


def bfotf_decode(blob):
    """(OpenType font, key) of a .bfotf (the module docstring). The font fills the rest of the file."""
    key = xor(blob[4:8], struct.pack('>I', len(blob) - 8))
    otf = xor(blob[8:], key)
    assert otf[:4] == b'OTTO', 'not a CFF OpenType font'
    return otf, key


def bfotf_encode(otf, key, head):
    """The .bfotf of otf: head (the original's first word) and the size, the rest XORed with key as bfotf_decode reads
    it. The size field holds the whole font, so its length has to be a multiple of 4 (fontTools pads its tables)."""
    assert len(otf) % 4 == 0, len(otf)
    return head[:4] + xor(struct.pack('>I', len(otf)) + otf, key)


# ---- drawing, in units of the font's 1000-unit em; the round A/B/X/Y icons fill y 20..585
Y0, Y1 = 20, 585
CY, R = (Y0 + Y1) / 2, (Y1 - Y0) / 2
K = 0.5523   # control point distance of a quarter circle drawn with one cubic curve, per unit of radius


class Glyphs:
    """Outlines of letters from the game's own text fonts, drawn as holes in a shape."""
    def __init__(self, fonts):
        self.fonts = fonts

    def outline(self, ch):
        for font in self.fonts:
            cmap = font.getBestCmap()
            if ord(ch) in cmap:
                glyphs = font.getGlyphSet(); g = glyphs[cmap[ord(ch)]]
                rec = RecordingPen(); g.draw(rec); box = BoundsPen(glyphs); g.draw(box)
                return rec, box.bounds, font['head'].unitsPerEm, g.width
        raise KeyError(f'no text font has {ch!r}')

    def hole(self, pen, text, cx, cy, cap, max_width, ref=None):
        """text centred on (cx, cy), at most cap high and max_width wide, its contours reversed so they cut holes.
        ref: a letter whose height the text counts as at least (a key's lone '.' or '-' is then as small beside a
        capital as it is in print, instead of filling the key)."""
        parts, x, ink = [], 0.0, [1e9, 1e9, -1e9, -1e9]
        for ch in text:
            rec, b, upm, advance = self.outline(ch); k = 1000 / upm
            parts.append((rec, k, x))
            ink = [min(ink[0], x + b[0] * k), min(ink[1], b[1] * k), max(ink[2], x + b[2] * k), max(ink[3], b[3] * k)]
            x += advance * k * 0.96
        width, height = ink[2] - ink[0], ink[3] - ink[1]
        tall = height
        if ref:
            _, rb, rupm, _ = self.outline(ref)
            tall = max(height, (rb[3] - rb[1]) * 1000 / rupm)
        scale = min(cap / tall, max_width / width)
        ox, oy = cx - (ink[0] + width / 2) * scale, cy - (ink[1] + height / 2) * scale
        reverse = ReverseContourPen(pen)
        for rec, k, x in parts:
            rec.replay(TransformPen(reverse, (scale * k, 0, 0, scale * k, ox + x * scale, oy)))

    def fill(self, pen, text, cx, cy, cap, max_width):
        """text centred on (cx, cy) as hole() places it, its contours as they are: solid letters (in an empty area)."""
        parts, x, ink = [], 0.0, [1e9, 1e9, -1e9, -1e9]
        for ch in text:
            rec, b, upm, advance = self.outline(ch); k = 1000 / upm
            parts.append((rec, k, x))
            ink = [min(ink[0], x + b[0] * k), min(ink[1], b[1] * k), max(ink[2], x + b[2] * k), max(ink[3], b[3] * k)]
            x += advance * k * 0.96
        width, height = ink[2] - ink[0], ink[3] - ink[1]
        scale = min(cap / height, max_width / width)
        ox, oy = cx - (ink[0] + width / 2) * scale, cy - (ink[1] + height / 2) * scale
        for rec, k, x in parts:
            rec.replay(TransformPen(pen, (scale * k, 0, 0, scale * k, ox + x * scale, oy)))


def text_glyphs(blobs):
    """Glyphs over the text fonts in blobs (.bfotf files), the first that has a letter giving it."""
    return Glyphs([TTFont(io.BytesIO(bfotf_decode(blob)[0])) for blob in blobs])


def circle(pen, cx, cy, r):
    k = K * r
    pen.moveTo((cx + r, cy))
    pen.curveTo((cx + r, cy + k), (cx + k, cy + r), (cx, cy + r))
    pen.curveTo((cx - k, cy + r), (cx - r, cy + k), (cx - r, cy))
    pen.curveTo((cx - r, cy - k), (cx - k, cy - r), (cx, cy - r))
    pen.curveTo((cx + k, cy - r), (cx + r, cy - k), (cx + r, cy))
    pen.closePath()


def rounded(pen, x0, y0, x1, y1, bottom, top=None):
    """Counter-clockwise rounded rectangle (bottom and top corner radii)."""
    top = bottom if top is None else top
    kb, kt = K * bottom, K * top
    pen.moveTo((x0 + bottom, y0)); pen.lineTo((x1 - bottom, y0))
    pen.curveTo((x1 - bottom + kb, y0), (x1, y0 + bottom - kb), (x1, y0 + bottom)); pen.lineTo((x1, y1 - top))
    pen.curveTo((x1, y1 - top + kt), (x1 - top + kt, y1), (x1 - top, y1)); pen.lineTo((x0 + top, y1))
    pen.curveTo((x0 + top - kt, y1), (x0, y1 - top + kt), (x0, y1 - top)); pen.lineTo((x0, y0 + bottom))
    pen.curveTo((x0, y0 + bottom - kb), (x0 + bottom - kb, y0), (x0 + bottom, y0)); pen.closePath()


def polygon(pen, points):
    pen.moveTo(points[0])
    for p in points[1:]:
        pen.lineTo(p)
    pen.closePath()


def counter_clockwise(points):
    """points in counter-clockwise order (as the outlines are drawn: y up), reversed when they run the other way."""
    area = sum(points[i][0] * points[(i + 1) % len(points)][1] - points[(i + 1) % len(points)][0] * points[i][1]
               for i in range(len(points)))
    return points if area > 0 else points[::-1]


def capsule(cx, y0, y1, w, n=48):
    """A stadium standing up, w wide from y0 to y1: half circles at both ends, as points counter-clockwise."""
    r = w / 2
    pts = [(cx + r * math.cos(math.pi * i / n), y1 - r + r * math.sin(math.pi * i / n)) for i in range(n + 1)]
    pts += [(cx + r * math.cos(math.pi * (1 + i / n)), y0 + r + r * math.sin(math.pi * (1 + i / n))) for i in range(n + 1)]
    return counter_clockwise(pts)


def clip_half(points, keep):
    """The convex polygon points cut to the half-plane where keep (a linear function of a point) is not negative."""
    out = []
    for i in range(len(points)):
        p, q = points[i], points[(i + 1) % len(points)]
        fp, fq = keep(p), keep(q)
        if fp >= 0:
            out.append(p)
        if (fp >= 0) != (fq >= 0):
            t = fp / (fp - fq)
            out.append((p[0] + (q[0] - p[0]) * t, p[1] + (q[1] - p[1]) * t))
    return out


def bar(pen, x0, y0, x1, y1):
    polygon(pen, [(x0, y0), (x1, y0), (x1, y1), (x0, y1)])


def bold_arrow(pen, x, y, d, s, double=False):
    """A filled arrow pointing d ('up', 'down', 'left' or 'right'), s long, centred on (x, y), in the weight of the
    game's own arrows beside its sticks: the head three quarters as wide as the arrow is long, the shaft a third of
    the head. double: a head at both ends."""
    hl, hw, sw = s * 0.46, s * 0.37, s * 0.13
    if double:
        hl = s * 0.36
        pts = [(s / 2, 0), (s / 2 - hl, hw), (s / 2 - hl, sw), (-s / 2 + hl, sw), (-s / 2 + hl, hw), (-s / 2, 0),
               (-s / 2 + hl, -hw), (-s / 2 + hl, -sw), (s / 2 - hl, -sw), (s / 2 - hl, -hw)]
    else:
        pts = [(s / 2, 0), (s / 2 - hl, hw), (s / 2 - hl, sw), (-s / 2, sw), (-s / 2, -sw), (s / 2 - hl, -sw),
               (s / 2 - hl, -hw)]
    c, sn = {'right': (1, 0), 'left': (-1, 0), 'up': (0, 1), 'down': (0, -1)}[d]
    polygon(pen, counter_clockwise([(x + px * c - py * sn, y + px * sn + py * c) for px, py in pts]))


def xbox_icons(g):
    """Code point -> (advance, draw(pen)) for the buttons an Xbox controller names differently from a Switch: L R = LB
    RB, ZL ZR = LT RT, Plus and Minus = Menu and View, the d-pad as a cross. g: Glyphs for the letters. The face
    buttons keep the game's own glyphs (redraw_font swaps them for the Nintendo layout)."""
    def bumper(label, advance=820):
        def draw(pen):
            rounded(pen, 40, Y0 + 70, advance - 40, Y1 - 70, 70, 150); g.hole(pen, label, advance / 2, CY, 230, advance - 200)
        return advance, draw

    def trigger(label, advance=760):
        def draw(pen):
            rounded(pen, 60, Y0, advance - 60, Y1, 60, 230); g.hole(pen, label, advance / 2, CY - 20, 230, advance - 220)
        return advance, draw

    def menu(advance=640):   # three bars, as on the Menu button
        def draw(pen):
            circle(pen, advance / 2, CY, R); reverse = ReverseContourPen(pen)
            for dy in (-115, 0, 115):
                rounded(reverse, advance / 2 - 150, CY + dy - 38, advance / 2 + 150, CY + dy + 38, 38)
        return advance, draw

    def view(advance=640):   # two overlapping windows, as on the View button
        def draw(pen):
            circle(pen, advance / 2, CY, R); reverse = ReverseContourPen(pen); s = 150
            rounded(reverse, advance / 2 - s - 15, CY + 15, advance / 2 - 15, CY + s + 15, 16)
            rounded(reverse, advance / 2 + 15, CY - s - 15, advance / 2 + s + 15, CY - 15, 16)
        return advance, draw

    def dpad(*lit, advance=640):   # a cross with an arrow cut into each direction the prompt means
        def draw(pen):
            cx, length, arm = advance / 2, R, R * 0.40
            polygon(pen, [(cx + length, CY - arm), (cx + length, CY + arm), (cx + arm, CY + arm), (cx + arm, CY + length),
                          (cx - arm, CY + length), (cx - arm, CY + arm), (cx - length, CY + arm), (cx - length, CY - arm),
                          (cx - arm, CY - arm), (cx - arm, CY - length), (cx + arm, CY - length), (cx + arm, CY - arm)])
            m, t, h = (length + arm) / 2, arm * 0.72, arm * 0.80   # arm centre, half base and height of the arrow
            heads = {'up': [(cx - t, CY + m - h / 2), (cx, CY + m + h / 2), (cx + t, CY + m - h / 2)],
                     'down': [(cx + t, CY - m + h / 2), (cx, CY - m - h / 2), (cx - t, CY - m + h / 2)],
                     'left': [(cx - m + h / 2, CY - t), (cx - m - h / 2, CY), (cx - m + h / 2, CY + t)],
                     'right': [(cx + m - h / 2, CY + t), (cx + m + h / 2, CY), (cx + m - h / 2, CY - t)]}
            for d in lit:
                polygon(pen, heads[d])   # already clockwise: a hole
        return advance, draw

    return {
        0xE044: bumper('LB'), 0xE045: bumper('RB'), 0xE046: trigger('LT'), 0xE047: trigger('RT'),
        0xE04B: menu(), 0xE04C: view(),
        0xE049: dpad('up', 'down', 'left', 'right'), 0xE060: dpad('up'), 0xE061: dpad('down'),
        0xE062: dpad('left'), 0xE063: dpad('right'), 0xE064: dpad('up', 'down'), 0xE065: dpad('left', 'right'),
        0xE06A: dpad('left', 'down', 'right'),
    }


def playstation_icons(g, by_position=True):
    """Code point -> (advance, draw(pen)) for a PlayStation pad: the face buttons as their shapes in a filled disc, L1 R1
    L2 R2, Options (three bars) and Create (a pill under three rays), the d-pad as a cross. by_position: the Switch's A
    is the right button (circle), B the bottom one (cross), X the top (triangle), Y the left (square), which is how the
    Nintendo pad layout plays it; else A is the bottom button as on an Xbox pad (cross), B circle, X square, Y triangle."""
    base = xbox_icons(g)
    adv = 640

    def face(shape):
        def draw(pen):
            cx = adv / 2
            circle(pen, cx, CY, R)
            cut = ReverseContourPen(pen)
            if shape == 'circle':
                circle(cut, cx, CY, R * .58); circle(pen, cx, CY, R * .40)
            elif shape == 'square':
                s, t = R * .50, R * .33
                polygon(cut, [(cx - s, CY - s), (cx + s, CY - s), (cx + s, CY + s), (cx - s, CY + s)])
                polygon(pen, [(cx - t, CY - t), (cx + t, CY - t), (cx + t, CY + t), (cx - t, CY + t)])
            elif shape == 'triangle':
                s, t, dy = R * .66, R * .34, R * .10
                polygon(cut, [(cx - s * .866, CY - s * .5 - dy), (cx + s * .866, CY - s * .5 - dy), (cx, CY + s - dy)])
                polygon(pen, [(cx - t * .866, CY - t * .5 - dy), (cx + t * .866, CY - t * .5 - dy), (cx, CY + t - dy)])
            else:   # cross: one twelve-point outline of an X
                a, w = R * .52, R * .13
                pts = [(w, 0), (a, a - w), (a - w, a), (0, w), (-(a - w), a), (-a, a - w), (-w, 0), (-a, -(a - w)),
                       (-(a - w), -a), (0, -w), (a - w, -a), (a, -(a - w))]
                polygon(cut, [(cx + x, CY + y) for x, y in pts])
        return adv, draw

    def shoulder(label, trigger):
        width = 760 if trigger else 820
        def draw(pen):
            if trigger:
                rounded(pen, 60, Y0, width - 60, Y1, 60, 230); g.hole(pen, label, width / 2, CY - 20, 230, width - 220)
            else:
                rounded(pen, 40, Y0 + 70, width - 40, Y1 - 70, 70, 150); g.hole(pen, label, width / 2, CY, 230, width - 200)
        return width, draw

    def create():
        def draw(pen):
            cx = adv / 2
            circle(pen, cx, CY, R); cut = ReverseContourPen(pen)
            rounded(cut, cx - 70, CY - 170, cx + 70, CY + 20, 60)
            for dx, lean in ((-150, -45), (0, 0), (150, 45)):   # three rays above the pill
                polygon(cut, [(cx + dx - 26, CY + 70), (cx + dx + 26, CY + 70), (cx + dx + 26 + lean, CY + 180),
                              (cx + dx - 26 + lean, CY + 180)])
        return adv, draw

    a, b, x, y = ('circle', 'cross', 'triangle', 'square') if by_position else ('cross', 'circle', 'square', 'triangle')
    base.update({0xE040: face(a), 0xE041: face(b), 0xE042: face(x), 0xE043: face(y),
                 0xE044: shoulder('L1', False), 0xE045: shoulder('R1', False),
                 0xE046: shoulder('L2', True), 0xE047: shoulder('R2', True), 0xE04C: create()})
    return base


def playstation_symbols(by_position=True):
    """Code point -> (advance, draw(pen)) of the four face buttons' shapes alone, as lines of one weight (a ring, a
    cross, a triangle, a square): the symbol for a layout that draws its own button disc and puts the symbol on it
    (tools/make_oot_prompts.py KEY_TEXTURES). The A to Y order as playstation_icons with by_position."""
    adv, cx = 640, 320

    def shape(kind):
        def draw(pen):
            cut = ReverseContourPen(pen)
            if kind == 'circle':
                circle(pen, cx, CY, R * .62); circle(cut, cx, CY, R * .42)
            elif kind == 'square':
                s, t = R * .55, R * .35
                polygon(pen, [(cx - s, CY - s), (cx + s, CY - s), (cx + s, CY + s), (cx - s, CY + s)])
                polygon(cut, [(cx - t, CY - t), (cx + t, CY - t), (cx + t, CY + t), (cx - t, CY + t)])
            elif kind == 'triangle':   # the inner one 0.2 R further in on every side: its circumradius 0.4 R less
                for p, s in ((pen, R * .66), (cut, R * .26)):
                    polygon(p, [(cx - s * .866, CY - s * .5), (cx + s * .866, CY - s * .5), (cx, CY + s)])
            else:   # cross: one twelve-point outline of an X, counter-clockwise
                a, w = R * .55, R * .10
                polygon(pen, [(cx + x, CY + y) for x, y in (
                    (w, 0), (a, a - w), (a - w, a), (0, w), (-(a - w), a), (-a, a - w), (-w, 0), (-a, -(a - w)),
                    (-(a - w), -a), (0, -w), (a - w, -a), (a, -(a - w)))])
        return adv, draw

    a, b, x, y = ('circle', 'cross', 'triangle', 'square') if by_position else ('cross', 'circle', 'square', 'triangle')
    return {0xE040: shape(a), 0xE041: shape(b), 0xE042: shape(x), 0xE043: shape(y)}


def rekarina_icons(g, otf, by_position=True):
    """Code point -> (advance, draw(pen)) for Rekarina's own set: an Xbox pad's names (A B X Y, LB RB LT RT, Menu,
    View) on the game's own button outlines, so the prompts look as the game draws them and read as the pad in the
    player's hands. The face buttons are the game's glyphs (an Xbox pad names them alike); L, R, ZL and ZR keep the
    game's tapered and slanted plates with LB, RB, LT and RT cut out in the game's text font where the game cuts its
    own label; Plus and Minus keep the game's discs with the Menu sign (three bars) and the View sign (two windows) cut
    out. The d-pad and the sticks stay the game's. g: Glyphs for the labels; otf: the game's button font (OpenType).
    by_position: each prompt names the button at the Switch button's place (the Switch's A is the right button, an
    Xbox pad's B), for a pad played by position; else by its own letter."""
    font = TTFont(io.BytesIO(otf))
    cmap, glyphs, hmtx = font.getBestCmap(), font.getGlyphSet(), font['hmtx']

    def parts(cp):
        """The contours of the game's glyph for cp with their bounds, the largest (the plate) first."""
        rec = RecordingPen(); glyphs[cmap[cp]].draw(rec)
        out, cur = [], []
        for op, args in rec.value:
            cur.append((op, args))
            if op in ('closePath', 'endPath'):
                one = RecordingPen(); one.value = cur; box = BoundsPen(None); one.replay(box)
                out.append((one, box.bounds)); cur = []
        return sorted(out, key=lambda c: -(c[1][2] - c[1][0]) * (c[1][3] - c[1][1]))

    def game(cp):
        return hmtx[cmap[cp]][0], glyphs[cmap[cp]].draw

    def relabel(cp, label=None, sign=None):
        """The game's plate of cp with label (or sign: draw(pen, cx, cy, r) of a cut-out sign) in place of its own."""
        (plate, (x0, y0, x1, y1)), *own = parts(cp)
        lx0, ly0 = min(b[0] for _, b in own), min(b[1] for _, b in own)
        lx1, ly1 = max(b[2] for _, b in own), max(b[3] for _, b in own)

        def draw(pen):
            plate.replay(pen)
            if label:
                g.hole(pen, label, (lx0 + lx1) / 2, (ly0 + ly1) / 2, ly1 - ly0, (x1 - x0) * 0.78)
            else:
                sign(ReverseContourPen(pen), (x0 + x1) / 2, (y0 + y1) / 2, min(x1 - x0, y1 - y0) / 2)
        return hmtx[cmap[cp]][0], draw

    def bars(pen, cx, cy, r):      # Menu
        for d in (-0.36, 0, 0.36):
            rounded(pen, cx - r * .46, cy + d * r - r * .11, cx + r * .46, cy + d * r + r * .11, r * .11)

    def windows(pen, cx, cy, r):   # View
        s = r * .42
        rounded(pen, cx - s - r * .05, cy + r * .05, cx - r * .05, cy + s + r * .05, r * .06)
        rounded(pen, cx + r * .05, cy - s - r * .05, cx + s + r * .05, cy - r * .05, r * .06)

    out = {0xE044: relabel(0xE044, 'LB'), 0xE045: relabel(0xE045, 'RB'), 0xE046: relabel(0xE046, 'LT'),
           0xE047: relabel(0xE047, 'RT'), 0xE04B: relabel(0xE04B, sign=bars), 0xE04C: relabel(0xE04C, sign=windows)}
    if by_position:   # the letters trade places as on an Xbox pad: the Switch's A is its B, X its Y
        out.update({0xE040: game(0xE041), 0xE041: game(0xE040), 0xE042: game(0xE043), 0xE043: game(0xE042)})
    return out

# ---- keyboard and mouse prompts (--prompts pc): every button the keyboard drives, drawn as the key or mouse button
# bound to it, in the same filled-shape style. Each glyph keeps the advance of the game's own (a wider glyph runs into
# the text of a right-aligned guide).
# The keyboard defaults of runtime/input_map.c (input_map_reset; the rekarina.ini bind_ lines apply on top, pc_binds). The
# right stick is the camera, which the mouse drives; SHAKE is the key that shakes it (the mouse never does).
PC_BINDS = {
    'A': 'e', 'B': 'lshift', 'X': 'space', 'Y': 'mouse_l', 'L': 'q', 'R': 'r', 'ZL': 'mouse_r', 'ZR': 'f',
    'PLUS': 'tab', 'MINUS': 'm', 'LEFT': '1', 'UP': '2', 'RIGHT': '3', 'DOWN': '4', 'LCLICK': 'lctrl', 'RCLICK': 'c',
    'LSTICK_UP': 'w', 'LSTICK_LEFT': 'a', 'LSTICK_DOWN': 's', 'LSTICK_RIGHT': 'd', 'SHAKE': 'g',
}
# What a key shows: its letter, a symbol or a short name (key names as in rekarina.ini; the rest show their name).
PC_LABELS = {
    'lshift': '⇧', 'rshift': '⇧', 'lctrl': 'CTRL', 'rctrl': 'CTRL', 'lalt': 'ALT', 'tab': 'TAB',
    'enter': 'ENT', 'backspace': 'BKSP', 'capslock': 'CAPS', 'insert': 'INS', 'delete': 'DEL', 'home': 'HOME',
    'end': 'END', 'pageup': 'PGUP', 'pagedown': 'PGDN', 'up': '↑', 'down': '↓', 'left': '←',
    'right': '→', 'num_add': '+', 'num_sub': '-', 'num_mul': '*', 'num_div': '/', 'num_decimal': '.',
    'minus': '-', 'equals': '=', 'lbracket': '[', 'rbracket': ']', 'semicolon': ';', 'apostrophe': "'", 'comma': ',',
    'period': '.', 'slash': '/', 'backslash': '\\', 'grave': '`', 'mouse_x1': 'M4', 'mouse_x2': 'M5',
}
# The one-letter panes around the action prompt (T_A_00 ...) are in the Normal font (Rodin B and Symbol): one character
# of it per key.
PC_LETTER_LABELS = {'lshift': '↑', 'rshift': '↑', 'space': '▬', 'mouse_l': 'L', 'mouse_r': 'R',
                    'mouse_m': 'M', 'lctrl': 'C', 'rctrl': 'C', 'tab': 'T'}
# The button guide's images in the pc style: every symbol mask of the action prompt. In the 1.4.3 Common archive
# LSt_00 shows the left stick tilted up (as U+E052) and LSt_01 the left stick (as U+E05E).
PC_HUD_TEXTURES = {'Nt_KeyTexA_00': 0xE040, 'Nt_KeyTexB_00': 0xE041, 'Nt_KeyTexX_00': 0xE042, 'Nt_KeyTexY_00': 0xE043,
                   'Nt_KeyTexL_00': 0xE044, 'Nt_KeyTexZL_00': 0xE046, 'Nt_KeyTexLSt_00': 0xE052,
                   'Nt_KeyTexLSt_01': 0xE05E}
PC_MOUSE = ('mouse_l', 'mouse_r', 'mouse_m')


def pc_binds(ini_text=None):
    """PC_BINDS with the first key of each bind_<action> line of a rekarina.ini text over it (a line without a key entry,
    or with key:default, keeps the default)."""
    binds = dict(PC_BINDS)
    for line in (ini_text or '').splitlines():
        line = line.split('#')[0].split(';')[0].strip()
        if not line.lower().startswith('bind_') or '=' not in line:
            continue
        action, value = (s.strip() for s in line.split('=', 1))
        keys = [e.strip()[4:].strip().lower() for e in value.split(',') if e.strip().lower().startswith('key:')]
        keys = [k for k in keys if k != 'default']
        if keys and action[5:].upper() in binds:
            binds[action[5:].upper()] = keys[0]
    return binds


def font_advances(blob):
    """Code point -> advance of every glyph of the button font (a .bfotf)."""
    font = TTFont(io.BytesIO(bfotf_decode(blob)[0]))
    return {cp: font['hmtx'][name][0] for cp, name in font.getBestCmap().items()}


def pc_letter(key):
    """The one character the panes around the action prompt show for key."""
    if key in PC_LETTER_LABELS:
        return PC_LETTER_LABELS[key]
    return PC_LABELS.get(key, key.upper())[0]


def pc_icons(g, adv, binds=PC_BINDS):
    """Code point -> (advance, draw(pen)) for every button glyph but Home and Capture, drawn as the key or mouse button
    binds puts on it. g: Glyphs for the key labels; adv: the game's advance of each glyph (font_advances), which each
    drawing keeps."""
    def mouse_body(pen, cx, y0, y1, lit=None):
        """A mouse seen from above, two thirds as wide as it is long, in one even outline: the buttons the top two
        fifths, split by a seam. lit 'l' or 'r': that button solid from the outline to the middle line, so the edge
        between the pressed and the empty button is the mouse's middle, and no wheel; 'm' or None: the wheel between
        the buttons, larger for 'm'."""
        h = y1 - y0; w = h * 0.66; t = h * 0.075
        polygon(pen, capsule(cx, y0, y1, w))
        polygon(ReverseContourPen(pen), capsule(cx, y0 + t, y1 - t, w - 2 * t))
        ys = y1 - h * 0.43   # the buttons' lower edge
        bar(pen, cx - w / 2 + t / 2, ys - t / 2, cx + w / 2 - t / 2, ys + t / 2)
        if lit in ('l', 'r'):   # no seam over the solid button: it would carry the white past the middle
            part = clip_half(capsule(cx, y0 + t, y1 - t, w - 2 * t), lambda p: p[1] - ys)
            part = clip_half(part, (lambda p: cx - p[0]) if lit == 'l' else (lambda p: p[0] - cx))
            polygon(pen, counter_clockwise(part))
            return
        wy0, wy1, ww, gap = ys + h * 0.07, y1 - h * 0.13, w * (0.2 if lit == 'm' else 0.15), t * 0.75
        if lit != 'm':   # the seam above and below the wheel
            bar(pen, cx - t / 2, ys, cx + t / 2, wy0 - gap)
            bar(pen, cx - t / 2, wy1 + gap, cx + t / 2, y1 - t / 2)
        polygon(pen, capsule(cx, wy0, wy1, ww, 16))

    def key_cap(pen, x0, y0, x1, y1, key):
        """A key: a rounded square with its label cut out; a mouse button draws the mouse."""
        if key in PC_MOUSE:
            mouse_body(pen, (x0 + x1) / 2, y0, y1, key[6:])
            return
        w, h = x1 - x0, y1 - y0
        rounded(pen, x0, y0, x1, y1, min(w, h) * 0.18)
        if key == 'space':   # an open bracket lying on its back
            t = h * 0.09
            bx0, bx1, by0, by1 = x0 + w * 0.22, x1 - w * 0.22, y0 + h * 0.36, y0 + h * 0.60
            polygon(ReverseContourPen(pen), [(bx0, by0), (bx1, by0), (bx1, by1), (bx1 - t, by1), (bx1 - t, by0 + t),
                                             (bx0 + t, by0 + t), (bx0 + t, by1), (bx0, by1)])
            return
        label = PC_LABELS.get(key, key.upper())
        cap = h * (0.52 if len(label) == 1 else 0.36 if len(label) == 2 else 0.26)
        g.hole(pen, label, (x0 + x1) / 2, (y0 + y1) / 2, cap, w * 0.74, 'H')

    def one(key, advance):
        def draw(pen):
            side = min(advance - 80, Y1 - Y0)
            key_cap(pen, (advance - side) / 2, Y0, (advance + side) / 2, Y1, key)
        return advance, draw

    def grid(cells, advance):
        """Small keys in a grid: cells = [(key, column, row)], row 0 on top."""
        def draw(pen):
            cols, rows = max(c for _, c, _ in cells) + 1, max(r for _, _, r in cells) + 1
            gap = 24; size = min((advance - 60 - gap * (cols - 1)) / cols, (Y1 - Y0 - gap * (rows - 1)) / rows)
            left, top = (advance - cols * size - gap * (cols - 1)) / 2, CY + (rows * size + gap * (rows - 1)) / 2
            for key, c, r in cells:
                x, y = left + c * (size + gap), top - (r + 1) * size - r * gap
                key_cap(pen, x, y, x + size, y + size, key)
        return advance, draw

    def mouse_move(advance, *dirs):
        """The mouse, no button pressed, and the directions the prompt means: none named (the camera), an arrow on each
        side of it; one, a bold arrow at its lower right as the game's sticks have it; up and down or left and right,
        one arrow with a head at both ends."""
        def draw(pen):
            if not dirs:
                mh = (Y1 - Y0) * 0.62; mw, s, d = mh * 0.66, 135, 14
                cx, my0 = advance / 2, (Y0 + Y1) / 2 - mh / 2
                mouse_body(pen, cx, my0, my0 + mh)
                bold_arrow(pen, cx, my0 + mh + d + s / 2 - 40, 'up', s)
                bold_arrow(pen, cx, my0 - d - s / 2 + 40, 'down', s)
                bold_arrow(pen, cx - mw / 2 - d - s / 2, (Y0 + Y1) / 2, 'left', s)
                bold_arrow(pen, cx + mw / 2 + d + s / 2, (Y0 + Y1) / 2, 'right', s)
                return
            mh = Y1 - Y0; mw = mh * 0.66
            cx = advance / 2 - mw * 0.42
            mouse_body(pen, cx, Y0, Y1)
            ax = cx + mw / 2 + 125
            if set(dirs) == {'up', 'down'}:
                bold_arrow(pen, ax + 22, Y0 + mh * 0.48, 'up', 300, double=True)
            elif set(dirs) == {'left', 'right'}:
                bold_arrow(pen, ax + 30, Y0 + mh * 0.3, 'right', 300, double=True)
            else:
                bold_arrow(pen, ax, Y0 + mh * 0.3, dirs[0], 230)
        return advance, draw

    b = binds
    up, left, down, right = b['LSTICK_UP'], b['LSTICK_LEFT'], b['LSTICK_DOWN'], b['LSTICK_RIGHT']
    du, dl, dd, dr = b['UP'], b['LEFT'], b['DOWN'], b['RIGHT']
    return {
        **{cp: one(b[name], adv[cp]) for cp, name in ((0xE040, 'A'), (0xE041, 'B'), (0xE042, 'X'), (0xE043, 'Y'),
                                                      (0xE044, 'L'), (0xE045, 'R'), (0xE046, 'ZL'), (0xE047, 'ZR'),
                                                      (0xE04B, 'PLUS'), (0xE04C, 'MINUS'), (0xE056, 'LCLICK'),
                                                      (0xE057, 'RCLICK'))},
        # the d-pad: its four keys as a cross (up on top), or the keys of the directions the prompt means
        0xE049: grid([(du, 1, 0), (dl, 0, 1), (dd, 1, 1), (dr, 2, 1)], adv[0xE049]),
        0xE060: one(du, adv[0xE060]), 0xE061: one(dd, adv[0xE061]), 0xE062: one(dl, adv[0xE062]),
        0xE063: one(dr, adv[0xE063]), 0xE064: grid([(du, 0, 0), (dd, 0, 1)], adv[0xE064]),
        0xE065: grid([(dl, 0, 0), (dr, 1, 0)], adv[0xE065]),
        **({0xE06A: grid([(dl, 0, 0), (dd, 1, 0), (dr, 2, 0)], adv[0xE06A])} if 0xE06A in adv else {}),   # OoT's font only
        # the left stick: its keys (W A S D)
        0xE05E: grid([(up, 1, 0), (left, 0, 1), (down, 1, 1), (right, 2, 1)], adv[0xE05E]),
        0xE052: one(up, adv[0xE052]), 0xE050: one(down, adv[0xE050]), 0xE05A: one(left, adv[0xE05A]),
        0xE058: one(right, adv[0xE058]), 0xE05C: grid([(up, 0, 0), (down, 0, 1)], adv[0xE05C]),
        0xE054: grid([(left, 0, 0), (right, 1, 0)], adv[0xE054]),
        # the right stick: the mouse
        0xE05F: mouse_move(adv[0xE05F]), 0xE053: mouse_move(adv[0xE053], 'up'),
        0xE051: mouse_move(adv[0xE051], 'down'), 0xE05B: mouse_move(adv[0xE05B], 'left'),
        0xE059: mouse_move(adv[0xE059], 'right'), 0xE05D: mouse_move(adv[0xE05D], 'up', 'down'),
        0xE055: mouse_move(adv[0xE055], 'left', 'right'),
        # the right stick shaken: the shake key
        0xE068: one(b['SHAKE'], adv[0xE068]),
    }


def redraw_font(otf, icons, swaps=()):
    """The OpenType font otf with the glyphs of each (code point, code point) pair of swaps trading places (outlines and
    horizontal metrics), then every code point of icons (code point -> (advance, draw)) drawn anew, written again by
    fontTools; a code point the font does not have is left out (U+E06A, the d-pad's left, down and right, is OoT's
    alone). The timestamps stay, so the same input gives the same bytes."""
    font = TTFont(io.BytesIO(otf), recalcTimestamp=False)
    assert font['head'].unitsPerEm == 1000, f'em of {font["head"].unitsPerEm} units, the drawings assume 1000'
    cff = font['CFF '].cff
    top = cff.topDictIndex[0]
    assert not hasattr(top, 'ROS'), 'CID-keyed CFF'   # its glyphs can use different private dictionaries
    charstrings, private, hmtx, cmap = top.CharStrings, top.Private, font['hmtx'], font.getBestCmap()
    nominal = getattr(private, 'nominalWidthX', 0)
    for a, b in swaps:
        ga, gb = cmap[a], cmap[b]
        outline_a, outline_b = charstrings[ga], charstrings[gb]
        charstrings[ga], charstrings[gb] = outline_b, outline_a
        hmtx[ga], hmtx[gb] = hmtx[gb], hmtx[ga]
    icons = {cp: icon for cp, icon in icons.items() if cp in cmap}
    for cp, (advance, draw) in icons.items():
        pen = T2CharStringPen(advance - nominal, None); draw(pen)   # the width operand is relative to nominalWidthX
        charstrings[cmap[cp]] = pen.getCharString(private=private, globalSubrs=cff.GlobalSubrs)
    glyphs = font.getGlyphSet()
    for cp, (advance, _) in icons.items():
        box = BoundsPen(glyphs); glyphs[cmap[cp]].draw(box)
        hmtx[cmap[cp]] = (advance, int(round(box.bounds[0])))
    out = io.BytesIO(); font.save(out)
    return out.getvalue()


def flat_cmap(mapping, extra=()):
    """A cmap table whose Unicode subtable (format 4, shared by platforms 0 and 3) gives every code point a segment of
    its own: glyph = code point + idDelta, so one code point's glyph changes by writing its 16-bit delta and nothing
    else moves. mapping: code point -> glyph id; extra: (platform, encoding, subtable bytes) kept as they are (the
    font's Macintosh subtable). Returns (table bytes, {code point: offset of its delta in the table})."""
    cps = sorted(mapping)
    assert cps and cps[-1] < 0xFFFF
    n = len(cps) + 1   # the last segment ends the table: 0xFFFF, delta 1
    power = 1 << (n.bit_length() - 1)
    words = lambda values: b''.join(struct.pack('>H', v & 0xFFFF) for v in values)
    sub = (struct.pack('>7H', 4, 16 + 8 * n, 0, 2 * n, 2 * power, n.bit_length() - 1, 2 * n - 2 * power)
           + words(cps + [0xFFFF]) + bytes(2) + words(cps + [0xFFFF]) + words([mapping[cp] - cp for cp in cps] + [1])
           + bytes(2 * n))
    records = sorted([(0, 3, None), (3, 1, None)] + [(p, e, data) for p, e, data in extra])
    at = 4 + 8 * len(records)
    head, body, shared = struct.pack('>HH', 0, len(records)), sub, at
    for p, e, data in records:
        if data is None:
            head += struct.pack('>HHI', p, e, shared)
        else:
            head += struct.pack('>HHI', p, e, at + len(body))
            body += data
    deltas = shared + 14 + 2 * n + 2 + 2 * n
    return head + body, {cp: deltas + 2 * i for i, cp in enumerate(cps)}


def fonts_with_sets(otf, sets, keys=None):
    """One font per prompt set, each holding the button glyphs of every set, for switching the prompts while the game runs
    (runtime/romfs.c romfs_prompts_live). sets: name -> (icons, swaps) as redraw_font takes them. Every set gets a block
    of new glyphs, one per button code point (the union of what any set draws or swaps), appended in the same order; a
    set's font differs from the others only in its cmap, which sends those code points to its block. Every cmap is
    flat_cmap's, so the tables have one length, sit at one offset, and the game's copy of one can be replaced by
    another in place, or one code point's glyph changed alone. keys: key name -> (advance, draw), a glyph for each
    key of the keyboard prompts, in every font, which the runtime puts on a button when its binding changes.
    Returns ({name: font bytes}, {name: cmap table bytes}, {'deltas': {code point: offset in the table},
    'keys': {key name: glyph id}})."""
    font = TTFont(io.BytesIO(otf), recalcTimestamp=False)
    assert font['head'].unitsPerEm == 1000, f'em of {font["head"].unitsPerEm} units, the drawings assume 1000'
    cff = font['CFF '].cff
    top = cff.topDictIndex[0]
    assert not hasattr(top, 'ROS'), 'CID-keyed CFF'
    charstrings, private, hmtx, cmap = top.CharStrings, top.Private, font['hmtx'], dict(font.getBestCmap())
    nominal = getattr(private, 'nominalWidthX', 0)
    glyphs = font.getGlyphSet()
    cps = sorted({cp for icons, swaps in sets.values() for cp in icons} | {cp for _, swaps in sets.values() for pair in swaps for cp in pair})
    assert all(cp in cmap for cp in cps), 'a button code point the font does not have'
    order = font.getGlyphOrder()

    def add(gname):   # a CFF font's glyph order is its charset list (fontTools): one append, else both lists
        order.append(gname)
        if top.charset is not order:
            top.charset.append(gname)
    block = {}
    for name, (icons, swaps) in sets.items():
        partner = {a: b for a, b in swaps} | {b: a for a, b in swaps}
        for cp in cps:
            if cp in icons:
                advance, draw = icons[cp]
            else:   # the game's glyph, or its swap partner's
                src = cmap[partner.get(cp, cp)]
                advance, draw = hmtx[src][0], glyphs[src].draw
            pen = T2CharStringPen(advance - nominal, None)
            draw(pen)
            box = BoundsPen(None)
            rec = RecordingPen(); draw(rec); rec.replay(box)
            gname = f'rk.{name}.{cp:04X}'
            charstrings.charStringsIndex.append(pen.getCharString(private=private, globalSubrs=cff.GlobalSubrs))
            charstrings.charStrings[gname] = len(charstrings.charStringsIndex) - 1
            add(gname)
            hmtx[gname] = (advance, int(round(box.bounds[0])) if box.bounds else 0)
            block[name, cp] = gname
    key_glyph = {}
    for key, (advance, draw) in (keys or {}).items():
        pen = T2CharStringPen(advance - nominal, None)
        draw(pen)
        box = BoundsPen(None)
        rec = RecordingPen(); draw(rec); rec.replay(box)
        gname = f'rk.key.{key}'
        charstrings.charStringsIndex.append(pen.getCharString(private=private, globalSubrs=cff.GlobalSubrs))
        charstrings.charStrings[gname] = len(charstrings.charStringsIndex) - 1
        add(gname)
        hmtx[gname] = (advance, int(round(box.bounds[0])) if box.bounds else 0)
        key_glyph[key] = gname
    font.setGlyphOrder(order)
    # the subtables that are not Unicode stay as the font has them (they do not hold the buttons)
    extra = [(t.platformID, t.platEncID, t.compile(font)) for t in font['cmap'].tables if not t.isUnicode()]
    fonts, tables, deltas = {}, {}, {}
    for name in sets:
        mapping = {cp: font.getGlyphID(block[name, cp] if cp in cps else g) for cp, g in cmap.items()}
        raw, deltas = flat_cmap(mapping, extra)
        table = DefaultTable('cmap'); table.data = raw
        font['cmap'] = table
        out = io.BytesIO(); font.save(out)
        fonts[name] = out.getvalue()
        tables[name] = TTFont(io.BytesIO(fonts[name])).reader['cmap']
        assert tables[name] == raw
        again = TTFont(io.BytesIO(fonts[name]))   # the table as a font reader understands it
        assert {cp: again.getGlyphID(g) for cp, g in again.getBestCmap().items()} == mapping, f'{name}: cmap reads back wrong'
    sizes = {len(t) for t in tables.values()}
    assert len(sizes) == 1, f'cmap tables of different lengths: {sizes}'
    first = next(iter(fonts.values()))
    at = first.find(next(iter(tables.values())))
    assert all(f[:at] + tables[n] + f[at + len(tables[n]):] == f for n, f in fonts.items()), 'cmap is not at one offset'
    return fonts, tables, {'deltas': deltas, 'keys': {key: font.getGlyphID(g) for key, g in key_glyph.items()}}


# ---- the keyboard prompts while the game runs: a glyph for every key the runtime can bind (runtime/input_map.c
# key_names, the names rekarina.ini takes), and the buttons whose prompt is the one key bound to them.
PC_KEYS = (list('abcdefghijklmnopqrstuvwxyz0123456789')
           + ['space', 'tab', 'lshift', 'lctrl', 'up', 'down', 'left', 'right', 'mouse_l', 'mouse_r', 'mouse_m',
              'mouse_x1', 'mouse_x2', 'enter', 'backspace', 'rshift', 'rctrl', 'lalt', 'capslock', 'insert', 'delete',
              'home', 'end', 'pageup', 'pagedown', 'f2', 'f3', 'f4', 'f5', 'f6', 'f7', 'f9', 'f12']
           + [f'num{i}' for i in range(10)]
           + ['num_add', 'num_sub', 'num_mul', 'num_div', 'num_decimal', 'minus', 'equals', 'lbracket', 'rbracket',
              'semicolon', 'apostrophe', 'comma', 'period', 'slash', 'backslash', 'grave'])
# button code point -> the action (a bind_<action> line of rekarina.ini) whose first key it shows
PC_KEY_BUTTONS = {0xE040: 'A', 0xE041: 'B', 0xE042: 'X', 0xE043: 'Y', 0xE044: 'L', 0xE045: 'R', 0xE046: 'ZL',
                  0xE047: 'ZR', 0xE04B: 'PLUS', 0xE04C: 'MINUS', 0xE056: 'LCLICK', 0xE057: 'RCLICK',
                  0xE060: 'UP', 0xE061: 'DOWN', 0xE062: 'LEFT', 0xE063: 'RIGHT', 0xE052: 'LSTICK_UP',
                  0xE050: 'LSTICK_DOWN', 0xE05A: 'LSTICK_LEFT', 0xE058: 'LSTICK_RIGHT', 0xE068: 'SHAKE'}
# a label's character the game's text fonts may lack -> letters every Latin font has
PC_LABEL_FALLBACK = {'⇧': 'SHIFT', '↑': 'UP', '↓': 'DN', '←': 'LT', '→': 'RT', '▬': 'SPC'}


def pc_labels_fit(g):
    """Rewrites PC_LABELS so every label can be drawn with the text fonts of g (Glyphs): a character none of them
    has becomes its fallback word. The icons read PC_LABELS when they are drawn, so this comes before any drawing."""
    def have(text):
        try:
            for ch in text:
                g.outline(ch)
            return True
        except KeyError:
            return False
    for key in PC_KEYS:
        label = PC_LABELS.get(key, key.upper())
        if not have(label):
            fixed = ''.join(PC_LABEL_FALLBACK.get(ch, ch) for ch in label)
            PC_LABELS[key] = fixed if have(fixed) else '?'


def pc_key_icons(g, adv, advance):
    """key name -> (advance, draw) for every key of PC_KEYS: the key's cap as pc_icons draws it on a face button, at one
    advance (a face button's), so a prompt keeps about its width whichever key is bound."""
    flat = dict(adv)
    flat[0xE040] = advance
    return {key: pc_icons(g, flat, dict(PC_BINDS, A=key))[0xE040] for key in PC_KEYS}


# ---- the button guide's images: square R8_UNORM masks (white key, black letter), one mip, block-linear, 72x72 up to
# 1.2.1 and 108x108 in 1.4.3. Each is drawn from the icon of the same Switch button, fitted into the square.
R8_UNORM, BC4_UNORM, HUD_MARGIN, SUPERSAMPLE = 0x201, 0x1d01, 4, 4


def gob_offset(x, y, width_gobs, block_height, ns2=False):
    """Byte offset of (x, y) in a Tegra block-linear surface (GOBs of 64 bytes x 8 rows): x in bytes, y in rows (of
    pixels, or of 4x4 blocks in a block-compressed format). ns2: the Switch 2's GOB, whose 16-byte pieces go x5 y2 x4 y1 y0
    where the Tegra X1's go x5 y2 y1 x4 y0: offset bits 5 and 6 exchanged (runtime/tex_deswizzle.c gob_off)."""
    o = ((y // (8 * block_height)) * 512 * block_height * width_gobs + (x // 64) * 512 * block_height
         + ((y % (8 * block_height)) // 8) * 512 + ((x % 64) // 32) * 256 + ((y % 8) // 2) * 64
         + ((x % 32) // 16) * 32 + (y % 2) * 16 + (x % 16))
    return (o & ~0x60) | ((o & 0x20) << 1) | ((o & 0x40) >> 1) if ns2 else o


class PolygonPen(BasePen):
    """Outlines flattened to closed polygons."""
    def __init__(self):
        super().__init__(None); self.polygons = []

    def _moveTo(self, pt):
        self.polygons.append([pt])

    def _lineTo(self, pt):
        self.polygons[-1].append(pt)

    def _curveToOne(self, pt1, pt2, pt3):
        p0 = self.polygons[-1][-1]
        for i in range(1, 17):
            t = i / 16; u = 1 - t
            self.polygons[-1].append(tuple(u ** 3 * p0[k] + 3 * u * u * t * pt1[k] + 3 * u * t * t * pt2[k] + t ** 3 * pt3[k]
                                           for k in (0, 1)))

    def _qCurveToOne(self, pt1, pt2):
        p0 = self.polygons[-1][-1]
        for i in range(1, 17):
            t = i / 16; u = 1 - t
            self.polygons[-1].append(tuple(u * u * p0[k] + 2 * u * t * pt1[k] + t * t * pt2[k] for k in (0, 1)))


def fill_mask(polygons, width, height):
    """A width x height coverage mask (0..255, rows top down) of closed polygons in pixel coordinates, nonzero winding."""
    edges = [(a, b) for poly in polygons for a, b in zip(poly, poly[1:] + poly[:1]) if a[1] != b[1]]
    cover = [0] * (width * height)
    for row in range(height * SUPERSAMPLE):
        y = (row + 0.5) / SUPERSAMPLE
        crossings = sorted((a[0] + (y - a[1]) * (b[0] - a[0]) / (b[1] - a[1]), 1 if b[1] > a[1] else -1)
                           for a, b in edges if min(a[1], b[1]) <= y < max(a[1], b[1]))
        winding, start = 0, None
        for x, d in crossings:
            if winding == 0:
                start = x
            winding += d
            if winding == 0:
                for col in range(max(0, round(start * SUPERSAMPLE)), min(width * SUPERSAMPLE, round(x * SUPERSAMPLE))):
                    cover[(row // SUPERSAMPLE) * width + col // SUPERSAMPLE] += 1
    return bytes(min(255, c * 255 // (SUPERSAMPLE * SUPERSAMPLE)) for c in cover)


def rasterize(draw, size, fit=None):
    """A size x size coverage mask (0..255, rows top down) of draw's outline, nonzero winding, centred and scaled so its
    larger side spans fit pixels (default: the square less HUD_MARGIN on each side), or, with a (width, height) pair,
    so it is as large as fits both."""
    rec = RecordingPen(); draw(rec)
    box = BoundsPen(None); rec.replay(box); x0, y0, x1, y1 = box.bounds
    inner = fit or size - 2 * HUD_MARGIN
    if isinstance(inner, tuple):   # (width, height): the outline as large as fits both
        scale = min(inner[0] / (x1 - x0), inner[1] / (y1 - y0))
    else:
        scale = inner / max(x1 - x0, y1 - y0)
    ox = (size - (x1 - x0) * scale) / 2 - x0 * scale
    oy = (size - (y1 - y0) * scale) / 2 - y0 * scale
    flat = PolygonPen(); rec.replay(TransformPen(flat, (scale, 0, 0, -scale, ox, size - oy)))   # font y up -> rows down
    return fill_mask(flat.polygons, size, size)


# The buttons a prompt set is shown by in the settings menu, in the order drawn: A, B, X, Y, L, R, ZL, ZR, Minus, Plus,
# the d-pad.
PREVIEW_CODES = (0xE040, 0xE041, 0xE042, 0xE043, 0xE044, 0xE045, 0xE046, 0xE047, 0xE04C, 0xE04B, 0xE049)


def preview_strip(otf, cell, codes=PREVIEW_CODES, names=None, extent=None):
    """(count, masks) for the settings menu's picture of a prompt set (runtime/ui/menu.cpp): one cell x cell coverage
    mask per button glyph of the OpenType font otf, in the order of codes, each glyph centred in its cell and all of
    them at one scale, so a shoulder button stays wider than a face button as in the game's text. A code point the font
    lacks gives an empty cell. names: glyph names to draw instead of codes (the keys of the keyboard prompts);
    extent: the font units the cell holds (default: the largest glyph drawn)."""
    font = TTFont(io.BytesIO(otf))
    cmap, glyphs = font.getBestCmap(), font.getGlyphSet()
    wanted = list(names) if names is not None else [cmap.get(cp) for cp in codes]
    drawn = []
    for gname in wanted:
        if gname is None or gname not in glyphs:
            drawn.append(None)
            continue
        rec = RecordingPen(); glyphs[gname].draw(rec)
        box = BoundsPen(glyphs); rec.replay(box)
        drawn.append((rec, box.bounds) if box.bounds else None)
    extent = extent or max((max(b[2] - b[0], b[3] - b[1]) for _, b in filter(None, drawn)), default=1000)
    margin = 2
    scale = (cell - 2 * margin) / extent
    masks = b''
    for item in drawn:
        if not item:
            masks += bytes(cell * cell)
            continue
        rec, (x0, y0, x1, y1) = item
        ox = (cell - (x1 - x0) * scale) / 2 - x0 * scale
        oy = (cell - (y1 - y0) * scale) / 2 - y0 * scale
        flat = PolygonPen(); rec.replay(TransformPen(flat, (scale, 0, 0, -scale, ox, cell - oy)))
        masks += fill_mask(flat.polygons, cell, cell)
    return len(wanted), masks


def bntx_images(data, base):
    """Name (without the ^ suffix) -> (shape, start, size, format, mips, width, height, block height) of every texture
    of the BNTX at data[base:]. shape: its header from flags to layout and its image size, equal for two textures whose
    image bytes can stand for each other. The platform word is "NX  " in a Switch file, "Ounc" in a Switch 2 one."""
    assert data[base:base + 4] == b'BNTX' and data[base + 0x20:base + 0x24] in (b'NX  ', b'Ounc')
    count, table = struct.unpack_from('<IQ', data, base + 0x24)
    out = {}
    for k in range(count):
        info = base + struct.unpack_from('<Q', data, base + table + 8 * k)[0]
        assert data[info:info + 4] == b'BRTI'
        at = base + struct.unpack_from('<Q', data, info + 0x60)[0]
        name = data[at + 2:at + 2 + struct.unpack_from('<H', data, at)[0]].decode().split('^')[0]
        mips, fmt = struct.unpack_from('<H', data, info + 0x16)[0], struct.unpack_from('<I', data, info + 0x1c)[0]
        (w, h), layout = struct.unpack_from('<II', data, info + 0x24), struct.unpack_from('<I', data, info + 0x34)[0]
        start = base + struct.unpack_from('<Q', data, base + struct.unpack_from('<Q', data, info + 0x70)[0])[0]
        size = struct.unpack_from('<I', data, info + 0x50)[0]
        shape = bytes(data[info + 0x10:info + 0x3c]) + bytes(data[info + 0x50:info + 0x54])
        out[name] = (shape, start, size, fmt, mips, w, h, 1 << (layout & 7))
    return out


def bc4_block(px):
    """The 8 bytes of a BC4 (one-channel) block of 16 values, rows top down: the largest and the smallest value as the
    two ends, so the six steps between them are used, each value its nearest step."""
    hi, lo = max(px), min(px)
    if hi == lo:   # one value: the six-step mode with index 0 everywhere
        return bytes((hi, lo)) + bytes(6)
    steps = [hi, lo] + [((7 - i) * hi + i * lo + 3) // 7 for i in range(1, 7)]
    bits = 0
    for k, v in enumerate(px):
        bits |= min(range(8), key=lambda j: abs(steps[j] - v)) << (3 * k)
    return bytes((hi, lo)) + bits.to_bytes(6, 'little')


def bntx_redraw(data, base, textures, icons, ns2=False, fit=None):
    """Paint the icon of each texture of textures (name without the ^ suffix -> code point; icons: code point ->
    (advance, draw)) over its image in the BNTX at data[base:] (a bytearray), in place, the size unchanged. R8 or BC4
    images of one mip; ns2: the Switch 2's block-linear layout (gob_offset); fit: the icon's larger side in pixels
    (rasterize). Returns the names painted."""
    images, done = bntx_images(data, base), []
    for name, code in textures.items():
        assert name in images, f'not in the texture list: {name}'
        _, start, size, fmt, mips, w, h, block = images[name]
        assert fmt in (R8_UNORM, BC4_UNORM) and w == h and mips == 1, f'{name}: format {fmt:#x}, {w}x{h}, {mips} mips'
        mask = rasterize(icons[code][1], w, fit)
        if fmt == R8_UNORM:
            for y in range(h):
                for x in range(w):
                    at = gob_offset(x, y, (w + 63) // 64, block, ns2)
                    assert at < size, f'{name}: pixel ({x}, {y}) past the image'
                    data[start + at] = mask[y * w + x]
        else:   # 4x4 blocks of 8 bytes, laid out as the pixels of a surface a quarter as wide and high
            bw = (w + 3) // 4
            for by in range((h + 3) // 4):
                for bx in range(bw):
                    px = [mask[min(by * 4 + j // 4, h - 1) * w + min(bx * 4 + j % 4, w - 1)] for j in range(16)]
                    at = gob_offset(bx * 8, by, (bw * 8 + 63) // 64, block, ns2)
                    assert at + 8 <= size, f'{name}: block ({bx}, {by}) past the image'
                    data[start + at:start + at + 8] = bc4_block(px)
        done.append(name)
    return done


def bntx_swap(data, base, pairs):
    """Swap the image bytes of each (name, name) pair of textures in the BNTX at data[base:] (a bytearray), in place.
    Both of a pair must have the same shape (bntx_images), so the bytes of one are a valid image of the other."""
    images = bntx_images(data, base)
    for a, b in pairs:
        assert a in images and b in images, f'{a} or {b} missing from the texture list'
        (shape_a, at_a, size, *_), (shape_b, at_b, *_) = images[a], images[b]
        assert shape_a == shape_b, f'{a} and {b} differ in format, size or tiling'
        data[at_a:at_a + size], data[at_b:at_b + size] = data[at_b:at_b + size], data[at_a:at_a + size]


# ---- images whose letter lies on an opaque shape of the image itself (OoT's song screen: a white disc or key with a
# black letter, ASTC 4x4, the shape in the alpha channel). The letter's surroundings are replaced, the shape's edge
# stays as the game has it: every 4x4 block of a box inside the opaque part is written again, white with the new
# drawing in black. Such a block needs no alpha and one pair of grey ends, the simplest block ASTC has.
ASTC_4X4_SRGB = 0x2d06
ASTC_WEIGHTS = (0, 9, 18, 27, 37, 46, 55, 64)   # a 3-bit weight as a decoder widens it to 0..64


def astc_grey_block(px):
    """The 16 bytes of an opaque grey ASTC 4x4 block of 16 values, rows top down: block mode 0x53 (a 4x4 grid of 3-bit
    weights, one plane), one partition, endpoint mode 0 (two luminance values of 8 bits, the smallest and the largest
    value), each texel the nearest of the eight steps between them. The weights fill the block from its last bit
    down, the first texel's lowest bit in bit 127. The steps are computed as an sRGB decode does (the ends widened to
    16 bits with 0x80 below)."""
    lo, hi = min(px), max(px)
    v = 0x53 | lo << 17 | hi << 25
    if hi > lo:
        steps = [((lo << 8 | 0x80) * (64 - w) + (hi << 8 | 0x80) * w + 32 >> 6) >> 8 for w in ASTC_WEIGHTS]
        for i, p in enumerate(px):
            k = min(range(8), key=lambda j: abs(steps[j] - p))
            for b in range(3):
                v |= (k >> b & 1) << (127 - (3 * i + b))
    return v.to_bytes(16, 'little')


def bntx_redraw_box(data, base, textures, icons, box, fit, ns2=True):
    """Paint the icon of each texture of textures (name -> code point; icons: code point -> (advance, draw)) in black
    on white over the box (x0, y0, width, height; multiples of 4) of its image in the BNTX at data[base:] (a
    bytearray), in place: ASTC 4x4 images of one mip whose box is opaque in the game's file. The icon is centred in the
    box, as large as fits fit (rasterize). Returns the names painted."""
    images, done = bntx_images(data, base), []
    x0, y0, bw_px, bh_px = box
    assert x0 % 4 == 0 and y0 % 4 == 0 and bw_px % 4 == 0 and bh_px % 4 == 0, box
    side = max(bw_px, bh_px)   # the mask is square, centred on the box: the box is its middle
    ox, oy = (side - bw_px) // 2, (side - bh_px) // 2
    for name, code in textures.items():
        assert name in images, f'not in the texture list: {name}'
        _, start, size, fmt, mips, w, h, block = images[name]
        assert fmt == ASTC_4X4_SRGB and mips == 1 and x0 + bw_px <= w and y0 + bh_px <= h, \
            f'{name}: format {fmt:#x}, {w}x{h}, {mips} mips'
        mask = rasterize(icons[code][1], side, fit)
        bw = (w + 3) // 4
        for by in range(bh_px // 4):
            for bx in range(bw_px // 4):
                px = [255 - mask[(oy + by * 4 + j // 4) * side + ox + bx * 4 + j % 4] for j in range(16)]
                at = gob_offset((x0 // 4 + bx) * 16, y0 // 4 + by, (bw * 16 + 63) // 64, block, ns2)
                assert at + 16 <= size, f'{name}: block ({bx}, {by}) past the image'
                data[start + at:start + at + 16] = astc_grey_block(px)
        done.append(name)
    return done


def label_icons(g, labels):
    """Code point -> (advance, draw(pen)) of plain text in the game's own text font (g: Glyphs), for an image that
    carries its own key shape: labels maps code point -> text."""
    def text(label):
        def draw(pen):
            g.fill(pen, label, 0, 0, 700, 1e9)
        return 0, draw
    return {code: text(label) for code, label in labels.items()}


# ---- the resource size table: System/Resource/ResourceSizeTable.Product.<n>.Nin_NX_NVN.rsizetable.zs, a RESTBL whose
# entries are keyed by the CRC32 of the RomFS path without .zs. The game sizes the buffer it loads a file into from its
# entry, so an archive that grew needs its entry raised by as much.
def restbl_offsets(table):
    """CRC32 of a path -> offset of its size in the RESTBL table."""
    assert table[:6] == b'RESTBL', 'not a RESTBL'
    count = struct.unpack_from('<I', table, 14)[0]
    return {struct.unpack_from('<I', table, 22 + i * 8)[0]: 22 + i * 8 + 4 for i in range(count)}


def grow_restbl(table, growth):
    """table with the entry of each RomFS path of growth (path -> bytes it grew) raised by that much."""
    at, out = restbl_offsets(table), bytearray(table)
    for path, extra in growth.items():
        h = zlib.crc32(path.removesuffix('.zs').encode())
        assert h in at, f'{path} has no entry in the resource size table'
        struct.pack_into('<I', out, at[h], struct.unpack_from('<I', out, at[h])[0] + extra)
    return bytes(out)
