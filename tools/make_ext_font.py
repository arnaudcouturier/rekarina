#!/usr/bin/env python3
# Adapted from seedds/nxnative dm/tools/make_ext_font.py @ ee9b776 (MIT, Copyright (c) 2026 seedds): Rekarina adds the
# Xbox styles (--style xbox-label: the letters as labelled, LB/RB/LT/RT plates, Menu and View for + and -; --style
# xbox: the letters of the Xbox button in each Nintendo button's place, for a pad mapped by position) and writes the
# BFTTF wrapping nn::pl serves (--bfttf), with Windows' Arial Bold as the default letter font.
# Generate a stand-in for the "Nintendo extension" shared font (nintendo_ext.ttf): controller-button icons at the original private-use code points (A/B/X/Y, L/R/ZL/ZR/SL/SR, +/−, D-pad, stick direction arrows).
# The host has no original system fonts; button icons in game text (e.g. Mario's GameMsg/Key.msbt: U+E0E0 = A button) render as "?" when the glyph is missing. Letters come from macOS's Arial Rounded Bold, knocked out of a solid plate.
# Usage: make_ext_font.py out.ttf [letter-font.ttf]; needs fontTools (pip install fonttools)
import math, sys
from fontTools.ttLib import TTFont
from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.pens.transformPen import TransformPen
from fontTools.pens.reverseContourPen import ReverseContourPen
from fontTools.pens.boundsPen import BoundsPen
from fontTools.pens.recordingPen import RecordingPen

EM, ASC, DESC = 1000, 880, -120
CY, R = 350, 450          # icon centre height and circle radius: -100 to 800, sitting on the baseline

def area(c): return sum(c[i][0] * c[i - 1][1] - c[i - 1][0] * c[i][1] for i in range(len(c))) / 2
def contour(pen, pts, hole=False):
    # solid clockwise, hole counter-clockwise (TrueType convention, nonzero winding)
    if (area(pts) > 0) != hole: pts = pts[::-1]
    k = next(i for i, p in enumerate(pts) if p[2]); pts = pts[k:] + pts[:k]
    pen.moveTo(pts[0][:2]); offs = []
    for x, y, on in pts[1:]:
        if not on: offs.append((x, y)); continue
        if offs: pen.qCurveTo(*offs, (x, y)); offs = []
        else: pen.lineTo((x, y))
    if offs: pen.qCurveTo(*offs, pts[0][:2])
    pen.closePath()
def circle(cx, cy, r, n=8):
    pts = []
    for i in range(n):
        a = 2 * math.pi * i / n; b = a + math.pi / n; rr = r / math.cos(math.pi / n)
        pts += [(round(cx + r * math.cos(a)), round(cy + r * math.sin(a)), 1), (round(cx + rr * math.cos(b)), round(cy + rr * math.sin(b)), 0)]
    return pts
def rrect(x0, y0, x1, y1, rad):
    rad = min(rad, (x1 - x0) / 2, (y1 - y0) / 2)
    return [(x0 + rad, y1, 1), (x1 - rad, y1, 1), (x1, y1, 0), (x1, y1 - rad, 1), (x1, y0 + rad, 1), (x1, y0, 0), (x1 - rad, y0, 1),
            (x0 + rad, y0, 1), (x0, y0, 0), (x0, y0 + rad, 1), (x0, y1 - rad, 1), (x0, y1, 0)]
def rect(x0, y0, x1, y1): return [(x0, y1, 1), (x1, y1, 1), (x1, y0, 1), (x0, y0, 1)]
def rot(pts, cx, cy, deg):
    a = math.radians(deg); c, s = math.cos(a), math.sin(a)
    return [(round(cx + (x - cx) * c - (y - cy) * s), round(cy + (x - cx) * s + (y - cy) * c), on) for x, y, on in pts]

