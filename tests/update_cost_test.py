"""tools/update_cost.py: which steps an update makes a player's setup run again, on two synthetic setup folders, and its
input lists against installer/oot.py's keys.

    python tests/update_cost_test.py
"""
import contextlib
import io
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
sys.path.insert(0, str(ROOT))
import update_cost as uc  # noqa: E402
from installer import oot  # noqa: E402


def setup_folder(base):
    for rel, text in (('recomp/gen.py', '"""Lifter."""\ndef f():\n    return 1\n'),
                      ('recomp/patches-oot-v111.txt', '# patches\nset realtime REKARINA_REALTIME 1\n'),
                      ('runtime/main.c', 'int main(void) { return 0; }\n'),
                      ('tools/build_game.py', 'x = 1\n'), ('tools/package_oot.py', 'x = 1\n'),
                      ('tools/oot_prepare_shaders.py', 'x = 1\n'), ('tools/make_oot_prompts.py', 'x = 1\n'),
                      ('bin/rekarina_xlate.id', 'a' * 64)):
        p = base / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text)
    return base


class UpdateCostTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.old = setup_folder(Path(self.tmp.name) / 'old')
        self.new = Path(self.tmp.name) / 'new'
        shutil.copytree(self.old, self.new)

    def tearDown(self):
        self.tmp.cleanup()

    def run_it(self, *extra):
        with contextlib.redirect_stdout(io.StringIO()) as out:
            code = uc.main([str(self.old), str(self.new), *extra])
        changed = {name for name, _, c in uc.compare(self.old, self.new) if c}
        return code, changed, out.getvalue()

    def test_a_comment_costs_nothing(self):
        (self.new / 'recomp/gen.py').write_text('"""The lifter, said otherwise."""\ndef f():\n    # why\n    return 1\n')
        (self.new / 'recomp/patches-oot-v111.txt').write_text('# other words\n\nset  realtime REKARINA_REALTIME 1  # on\n')
        self.assertEqual(self.run_it()[:2], (0, set()))

    def test_long_steps_fail_unless_allowed(self):
        (self.new / 'recomp/patches-oot-v111.txt').write_text('set realtime REKARINA_REALTIME 0\n')
        (self.new / 'tools/oot_prepare_shaders.py').write_text('x = 2\n')
        code, changed, out = self.run_it()
        self.assertEqual((code, changed), (1, {'lift', 'shaders'}))
        self.assertIn('LONG steps would run again', out)
        self.assertEqual(self.run_it('--allow=lift,shaders')[0], 0)

    def test_short_steps_pass(self):
        (self.new / 'runtime/main.c').write_text('int main(void) { return 1; }\n')
        (self.new / 'tools/package_oot.py').write_text('x = 2\n')
        self.assertEqual(self.run_it()[:2], (0, {'compile', 'package'}))

    def test_a_new_key_form_runs_the_step_unless_the_old_one_is_adopted(self):
        (self.old / 'installer').mkdir()
        (self.new / 'installer').mkdir()
        (self.old / 'installer/oot.py').write_text("k = fingerprint('oot-lift-2', a)\n")
        (self.new / 'installer/oot.py').write_text("k = fingerprint('oot-lift-4', a)\n")
        code, changed, out = self.run_it()
        self.assertEqual((code, changed), (1, {'lift'}))
        self.assertIn('oot-lift-2 -> oot-lift-4', out)
        # the new setup still computes the old form (Step.adopts): the record stands while the inputs keep their bytes
        (self.new / 'installer/oot.py').write_text("k = fingerprint('oot-lift-4', a)\nold = fingerprint('oot-lift-2', a)\n")
        self.assertEqual(self.run_it()[:2], (0, set()))
        (self.new / 'recomp/gen.py').write_text('"""Lifter, other words."""\ndef f():\n    return 1\n')   # same code
        self.assertEqual(self.run_it()[:2], (1, {'lift'}))

    def test_inputs_match_the_keys(self):
        shaders = next(i for n, _, i, _ in uc.STEPS if n == 'shaders')
        want = {f'tools/{n}' for n in oot.SHADER_TOOLS} | {f'tools/data/{n}' for n in oot.SHADER_DATA}
        self.assertTrue(want <= set(shaders), want - set(shaders))
        lift = next(i for n, _, i, _ in uc.STEPS if n == 'lift')
        self.assertTrue({'recomp', 'tools/cheats.py', 'tools/exefs_patches.py'} <= set(lift))


if __name__ == '__main__':
    unittest.main()
