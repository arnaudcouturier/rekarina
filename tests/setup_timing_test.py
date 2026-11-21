"""tools/setup_timing.py: the timing record from a setup's events, and the release check against the release before.
Game-free: a stand-in setup folder whose installer prints events as installer/oot.py does.

    python tests/setup_timing_test.py
"""
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'tools'))
import setup_timing  # noqa: E402

STAND_IN = """import json, sys
args = sys.argv[1:]
e = lambda **k: print(json.dumps(k), flush=True)
e(type='start', release='9.9.9', steps=['oot_lift', 'oot_compile'])
print('a plain line between the events')
e(type='progress', step='oot_lift', phase='lift', done=1, total=2)
e(type='step', step='oot_lift', state='done', seconds=12.5)
e(type='progress', step='oot_compile', phase='compile', done=1, total=2)
e(type='step', step='oot_compile', state='done', seconds=40)
e(type='end', result='ok' if '--dest' in args and '--no-desktop-shortcut' in args else 'bad arguments')
"""


class TimingTest(unittest.TestCase):
    def test_a_run_records_each_step(self):
        with tempfile.TemporaryDirectory() as tmp:
            setup = Path(tmp) / 'setup'
            (setup / 'installer').mkdir(parents=True)
            (setup / 'installer' / '__init__.py').write_text('')
            (setup / 'installer' / 'oot.py').write_text(STAND_IN)
            (setup / 'VERSION').write_text('9.9.9\n')
            lines = []
            record = setup_timing.run(setup, Path(tmp) / 'game.nup', Path(tmp) / 'dest', appdata=Path(tmp) / 'app',
                                      out=lines.append)
        self.assertEqual(record['result'], 'ok')
        self.assertEqual(record['release'], '9.9.9')
        self.assertEqual(record['steps'], {'oot_lift': 12.5, 'oot_compile': 40.0})
        self.assertEqual(sorted(record['phases']), ['oot_compile/compile', 'oot_lift/lift'])
        self.assertTrue(any('oot_compile done in 40 s' in x for x in lines))

    def test_compare_flags_a_clearly_slower_step(self):
        old = {'steps': {'oot_lift': 600, 'oot_compile': 900, 'oot_package': 20}}
        new = {'steps': {'oot_lift': 700, 'oot_compile': 1300, 'oot_package': 70, 'oot_new': 5}}
        # the lift is within a quarter, the package within the minute's grace; a step new in this release has no
        # time to compare with
        self.assertEqual(setup_timing.compare(old, new), [('oot_compile', 900, 1300)])
        self.assertEqual(setup_timing.compare(old, new, {'oot_compile'}), [])

    def test_a_folder_that_is_not_empty_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            dest = Path(tmp) / 'dest'
            dest.mkdir()
            (dest / 'old.txt').write_text('x')
            with self.assertRaises(SystemExit):
                setup_timing.main([tmp, '--game', 'g.nup', '--dest', str(dest), '--json', str(Path(tmp) / 't.json')])

    def test_events_that_are_not_json_are_skipped(self):
        self.assertIsNone(setup_timing.event('compile: 12 s'))
        self.assertIsNone(setup_timing.event('{broken'))
        self.assertEqual(setup_timing.event(json.dumps({'type': 'end'})), {'type': 'end'})


if __name__ == '__main__':
    unittest.main()