class Letters:
    def __init__(self, path):
        self.f = TTFont(path); self.gs = self.f.getGlyphSet(); self.cmap = self.f.getBestCmap()
    def knock(self, pen, text, cx, cy, h):
        # knock text out of the plate, centred at (cx, cy) with height h
        names = [self.cmap[ord(ch)] for ch in text]; x = 0; parts = []
        for nm in names: parts.append((nm, x)); x += self.gs[nm].width
        bp = BoundsPen(self.gs)
        for nm, dx in parts: self.gs[nm].draw(TransformPen(bp, (1, 0, 0, 1, dx, 0)))
        x0, y0, x1, y1 = bp.bounds; s = h / (y1 - y0)
        ox, oy = cx - (x0 + x1) / 2 * s, cy - (y0 + y1) / 2 * s
        # Knocked out = opposite direction to the plate. Fonts differ in outer-contour direction (TrueType clockwise, converted fonts often counter-clockwise); flip based on the largest contour's direction
        rec = RecordingPen()
        for nm, dx in parts: self.gs[nm].draw(TransformPen(rec, (s, 0, 0, s, ox + dx * s, oy)))
        rings, cur = [], []
        for op, args in rec.value:
            if op == 'moveTo': cur = [args[0]]; rings.append(cur)
            elif op in ('lineTo', 'qCurveTo', 'curveTo'): cur.extend(a for a in args if a is not None)
        big = max(rings, key=lambda c: abs(area(c)))
        rec.replay(ReverseContourPen(pen) if area(big) < 0 else pen)

def cross(cx, cy, half, arm):   # D-pad outline (one closed ring)
    a, h = arm / 2, half
    return [(cx - a, cy + h, 1), (cx + a, cy + h, 1), (cx + a, cy + a, 1), (cx + h, cy + a, 1), (cx + h, cy - a, 1), (cx + a, cy - a, 1),
            (cx + a, cy - h, 1), (cx - a, cy - h, 1), (cx - a, cy - a, 1), (cx - h, cy - a, 1), (cx - h, cy + a, 1), (cx - a, cy + a, 1)]

