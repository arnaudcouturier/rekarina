"""The action hint's layout with its four face buttons where a pad mapped by label has them (tools/make_oot_prompts.py).

The hint in the screen's corner (DoCommand_00 of the common layout archive) is a diamond of the four face buttons as
the console has them: A on the right, B at the bottom, X at the top, Y on the left. The button an action is on shows as
a pill with the action's name on that side, the other three as small images in their places. A pad mapped by label
(REKARINA_PAD_LAYOUT = xbox, and auto for every pad but a Switch one) does the game's A with its bottom button, B with
the right one, X with the left one and Y with the top one, so the diamond showed each button on the wrong side: the
cross of a PlayStation pad on the right. by_label moves them: A to the bottom, B to the right, X to the left, Y to the
top.

The file is a "BFFH" resource (Nintendo.Ui.ResModels.LayoutResData) of tables, each a header of three 64-bit offsets
(the names' pointers, the records' pointers, the end) and a count. The ones used here, by the header's offset:
  0x118  panes: 0x44-byte records of 17 words: +0x10 scale x, y; +0x1c translate x, y, z; +0x28 the parent's index;
         +0x2c size w, h; +0x34 the index after the pane's last descendant; +0x38 flags (bit 0 visible, bits 8..15 the
         pane's origin); +0x3c the indices of its drawable, material and text records
  0x178  texts: 0xa8-byte records; +0x18 the pointer to the "@name" the game sets the text by; +0x70 and +0x78 the
         alignment (0, or 2 where the text ends at the pane's edge, as in the pill on the left)
The four button groups N_A_00, N_B_00, N_X_00 and N_Y_00 have the same panes in the same order: the diamond's base
(P_Base<b>_00), the three other buttons' images (P_<b>_<nn>), and the pill (N_Btn<b>_00 with its window, the flash of a
press, the action's name T_Guide<b>_00 and the button T_Key<b>_00). Which group shows is the ButtonPattern animation's
choice by pane, and the game sets the name by the text's "@name", so a group keeps its identity and takes the other
side's geometry: the pill's panes trade their transforms, sizes and origins (and the name's alignment) between A and B
and between X and Y, each small image goes to its button's place, and the base, an image of the diamond with one arm
lit (DoCommandCrossBase_A_00 the right one, _B_00 the bottom one, mirrored for the other two), gets the other image by
trading the two names in the string pool. Nothing changes its size: the file is edited in place.
"""
import struct

PANES, TEXTS = 0x118, 0x178
PANE_WORDS = 17
GEOMETRY = (0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 11, 12)   # the words of a pane record that place and size it
FLAGS, ORIGIN_BITS = 14, 0x0000FF00
TEXT_ALIGN = (0x70, 0x78)
PILL = ('P_Base{}_00', 'N_Btn{}_00', 'W_Base{}_00', 'N_Base{}Decide_00', 'W_Base{}Decide_00', 'T_Guide{}_00', 'T_Key{}_00')
PAIRS = (('A', 'B'), ('X', 'Y'))
# Where a pad mapped by label has the button that does each of the game's four (x right, y up, the layout's units)
PLACE = {'A': (0.0, -80.0), 'B': (80.0, 0.0), 'X': (-80.0, 0.0), 'Y': (0.0, 80.0)}
BASE_IMAGES = (('DoCommandCrossBase_A_00', 'DoCommandCrossBase_B_00'), ('DoCommandCrossBaseGrd_A_00', 'DoCommandCrossBaseGrd_B_00'))


class LayoutError(ValueError):
    pass


def _table(data, header):
    """{name: record offset} of the table whose header is at `header`."""
    names, records, _, count = struct.unpack_from('<3QI', data, header)
    out = {}
    for i in range(count):
        n = struct.unpack_from('<Q', data, names + 8 * i)[0]
        out[bytes(data[n:data.index(b'\0', n)]).decode()] = struct.unpack_from('<Q', data, records + 8 * i)[0]
    return out


def _trade(data, a, b, size=4):
    data[a:a + size], data[b:b + size] = data[b:b + size], data[a:a + size]


def by_label(data):
    """data: the bytearray of DoCommand_00.buil, edited in place. Raises LayoutError when the file is not the one this
    was written for (another version of the game): the caller then leaves the hint as the game has it."""
    if data[:4] != b'BFFH' or b'Nintendo.Ui.ResModels.LayoutResData' not in data[:0x80]:
        raise LayoutError('not a BFFH layout')
    panes, texts = _table(data, PANES), _table(data, TEXTS)
    want = [p.format(b) for p in PILL for pair in PAIRS for b in pair] + [f'N_{b}_00' for pair in PAIRS for b in pair]
    missing = [n for n in want if n not in panes]
    if missing:
        raise LayoutError(f'panes missing: {missing}')
    # Everything is checked before anything is written. A group's small images are the three other buttons, one each
    images, order = [], list(panes)
    for group in 'ABXY':
        first = order.index(f'N_{group}_00')
        kids = [n for n in order[first + 1:first + 11] if len(n) == 6 and n.startswith('P_') and n[2] in PLACE]
        if sorted(n[2] for n in kids) != sorted(set(PLACE) - {group}):
            raise LayoutError(f'the group of {group} has the images {kids}')
        images += kids
    missing = [f'T_Guide{b}_00' for pair in PAIRS for b in pair if f'T_Guide{b}_00' not in texts]
    if missing:
        raise LayoutError(f'texts missing: {missing}')
    pool = []
    for x, y in BASE_IMAGES:
        ax, ay = data.find(x.encode() + b'\0'), data.find(y.encode() + b'\0')
        if ax < 0 or ay < 0 or data.find(x.encode() + b'\0', ax + 1) >= 0 or data.find(y.encode() + b'\0', ay + 1) >= 0:
            raise LayoutError(f'{x} and {y} are not once each in the string pool')
        pool.append((ax, ay, len(x)))
    for n in images:
        struct.pack_into('<2f', data, panes[n] + 4 * 7, *PLACE[n[2]])
    for a, b in PAIRS:
        for pattern in PILL:
            pa, pb = panes[pattern.format(a)], panes[pattern.format(b)]
            for w in GEOMETRY:
                _trade(data, pa + 4 * w, pb + 4 * w)
            fa, fb = (struct.unpack_from('<I', data, p + 4 * FLAGS)[0] for p in (pa, pb))
            struct.pack_into('<I', data, pa + 4 * FLAGS, fa & ~ORIGIN_BITS | fb & ORIGIN_BITS)
            struct.pack_into('<I', data, pb + 4 * FLAGS, fb & ~ORIGIN_BITS | fa & ORIGIN_BITS)
        ta, tb = texts[f'T_Guide{a}_00'], texts[f'T_Guide{b}_00']
        for o in TEXT_ALIGN:
            _trade(data, ta + o, tb + o)
    for ax, ay, size in pool:
        _trade(data, ax, ay, size)
    return data


def describe(data):
    """[(group, pill x, y, [(image, x, y)])] of the four groups, for a check or a picture."""
    panes = _table(data, PANES)
    out = []
    for group in 'ABXY':
        first = list(panes).index(f'N_{group}_00')
        kids = [n for n in list(panes)[first + 1:first + 11] if len(n) == 6 and n.startswith('P_')]
        pill = struct.unpack_from('<2f', data, panes[f'N_Btn{group}_00'] + 4 * 7)
        out.append((group, pill[0], pill[1], [(n,) + struct.unpack_from('<2f', data, panes[n] + 4 * 7) for n in kids]))
    return out
