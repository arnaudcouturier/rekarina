"""tools/build_game.py's compile job plan: the memory budget from the memory available at the start, the giant chunks'
estimates from the manifest, and how many jobs (and giants at once) fit, for 8, 16 and 32 GB PCs with 4, 6 and 12
threads. Game-free: synthetic manifests and memory figures.

    python tests/build_jobs_test.py        (-v prints the plan table)
"""
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'tools'))
import build_game as bg  # noqa: E402

GiB = bg.GiB
# OoT's release generated C, as its manifests describe it: 19 chunks of one function of 1.0 to 3.9 MB, ~530 others
GIANT_MB = [3.9, 2.8, 2.45, 2.2, 1.8, 1.6, 1.6, 1.3, 1.3, 1.3, 1.3, 1.3, 1.3, 1.25, 1.25, 1.2, 1.2, 1.1, 1.0]
LIGHT_N = 530


def release_estimates():
    giants = [int(bg.GIANT_MEM_BASE + bg.GIANT_MEM_PER_MB * mb) for mb in GIANT_MB]
    return giants + [bg.LIGHT_MEM] * LIGHT_N


def worst_case(jobs, giants, estimates):
    """The most memory the plan can have in use: the largest giants in their pool, light compiles in the rest."""
    big = sorted((e for e in estimates if e > bg.LIGHT_MEM), reverse=True)[:giants]
    return sum(big) + (jobs - len(big)) * bg.LIGHT_MEM


def budget_for(total_gb, used_gb):
    """compile_budget's figure on a PC with total_gb of memory of which used_gb is in use at the start."""
    saved = bg.memory_status
    bg.memory_status = lambda: (int(total_gb * GiB), int((total_gb - used_gb) * GiB))
    try:
        os.environ.pop('REKARINA_BUILD_MEM_GB', None)
        return bg.compile_budget()[0]
    finally:
        bg.memory_status = saved


