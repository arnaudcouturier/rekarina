"""tools/build_game.py's content stamps (obj/inputs.sha256): an output is made again when what it was made from changed,
whatever the file times say, and a build folder from before the stamps compiles its runtime objects again.

Game-free: a temporary folder stands in for a release folder, Ninja's deps log and the compiler's version are stubbed.

    python tests/build_content_test.py
"""
import os
import sys
import tempfile
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'tools'))
import build_game as bg  # noqa: E402

OLD = time.mktime((2026, 11, 19, 12, 0, 0, 0, 0, -1))   # a release file's time, read in another zone: in the past


class ContentStampTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name).resolve()
        self.saved = bg.ROOT, bg.ninja_deps, bg.compiler_id
        bg.ROOT = self.root
        bg._content.clear()
        self.deps = {}
        bg.ninja_deps = lambda path, env: {bg.output_key(o): list(d) for o, d in self.deps.items()}
        bg.compiler_id = lambda: 'clang version 21.1.0'
        self.out = self.root / 'temp' / 'oot-x' / 'build'
        rt = self.root / 'runtime'
        self.main_c, self.report_c, self.host_h = rt / 'main.c', rt / 'report.c', rt / 'host.h'
        self.gen_c = self.root / 'temp' / 'oot-x' / 'gen' / 'main' / 'main_0001.c'
        for f, text in ((self.main_c, 'int main(void) { return fg(); }\n'), (self.report_c, 'int other(void);\n'),
                        (self.host_h, 'int fg(void);\n'), (self.gen_c, 'void f_1(void) {}\n')):
            f.parent.mkdir(parents=True, exist_ok=True)
            f.write_text(text)
        self.main_o = self.out / 'obj' / 'runtime' / 'main.o'
        self.report_o = self.out / 'obj' / 'runtime' / 'report.o'
        self.gen_o = self.out / 'obj' / 'main' / 'main_0001.o'
        self.exe = self.out / 'rekarina.exe'
        self.edges = {self.main_o: ('clang -O2 rt', [self.main_c]), self.report_o: ('clang -O2 rt', [self.report_c]),
                      self.gen_o: ('clang -O2 gen', [self.gen_c]), self.exe: ('link 3 objects', [])}
        self.deps = {self.main_o: ['runtime/main.c', 'runtime/host.h'], self.report_o: ['runtime/report.c', 'runtime/host.h'],
                     self.gen_o: ['temp/oot-x/gen/main/main_0001.c']}

    def tearDown(self):
        bg.ROOT, bg.ninja_deps, bg.compiler_id = self.saved
        self.tmp.cleanup()

    def build(self, *outputs):
        """As Ninja and the link would: write each output (newer than every source)."""
        for o in outputs:
            o.parent.mkdir(parents=True, exist_ok=True)
            o.write_text(f'made from the current inputs at {time.time()}')

    def record(self):
        bg.record_inputs(self.out, self.edges, {}, [o for o in self.edges if o != self.exe])
        bg.record_inputs(self.out, self.edges, {}, [self.exe])

    def check(self):
        bg._content.clear()
        bg.drop_stale_by_content(self.out, self.edges, {})
        return {o.name for o in self.edges if not o.is_file()}

    def test_unchanged_is_kept(self):
        self.build(*self.edges)
        self.record()
        self.assertEqual(self.check(), set())

    def test_source_changed_with_an_older_time_is_made_again(self):
        # the release case: a new report.c whose file time is older than the report.o of the release before
        self.build(*self.edges)
        self.record()
        self.report_c.write_text('int fg(void) { return 1; }\n')
        os.utime(self.report_c, (OLD, OLD))
        self.assertLess(self.report_c.stat().st_mtime, self.report_o.stat().st_mtime)   # Ninja would keep it
        self.assertEqual(self.check(), {'report.o'})

    def test_header_changed_makes_its_includers_again(self):
        self.build(*self.edges)
        self.record()
        self.host_h.write_text('int fg(void);\nint fg_guard_crashed(void);\n')
        os.utime(self.host_h, (OLD, OLD))
        self.assertEqual(self.check(), {'main.o', 'report.o'})

    def test_flags_or_compiler_changed(self):
        self.build(*self.edges)
        self.record()
        self.edges[self.gen_o] = ('clang -O1 gen', [self.gen_c])
        self.assertEqual(self.check(), {'main_0001.o'})
        self.build(self.gen_o)
        self.record()
        bg.compiler_id = lambda: 'clang version 22.0.0'
        self.assertEqual(self.check(), set(o.name for o in self.edges))

    def test_link_input_changed(self):
        lib = self.root / 'runtime' / 'deps' / 'lib' / 'libvpx.a'
        lib.parent.mkdir(parents=True)
        lib.write_bytes(b'old')
        self.edges[self.exe] = ('link 3 objects', [lib])
        self.build(*self.edges)
        self.record()
        lib.write_bytes(b'new library')
        os.utime(lib, (OLD, OLD))
        self.assertEqual(self.check(), {'rekarina.exe'})

    def test_folder_without_a_record_makes_its_runtime_objects_again(self):
        # a temp folder an earlier release's setup built (and one the 1.2.4 update left half new): no record yet
        self.build(*self.edges)
        self.assertFalse((self.out / 'obj' / bg.INPUTS).exists())
        self.assertEqual(self.check(), {'main.o', 'report.o'})   # the generated object is kept
        self.build(self.main_o, self.report_o)
        self.record()
        self.assertEqual(self.check(), set())

    def test_record_keeps_other_entries_and_round_trips(self):
        self.build(*self.edges)
        self.record()
        before = (self.out / 'obj' / bg.INPUTS).read_text()
        self.assertEqual(len(before.splitlines()), 4)
        bg.record_inputs(self.out, self.edges, {}, [self.main_o])
        self.assertEqual((self.out / 'obj' / bg.INPUTS).read_text(), before)


if __name__ == '__main__':
    unittest.main()
