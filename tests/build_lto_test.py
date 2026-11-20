"""tools/build_game.py's runtime ThinLTO: on in a default (x86-64-v3) build when REKARINA_RT_LTO=1 asks for it, never
in a --cpu v2 build, whose runtime objects tools/check_isa.py must read as machine code (LLVM bitcode stopped every
setup on a CPU without AVX2 at its compile step). Game-free.

    python tests/build_lto_test.py
"""
import os
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'tools'))
import build_game as bg  # noqa: E402


class RuntimeLtoTest(unittest.TestCase):
    def setUp(self):
        self.rt, self.link = list(bg.RT_FLAGS), list(bg.LINK_FLAGS)
        self.env = os.environ.get('REKARINA_RT_LTO')

    def tearDown(self):
        bg.RT_FLAGS[:], bg.LINK_FLAGS[:] = self.rt, self.link
        if self.env is None:
            os.environ.pop('REKARINA_RT_LTO', None)
        else:
            os.environ['REKARINA_RT_LTO'] = self.env

    def test_v3_with_lto(self):
        os.environ['REKARINA_RT_LTO'] = '1'
        bg.set_lto('v3')
        self.assertIn('-flto=thin', bg.RT_FLAGS)
        self.assertIn('-flto=thin', bg.LINK_FLAGS)

    def test_v2_without_lto(self):
        os.environ['REKARINA_RT_LTO'] = '1'
        bg.set_lto('v2')
        self.assertNotIn('-flto=thin', bg.RT_FLAGS)
        self.assertNotIn('-flto=thin', bg.LINK_FLAGS)

    def test_not_asked(self):
        os.environ.pop('REKARINA_RT_LTO', None)
        bg.set_lto('v3')
        self.assertNotIn('-flto=thin', bg.RT_FLAGS)


if __name__ == '__main__':
    unittest.main()