def build(out, letter_font, style='switch'):
    L = Letters(letter_font); glyphs = {}; adv = {}; cmap = {}
    def glyph(name, codes, width, draw):
        pen = TTGlyphPen(None); draw(pen, width / 2); glyphs[name] = pen.glyph(); adv[name] = width
        for c in codes: cmap[c] = name
    def round_btn(t):
        def d(pen, cx): contour(pen, circle(cx, CY, R)); L.knock(pen, t, cx, CY, 470)
        return d
    def plate(t, w, h=620, th=400):
        def d(pen, cx): contour(pen, rrect(cx - w / 2, CY - h / 2, cx + w / 2, CY + h / 2, 220)); L.knock(pen, t, cx, CY, th)
        return d
    def sign(minus):
        def d(pen, cx):
            contour(pen, circle(cx, CY, R)); t, l = 70, 250
            if minus: contour(pen, rect(cx - l, CY - t, cx + l, CY + t), hole=True)
            else: contour(pen, cross(cx, CY, l, 2 * t), hole=True)
        return d
    def dpad(arm_dir):   # None = all solid; otherwise only one arm filled
        def d(pen, cx):
            contour(pen, cross(cx, CY, R, 320))
            if arm_dir is None: return
            contour(pen, cross(cx, CY, R - 55, 320 - 110), hole=True)
            arm = rect(cx - 105, CY + 105, cx + 105, CY + R - 55)
            contour(pen, rot(arm, cx, CY, {'up': 0, 'left': 90, 'down': 180, 'right': 270}[arm_dir]))
        return d
    def arrow(deg):
        def d(pen, cx):
            p = [(-380, 105, 1), (60, 105, 1), (60, 310, 1), (420, 0, 1), (60, -310, 1), (60, -105, 1), (-380, -105, 1)]
            contour(pen, rot([(cx + x, CY + y, on) for x, y, on in p], cx, CY, deg))
        return d
    def stick(t):
        def d(pen, cx):
            contour(pen, circle(cx, CY, R)); contour(pen, circle(cx, CY, R - 70), hole=True); contour(pen, circle(cx, CY, 300))
            if t: L.knock(pen, t, cx, CY, 300)
        return d

    pen = TTGlyphPen(None); glyphs['.notdef'] = pen.glyph(); adv['.notdef'] = 500
    def menu_btn():   # Xbox Menu: three bars
        def d(pen, cx):
            contour(pen, circle(cx, CY, R))
            for dy in (-150, 0, 150): contour(pen, rect(cx - 230, CY + dy - 45, cx + 230, CY + dy + 45), hole=True)
        return d
    def view_btn():   # Xbox View: two overlapping squares, drawn as two outlined frames
        def d(pen, cx):
            contour(pen, circle(cx, CY, R))
            for ox, oy in ((-70, 70), (70, -70)):
                x0, y0, x1, y1 = cx + ox - 170, CY + oy - 170, cx + ox + 170, CY + oy + 170
                contour(pen, rect(x0, y0, x1, y1), hole=True)
                contour(pen, rect(x0 + 60, y0 + 60, x1 - 60, y1 - 60))
        return d
    # Face buttons in Nintendo order A (east), B (south), X (north), Y (west): which letter each code point shows
    faces = {'switch': 'ABXY', 'xbox-label': 'ABXY', 'xbox': 'BAYX'}[style]
    for i, t in enumerate(faces): glyph('btn' + 'ABXY'[i], [0xE0A0 + i, 0xE0E0 + i], 1000, round_btn(t))
    shoulders = ([('L', 900), ('R', 900), ('ZL', 1200), ('ZR', 1200)] if style == 'switch'
                 else [('LB', 1200), ('RB', 1200), ('LT', 1200), ('RT', 1200)])
    for i, (t, w) in enumerate(shoulders + [('SL', 1200), ('SR', 1200)]):
        glyph('btn' + ['L', 'R', 'ZL', 'ZR', 'SL', 'SR'][i], [0xE0A4 + i, 0xE0E4 + i], w + 100, plate(t, w))
    if style == 'switch':
        glyph('btnPlus', [0xE0B5, 0xE0EF], 1000, sign(False)); glyph('btnMinus', [0xE0B6, 0xE0F0], 1000, sign(True))
    else:
        glyph('btnPlus', [0xE0B5, 0xE0EF], 1000, menu_btn()); glyph('btnMinus', [0xE0B6, 0xE0F0], 1000, view_btn())
    glyph('dpad', [0xE0EA], 1000, dpad(None))
    for c, dr in [(0xE0AB, 'right'), (0xE0AC, 'down'), (0xE0AD, 'up'), (0xE0AE, 'left')]: glyph('dpad_' + dr, [c, c + 0x40], 1000, dpad(dr))
    for c, (nm, deg) in zip(range(0xE090, 0xE098), [('right', 0), ('left', 180), ('up', 90), ('down', 270), ('rightup', 45), ('rightdown', 315), ('leftdown', 225), ('leftup', 135)]):
        glyph('arrow_' + nm, [c], 1000, arrow(deg))
    glyph('stick', [0xE100], 1000, stick(None)); glyph('stickL', [0xE101, 0xE0C1], 1000, stick('L')); glyph('stickR', [0xE102, 0xE0C2], 1000, stick('R'))
    # The other code points of the private-use block the real font covers get a plain ring: a game may take one of its glyphs without a null check (Super Mario 3D All-Stars' launcher
    # sets U+E06B as its font's alternate character at boot and read through the null glyph), and in text a ring is no worse than the "?" of a missing icon
    glyph('icon', [c for c in range(0xE000, 0xE200) if c not in cmap], 1000, lambda pen, cx: (contour(pen, circle(cx, CY, R)), contour(pen, circle(cx, CY, R - 70), hole=True)))

    order = ['.notdef'] + sorted(n for n in glyphs if n != '.notdef')
    fb = FontBuilder(EM, isTTF=True)
    fb.setupGlyphOrder(order); fb.setupCharacterMap(cmap); fb.setupGlyf(glyphs)
    glyf = fb.font['glyf']
    for n in order: glyf[n].recalcBounds(glyf)
    fb.setupHorizontalMetrics({n: (adv[n], getattr(glyf[n], 'xMin', 0)) for n in order})
    fb.setupHorizontalHeader(ascent=ASC, descent=DESC)
    fb.setupNameTable({'familyName': 'DM Nintendo Extension Stand-in', 'styleName': 'Regular'})
    fb.setupOS2(sTypoAscender=ASC, sTypoDescender=DESC, sTypoLineGap=0, usWinAscent=ASC, usWinDescent=-DESC)
    fb.setupPost(); fb.save(out)
    print('%s: %d glyphs, %d code points' % (out, len(order), len(cmap)))