class PlanTest(unittest.TestCase):
    def test_budget_is_available_less_the_reserve(self):
        self.assertEqual(budget_for(16, 5), int(11 * GiB) - bg.MEM_RESERVE)
        self.assertEqual(budget_for(8, 6), GiB)                 # never under 1 GiB
        self.assertEqual(budget_for(128, 8), bg.MEM_BUDGET)     # never over MEM_BUDGET

    def test_budget_knob(self):
        os.environ['REKARINA_BUILD_MEM_GB'] = '7.5'
        try:
            budget, why = bg.compile_budget()
        finally:
            del os.environ['REKARINA_BUILD_MEM_GB']
        self.assertEqual(budget, int(7.5 * GiB))
        self.assertIn('REKARINA_BUILD_MEM_GB', why)

    def test_plans_fit_their_budget(self):
        est = release_estimates()
        rows = []
        for total, used in ((8, 3.5), (16, 4.5), (16, 7), (32, 6)):
            budget = budget_for(total, used)
            for cpus in (4, 6, 12):
                jobs, giants = bg.plan_jobs(est, cpus, budget)
                peak = worst_case(jobs, giants, est)
                rows.append((total, used, cpus, budget, jobs, giants, peak))
                self.assertTrue(1 <= giants <= jobs <= cpus)
                if max(est) <= budget:   # a single giant that does not fit runs alone, nothing else beside it
                    self.assertLessEqual(peak, budget, (total, used, cpus))
                else:
                    self.assertEqual((jobs, giants), (1, 1))
        if '-v' in sys.argv:
            print('\nRAM  in use  threads  budget   jobs  giants  worst case')
            for total, used, cpus, budget, jobs, giants, peak in rows:
                print(f'{total:3} GB {used:5.1f}  {cpus:5}   {budget / GiB:5.1f} GiB {jobs:4} {giants:6}   {peak / GiB:5.1f} GiB')

    def test_the_freezing_pc(self):
        # the report: 16 GB, 12 threads, nothing else running (about 4.5 GB in use): the old rule gave 12 jobs, the
        # giants together over 16 GB; now at most one giant beside light compiles, within the budget
        est = release_estimates()
        budget = budget_for(16, 4.5)
        jobs, giants = bg.plan_jobs(est, 12, budget)
        self.assertEqual(giants, 1)
        self.assertLessEqual(worst_case(jobs, giants, est), budget)
        self.assertLess(12 * max(est), 16 * GiB * 3)   # (what twelve giants together would have needed: over 16 GB)

    def test_no_giants(self):
        jobs, giants = bg.plan_jobs([bg.LIGHT_MEM] * 100, 12, 4 * GiB)
        self.assertEqual((jobs, giants), (4 * GiB // bg.LIGHT_MEM, 0))   # 9 with 448 MiB a light compile

    def test_chunk_memory_from_the_manifest(self):
        with tempfile.TemporaryDirectory() as tmp:
            gen = Path(tmp)
            (gen / 'main').mkdir()
            info = [{'file': 'main_0000.c', 'functions': 1541, 'bytes': 3_813_776},
                    {'file': 'main_0001.c', 'functions': 1, 'bytes': 3_892_912},
                    {'file': 'main_0002.c', 'functions': 1, 'bytes': 120_000},
                    {'file': 'main_0003.c', 'functions': 1, 'bytes': 899_912}]   # one function just under the line
            for c in info:
                (gen / 'main' / c['file']).write_text('x')
            (gen / 'main' / 'main_table.c').write_text('x')
            (gen / 'main' / 'manifest.json').write_text(json.dumps({'chunks': [c['file'] for c in info],
                                                                     'chunk_info': info}))
            files = [gen / 'main' / c['file'] for c in info] + [gen / 'main' / 'main_table.c']
            est = bg.chunk_memory(gen, {'main': files})
        self.assertEqual(est[files[0]], bg.LIGHT_MEM)
        self.assertEqual(est[files[2]], bg.LIGHT_MEM)
        self.assertEqual(est[files[3]], bg.LIGHT_MEM)
        self.assertEqual(est[files[4]], bg.LIGHT_MEM)
        self.assertGreaterEqual(est[files[1]], int(2801 * 1.1) << 20)   # measured 2,801 MiB, with headroom

    def test_giants_first_in_their_pool(self):
        with tempfile.TemporaryDirectory() as tmp:
            gen, out = Path(tmp) / 'gen', Path(tmp) / 'out'
            (gen / 'main').mkdir(parents=True)
            light, giant = gen / 'main' / 'main_0000.c', gen / 'main' / 'main_0001.c'
            light.write_text('x'), giant.write_text('x')
            saved, bg.DEPS = bg.DEPS, Path(tmp) / 'deps'   # a deps folder: the VMA header is not looked for (or fetched)
            try:
                text, objs, _ = bg.ninja_file(gen, out, {'main': [light, giant]}, 5, giants={giant: 3 * GiB},
                                              giant_depth=1)
            finally:
                bg.DEPS = saved
        lines = text.splitlines()
        self.assertIn('pool gen_giant', lines)
        self.assertEqual(lines[lines.index('pool gen_giant') + 1], '  depth = 1')
        gl = next(i for i, l in enumerate(lines) if l.startswith('build ') and 'main_0001.c' in l)
        ll = next(i for i, l in enumerate(lines) if l.startswith('build ') and 'main_0000.c' in l)
        self.assertLess(gl, ll)
        self.assertIn('  pool = gen_giant', lines[gl + 1:gl + 3])
        self.assertIn('  pool = gen', lines[ll + 1:ll + 3])
        self.assertEqual([o.name for o in objs['main']], ['main_0000.o', 'main_0001.o'])   # the link order stays


class ColdObjectsTest(unittest.TestCase):
    def test_listed_objects_compile_at_o1_and_the_rest_as_before(self):
        with tempfile.TemporaryDirectory() as tmp:
            gen, out = Path(tmp) / 'gen', Path(tmp) / 'out'
            (gen / 'main').mkdir(parents=True)
            a, b = gen / 'main' / 'main_0000.c', gen / 'main' / 'main_0001.c'
            a.write_text('x'), b.write_text('x')
            (Path(tmp) / 'cold.txt').write_text('main_0001.o  a giant the profile never entered\n')
            saved_deps, saved_flags = bg.DEPS, list(bg.GEN_FLAGS)
            bg.DEPS = Path(tmp) / 'deps'
            bg.GEN_FLAGS[bg.GEN_FLAGS.index('-O1')] = '-O2'   # (a build with the full profile)
            try:
                plain = {}
                bg.ninja_file(gen, out, {'main': [a, b]}, 4, edges=plain)
                os.environ['REKARINA_COLD_OBJS'] = str(Path(tmp) / 'cold.txt')
                edges = {}
                text, _, _ = bg.ninja_file(gen, out, {'main': [a, b]}, 4, edges=edges)
            finally:
                os.environ.pop('REKARINA_COLD_OBJS', None)
                bg.DEPS, bg.GEN_FLAGS[:] = saved_deps, saved_flags
        oa, ob = out / 'obj' / 'main' / 'main_0000.o', out / 'obj' / 'main' / 'main_0001.o'
        self.assertIn(' -O1 ', edges[ob][0] + ' ')
        self.assertEqual(edges[oa][0], plain[oa][0])   # the others' commands unchanged: nothing compiled again
        self.assertIn('-O2', edges[oa][0])


class GateTest(unittest.TestCase):
    """The memory gate's choice (gate_pick), and on Windows the gate itself on real processes."""
    L, G = bg.LIGHT_MEM, 3 * GiB

    def pick(self, waiting, growth, free, running, waited=0.0):
        return bg.gate_pick(waiting, growth, free, running, waited, reserve=GiB, patience=30)

    def test_starts_what_fits(self):
        self.assertEqual(self.pick([self.L] * 4, 0, GiB + 2 * self.L, 1), [0, 1])
        self.assertEqual(self.pick([self.L] * 4, self.L, GiB + 2 * self.L, 1), [0])    # a running one still grows
        self.assertEqual(self.pick([self.L] * 4, 0, GiB // 2, 3), [])                  # memory gone: none starts

    def test_never_none_running(self):
        self.assertEqual(self.pick([self.G, self.L], 0, GiB // 2, 0), [0])

    def test_lights_pass_a_giant_for_a_while(self):
        free = GiB + 2 * self.L
        self.assertEqual(self.pick([self.G, self.L, self.L], 0, free, 2, waited=5), [1, 2])
        self.assertEqual(self.pick([self.G, self.L, self.L], 0, free, 2, waited=31), [])
        self.assertEqual(self.pick([self.G, self.L], 0, GiB + self.G + self.L, 2, waited=31), [0, 1])

    @unittest.skipUnless(sys.platform == 'win32', 'the gate holds compiles on Windows')
    def test_holds_real_processes(self):
        # A stand-in for Ninja starts four stand-in compiles at once ("-c a.c" in their command lines); with memory for
        # one at a time the gate runs them one after another, with plenty all four together. Each notes when it ran
        # (start and end), so the check is their overlap, not how fast Python starts on a busy PC.
        import time
        with tempfile.TemporaryDirectory() as tmp:
            log, driver = Path(tmp) / 'log.txt', Path(tmp) / 'driver.py'
            child = ("import sys, time; t = time.monotonic(); time.sleep(1.0); "
                     "open(sys.argv[3], 'a').write(f'{t} {time.monotonic()}\\n')")
            driver.write_text('import subprocess, sys\n'
                              f'ps = [subprocess.Popen([sys.executable, "-c", {child!r}, "-c", "a.c", {str(log)!r}]) '
                              'for _ in range(4)]\n'
                              'sys.exit(max(p.wait() for p in ps))\n')
            saved = bg.MemoryGate.free
            try:
                for free, together in ((bg.GATE_RESERVE + GiB + GiB // 2, False), (64 * GiB, True)):
                    log.write_text('')
                    bg.MemoryGate.free = lambda self, f=free: f
                    t0 = time.monotonic()
                    code, dt, _, _ = bg.run_measured([sys.executable, str(driver)], dict(os.environ), {'a.c': GiB})
                    runs = sorted(tuple(map(float, ln.split())) for ln in log.read_text().splitlines() if ln.strip())
                    self.assertEqual((code, len(runs)), (0, 4))
                    if together:   # all four ran at the same moment
                        self.assertLess(max(r[0] for r in runs), min(r[1] for r in runs), runs)
                    else:          # one at a time: each starts after the one before has ended
                        for before, after in zip(runs, runs[1:]):
                            self.assertGreaterEqual(after[0], before[1], runs)
                    self.assertLess(time.monotonic() - t0, 40)
            finally:
                bg.MemoryGate.free = saved


if __name__ == '__main__':
    unittest.main(argv=[a for a in sys.argv if a != '-v'])
