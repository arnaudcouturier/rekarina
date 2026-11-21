"""tools/nup_damage.py: which files of a .nup are damaged, and where. Game-free: a small .nup packed here by the
vendored reference packer (a romfs of stand-in files), then damaged the two ways a player's dump can be.

    python tests/nup_damage_test.py
"""
import io
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import nup_damage   # noqa: E402
import oot_romfs    # noqa: E402
from nup.pack import pack   # noqa: E402  (tools/vendor/nup, on the path through oot_romfs)

zstandard = oot_romfs.zstandard


def frame(data):
    """A zstd frame with its checksum, as the game's files carry one (a damaged frame without one may unpack wrong)."""
    try:
        return zstandard.ZstdCompressor(write_checksum=True).compress(data)
    except (AttributeError, TypeError):
        return None


class NupDamageTest(unittest.TestCase):
    def setUp(self):
        if frame(b'x') is None:
            self.skipTest('no zstd compressor')
        self.tmp = tempfile.TemporaryDirectory()
        d = Path(self.tmp.name)
        romfs = d / 'src' / 'romfs'
        self.files = {
            'Shader/good.bfsha.zs': frame(os.urandom(4000) + bytes(20000)),
            'Shader/bad.bfsha.zs': bytearray(frame(b'shader ' * 5000)),
            'Shader/copy.bfsha.zs': frame(os.urandom(70000)),
            'Model/other.bin': os.urandom(5000),
        }
        bad = self.files['Shader/bad.bfsha.zs']
        bad[len(bad) // 2] ^= 0xFF   # damaged before the .nup was made: its SHA-256 matches, its zstd data does not
        for name, data in self.files.items():
            p = romfs / name
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(bytes(data))
        self.nup = d / 'game.nup'
        pack(str(self.nup), str(d / 'src'), base_title_id=0x0400486024DEC000)

    def tearDown(self):
        self.tmp.cleanup()

    def damage_copy(self, name):
        """One byte of name's stored data changed in the .nup itself, as a damaged copy has it."""
        fs = oot_romfs.RomFS(str(self.nup))
        _n, _size, _sha, eoff, esz, _codec = fs.r.files[fs.index[name]]
        fs.r.close()
        with open(self.nup, 'r+b') as f:
            f.seek(eoff + esz // 2)
            b = f.read(1)
            f.seek(eoff + esz // 2)
            f.write(bytes([b[0] ^ 0x5A]))
        return eoff

    def test_both_kinds(self):
        offset = self.damage_copy('Shader/copy.bfsha.zs')
        found = {name: (at, why) for name, at, _n, why in nup_damage.check(self.nup, out=lambda *a, **k: None)}
        self.assertEqual(set(found), {'Shader/bad.bfsha.zs', 'Shader/copy.bfsha.zs'})
        self.assertIn('the dump was made from damaged data', found['Shader/bad.bfsha.zs'][1])
        self.assertEqual(found['Shader/copy.bfsha.zs'][0], offset)
        self.assertIn('copy of the .nup is damaged', found['Shader/copy.bfsha.zs'][1])

    def test_scope(self):
        """--shaders reads the shader files alone; --all every file."""
        self.damage_copy('Model/other.bin')
        quiet = lambda *a, **k: None   # noqa: E731
        self.assertNotIn('Model/other.bin', [d[0] for d in nup_damage.check(self.nup, 'shaders', out=quiet)])
        self.assertIn('Model/other.bin', [d[0] for d in nup_damage.check(self.nup, 'all', out=quiet)])

    def test_command_line(self):
        self.damage_copy('Shader/copy.bfsha.zs')
        p = subprocess.run([sys.executable, str(ROOT / 'tools' / 'nup_damage.py'), str(self.nup)], capture_output=True,
                           text=True, encoding='utf-8')
        self.assertEqual(p.returncode, 3, p.stdout + p.stderr)
        lines = [ln for ln in p.stdout.splitlines() if ln.startswith('damaged: ')]
        self.assertEqual(len(lines), 2, p.stdout)
        self.assertRegex(p.stdout, r'damaged: Shader/copy\.bfsha\.zs at byte \d+ of the \.nup \(\d+ bytes there\)')

    def setup_ctx(self, last_line):
        """A setup's context for installer/oot.py dump_damage, after a tool whose log ends in last_line."""
        sys.path.insert(0, str(ROOT))
        from installer.events import Reporter
        temp = Path(self.tmp.name) / 'temp'
        temp.mkdir(exist_ok=True)
        log = temp / 'tool.log'
        log.write_text('Traceback (most recent call last):\n  ...\n' + last_line + '\n', encoding='utf-8')
        ctx = SimpleNamespace(layout=SimpleNamespace(tools=ROOT / 'tools', python=Path(sys.executable), root=ROOT),
                              temp=temp, nup=self.nup, reporter=Reporter(out=io.StringIO()))
        self.addCleanup(ctx.reporter.close)
        return ctx, log

    def test_the_setup_names_the_file(self):
        """The shader step's failure as the player had it: the setup's message names the file and its place."""
        from installer import oot
        from installer.runner import InstallError
        offset = self.damage_copy('Shader/copy.bfsha.zs')
        ctx, log = self.setup_ctx('compression.zstd.ZstdError: Unable to decompress Zstandard data: Data corruption '
                                  'detected')
        with self.assertRaises(InstallError) as e:
            oot.dump_damage(ctx, 'oot_prepare', log, '--shaders')
        self.assertEqual(e.exception.id, 'check.input_damaged')
        detail = e.exception.params['detail']
        self.assertIn(f'Shader/copy.bfsha.zs at byte {offset} of the .nup', detail)
        self.assertIn('Shader/bad.bfsha.zs', detail)
        self.assertEqual(e.exception.params['path'], str(self.nup))

    def test_other_failures_are_left_alone(self):
        """Another error, or a dump with nothing damaged: the step raises its own error as before."""
        from installer import oot
        ctx, log = self.setup_ctx('MemoryError')
        oot.dump_damage(ctx, 'oot_prepare', log, '--shaders')   # (not run: the last line is no unpacking error)
        ctx, log = self.setup_ctx('compression.zstd.ZstdError: Data corruption detected')
        good = Path(self.tmp.name) / 'good'
        romfs = good / 'romfs' / 'Shader'
        romfs.mkdir(parents=True)
        (romfs / 'a.bfsha.zs').write_bytes(frame(b'fine ' * 1000))
        ctx.nup = Path(self.tmp.name) / 'good.nup'
        pack(str(ctx.nup), str(good), base_title_id=0x0400486024DEC000)
        oot.dump_damage(ctx, 'oot_prepare', log, '--shaders')


class NoDamageTest(unittest.TestCase):
    def test_a_whole_dump(self):
        """A whole dump, named by REKARINA_NUP_CHECK: nothing damaged in it (no false alarm)."""
        nup = os.environ.get('REKARINA_NUP_CHECK')
        if not nup:
            self.skipTest('REKARINA_NUP_CHECK names no dump')
        out = io.StringIO()
        self.assertEqual(nup_damage.check(nup, out=lambda *a, **k: print(*a, file=out)), [])


if __name__ == '__main__':
    unittest.main()
