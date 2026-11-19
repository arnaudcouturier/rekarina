"""tools/oot_hint_layout.py on a layout made here, with no game files: the four button groups of the action hint trade
their geometry between A and B and between X and Y, each small image goes to its button's place on a pad mapped by
label, the name texts trade their alignment, the two base images trade names, every other byte stays, and a file that
is not that layout is refused before anything changes.

Run: .venv\\Scripts\\python tests\\oot_hint_layout_test.py
"""
import struct
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import oot_hint_layout as hl   # noqa: E402

GROUP = ('N_{b}_00', 'P_Base{b}_00', 'P_{o0}_0{k}', 'P_{o1}_0{k}', 'P_{o2}_0{k}', 'N_Btn{b}_00', 'W_Base{b}_00',
         'N_Base{b}Decide_00', 'W_Base{b}Decide_00', 'T_Guide{b}_00', 'T_Key{b}_00')
# As the game lays the diamond out: the pill's place per group, and each button's small image
PILL = {'A': (80.0, 0.0), 'B': (0.0, -80.0), 'X': (0.0, 80.0), 'Y': (-80.0, 0.0)}
SMALL = {'A': (80.0, 0.0), 'B': (0.0, -80.0), 'X': (0.0, 80.0), 'Y': (-80.0, 0.0)}


def make():
    """A file with the two tables by_label reads (panes at 0x118, texts at 0x178), a string pool and the two base
    image names; returns (bytes, {pane name: record offset}, {text name: record offset})."""
    names = ['RootPane']
    for k, b in enumerate('ABXY'):   # the game numbers the small images so that no two panes share a name
        others = [o for o in 'ABXY' if o != b]
        names += [n.format(b=b, k=k, o0=others[0], o1=others[1], o2=others[2]) for n in GROUP]
    names.append('N_L_00')
    texts = [n for n in names if n.startswith('T_Guide')]
    pool_names = names + [x for pair in hl.BASE_IMAGES for x in pair]
    head = 0x400
    pane_names, pane_ptrs = head, head + 8 * len(names)
    pane_recs = pane_ptrs + 8 * len(names)
    text_names = pane_recs + 0x44 * len(names)
    text_ptrs = text_names + 8 * len(texts)
    text_recs = text_ptrs + 8 * len(texts)
    pool = text_recs + 0xa8 * len(texts)
    data = bytearray(pool)
    data[:4] = b'BFFH'
    data[0x28:0x28 + 35] = b'Nintendo.Ui.ResModels.LayoutResData'
    at = {}
    for n in pool_names:
        at[n] = len(data)
        data += n.encode() + b'\0'
    struct.pack_into('<3QI', data, hl.PANES, pane_names, pane_ptrs, pane_recs, len(names))
    struct.pack_into('<3QI', data, hl.TEXTS, text_names, text_ptrs, text_recs, len(texts))
    panes, trecs = {}, {}
    for i, n in enumerate(names):
        rec = pane_recs + 0x44 * i
        struct.pack_into('<Q', data, pane_names + 8 * i, at[n])
        struct.pack_into('<Q', data, pane_ptrs + 8 * i, rec)
        w = [0.0] * 17
        w[4] = w[5] = 1.0
        w[11], w[12] = 90.0, 120.0
        if n.startswith('N_Btn'):
            w[7], w[8] = PILL[n[5]]
        elif len(n) == 6 and n.startswith('P_'):
            w[7], w[8] = SMALL[n[2]]
        elif n == 'P_BaseX_00':
            w[5] = -1.0
        elif n in ('P_BaseY_00', 'W_BaseY_00'):
            w[4] = -1.0
        elif n == 'T_GuideY_00':
            w[7] = -68.0
        struct.pack_into('<17f', data, rec, *w)
        struct.pack_into('<I', data, rec + 40, i)                                   # the parent's place: an identity
        struct.pack_into('<I', data, rec + 56, 0x02555909 if n == 'T_GuideY_00' else 0x02554909)
        struct.pack_into('<I', data, rec + 60, 0xffff0000 | i)                      # links: an identity
        panes[n] = rec
    for i, n in enumerate(texts):
        rec = text_recs + 0xa8 * i
        struct.pack_into('<Q', data, text_names + 8 * i, at[n])
        struct.pack_into('<Q', data, text_ptrs + 8 * i, rec)
        struct.pack_into('<Q', data, rec + 0x18, 0x1000 + i)                        # the "@name": an identity
        if n == 'T_GuideY_00':
            struct.pack_into('<I', data, rec + 0x70, 2)
            struct.pack_into('<I', data, rec + 0x78, 2)
        trecs[n] = rec
    return data, panes, trecs, at


def place(data, rec):
    return struct.unpack_from('<2f', data, rec + 28)


class HintLayout(unittest.TestCase):
    def setUp(self):
        self.before, self.panes, self.texts, self.pool = make()
        self.after = hl.by_label(bytearray(self.before))

    def test_pills_and_images_are_where_a_pad_by_label_has_them(self):
        for b in 'ABXY':
            self.assertEqual(place(self.after, self.panes[f'N_Btn{b}_00']), hl.PLACE[b], b)
        for n, rec in self.panes.items():
            if len(n) == 6 and n.startswith('P_'):
                self.assertEqual(place(self.after, rec), hl.PLACE[n[2]], n)

    def test_the_pill_on_the_left_takes_the_mirrored_window_and_the_right_alignment(self):
        scale = lambda n: struct.unpack_from('<f', self.after, self.panes[n] + 16)[0]
        self.assertEqual((scale('W_BaseX_00'), scale('W_BaseY_00')), (-1.0, 1.0))
        self.assertEqual(place(self.after, self.panes['T_GuideX_00'])[0], -68.0)
        flags = lambda n: struct.unpack_from('<I', self.after, self.panes[n] + 56)[0]
        self.assertEqual((flags('T_GuideX_00'), flags('T_GuideY_00')), (0x02555909, 0x02554909))
        align = lambda n: struct.unpack_from('<I', self.after, self.texts[n] + 0x70)[0]
        self.assertEqual((align('T_GuideX_00'), align('T_GuideY_00')), (2, 0))

    def test_identity_stays(self):
        for i, (n, rec) in enumerate(self.panes.items()):
            self.assertEqual(struct.unpack_from('<I', self.after, rec + 40)[0], i, n)
            self.assertEqual(struct.unpack_from('<I', self.after, rec + 60)[0], 0xffff0000 | i, n)
        for i, rec in enumerate(self.texts.values()):
            self.assertEqual(struct.unpack_from('<Q', self.after, rec + 0x18)[0], 0x1000 + i)

    def test_base_images_trade_names_and_nothing_else_moves(self):
        for x, y in hl.BASE_IMAGES:
            self.assertEqual(self.after[self.pool[x]:self.pool[x] + len(y)], y.encode())
            self.assertEqual(self.after[self.pool[y]:self.pool[y] + len(x)], x.encode())
        self.assertEqual(len(self.after), len(self.before))
        root = self.panes['RootPane']
        self.assertEqual(self.after[root:root + 0x44], self.before[root:root + 0x44])
        last = self.panes['N_L_00']
        self.assertEqual(self.after[last:last + 0x44], self.before[last:last + 0x44])

    def test_another_file_is_refused_unchanged(self):
        for bad in (bytearray(b'FLYT' + bytes(0x400)), self.before.replace(b'T_KeyX_00', b'T_KeyQ_00')):
            keep = bytes(bad)
            with self.assertRaises(hl.LayoutError):
                hl.by_label(bad)
            self.assertEqual(bytes(bad), keep)


if __name__ == '__main__':
    unittest.main()
