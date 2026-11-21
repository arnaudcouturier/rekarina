"""tools/oot_exefs.py find_nup: which .nup the setup takes from a folder. Game-free: empty stand-in files, their
title ids given by a stand-in reader.

    python tests/oot_exefs_test.py
"""
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'tools'))
import oot_exefs  # noqa: E402

GAME = oot_exefs.TITLE
OTHER = 0x0400111111110000


class FindNupTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def nup(self, rel, size, *titles):
        p = self.dir / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(bytes(size))
        self.titles[p] = set(titles)
        return p

    def find(self):
        # (as titles_of gives them: the application, the update's low bits cleared)
        return oot_exefs.find_nup(self.dir, titles=lambda p: {t & ~0xFFF for t in self.titles.get(p, set())})

    titles = None

    def test_the_game_among_other_games_dumps(self):
        # the report: a folder of several Switch 2 dumps, another game's the largest
        self.titles = {}
        self.nup('other.nup', 3000, OTHER, OTHER | 0x800)
        mine = self.nup('mine.nup', 1000, GAME, GAME | 0x800)   # base and update packages
        self.nup('sub/third.nup', 2000, OTHER + 0x10000)
        self.assertEqual(self.find(), mine)

    def test_one_level_below_and_the_largest_of_two_copies(self):
        self.titles = {}
        self.nup('a.nup', 500, GAME)
        big = self.nup('dumps/b.nup', 900, GAME | 0x800)
        self.assertEqual(self.find(), big)

    def test_none_of_them_the_game_gives_the_largest(self):
        # (the extraction then says the file is not this game's version)
        self.titles = {}
        self.nup('x.nup', 100, OTHER)
        big = self.nup('y.nup', 200)
        self.assertEqual(self.find(), big)

    def test_a_file_named_directly_is_taken(self):
        self.titles = {}
        other = self.nup('other.nup', 10, OTHER)
        self.assertEqual(oot_exefs.find_nup(other, titles=lambda p: set()), other)
        self.assertIsNone(oot_exefs.find_nup(self.dir / 'nothing'))

    def test_an_unreadable_file_has_no_titles(self):
        p = self.dir / 'junk.nup'
        p.write_bytes(b'not a container')
        self.assertEqual(oot_exefs.titles_of(p), set())


if __name__ == '__main__':
    unittest.main()