BFTTF_MAGIC = 0x7f9a0218

def bfttf(ttf, key=0x49621806):
    # nn::pl's BFTTF wrapping (runtime/ov_system.c ov_bfttf_size): big-endian words XORed with key, after
    # MAGIC ^ key and size ^ key
    import struct
    data = ttf + bytes(1) * (-len(ttf) % 4)
    out = struct.pack('>II', BFTTF_MAGIC ^ key, len(ttf) ^ key)
    words = struct.unpack('>%dI' % (len(data) // 4), data)
    return out + struct.pack('>%dI' % len(words), *(w ^ key for w in words))[:len(ttf)]

# Linux has no Arial: the system's bold sans as fontconfig names it, else the places DejaVu, Liberation and Noto take
# on Debian and Ubuntu, Arch and SteamOS, and Fedora. A collection (.ttc) is passed over: TTFont would need its index.
LINUX_FONTS = ('/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf', '/usr/share/fonts/TTF/DejaVuSans-Bold.ttf',
               '/usr/share/fonts/dejavu-sans-fonts/DejaVuSans-Bold.ttf',
               '/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf',
               '/usr/share/fonts/liberation/LiberationSans-Bold.ttf', '/usr/share/fonts/noto/NotoSans-Bold.ttf',
               '/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf')


def default_letter_font():
    import os, subprocess
    if os.name == 'nt':
        return 'C:/Windows/Fonts/arialbd.ttf'
    if sys.platform == 'darwin':
        return '/System/Library/Fonts/Supplemental/Arial Rounded Bold.ttf'
    try:
        r = subprocess.run(['fc-match', '-f', '%{file}', 'sans-serif:bold'], capture_output=True, text=True, timeout=20)
        if r.returncode == 0 and r.stdout.lower().endswith(('.ttf', '.otf')) and os.path.isfile(r.stdout):
            return r.stdout
    except (OSError, subprocess.SubprocessError):
        pass
    return next((f for f in LINUX_FONTS if os.path.isfile(f)), LINUX_FONTS[0])


if __name__ == '__main__':
    import argparse, os, tempfile
    ap = argparse.ArgumentParser(description='a stand-in for the Nintendo extension shared font (button icons)')
    ap.add_argument('out', help='.ttf, or .bfttf for the BFTTF wrapping')
    ap.add_argument('letter_font', nargs='?', default=default_letter_font())
    ap.add_argument('--style', choices=('switch', 'xbox-label', 'xbox'), default='switch')
    a = ap.parse_args()
    if a.out.lower().endswith('.bfttf'):
        tmp = os.path.join(tempfile.gettempdir(), 'rekarina_ext_font.ttf')
        build(tmp, a.letter_font, a.style)
        raw = open(tmp, 'rb').read()
        open(a.out, 'wb').write(bfttf(raw))
        os.remove(tmp)
        print('%s: BFTTF, %d bytes of font' % (a.out, len(raw)))
    else:
        build(a.out, a.letter_font, a.style)
