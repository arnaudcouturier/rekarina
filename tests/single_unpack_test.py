"""The one-file Rekarina.exe (installer/launcher/rekarina_single.c) unpacks its zip by itself, and where that fails with
Windows' tar.exe: a stub compiled here, a zip of test files behind it (deflated at several levels, stored, empty, a
Cyrillic name, a path longer than 260 characters), run with --unpack-only --home <scratch> (no window, nothing outside
the scratch folder).

    python tests/single_unpack_test.py

Needs a clang for Windows: REKARINA_CLANG, the setup folder's own llvm\\bin\\clang.exe, or one on PATH; skipped
without one.
"""
import os
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VERSION = '9.9.9-test'
SYSTEM32 = Path(os.environ.get('SystemRoot', r'C:\Windows')) / 'System32'


def find_clang():
    for c in (os.environ.get('REKARINA_CLANG'), ROOT / 'llvm' / 'bin' / 'clang.exe', shutil.which('clang')):
        if c and Path(c).is_file():
            return str(c)
    return None


def long_name(p):
    """A path Python can open past 260 characters."""
    return '\\\\?\\' + str(Path(p).resolve())


@unittest.skipUnless(sys.platform == 'win32', 'the one-file program is for Windows')
class SingleUnpackTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        clang = find_clang()
        if not clang:
            raise unittest.SkipTest('no clang (REKARINA_CLANG)')
        cls.tmp = Path(tempfile.mkdtemp(prefix='rk-single-'))
        stub = cls.tmp / 'stub.exe'
        subprocess.run([clang, '-O2', '-mwindows', '-municode', str(ROOT / 'installer' / 'launcher' / 'rekarina_single.c'),
                        '-lole32', '-luuid', '-lshell32', '-o', str(stub)], check=True)
        cls.program = stub.read_bytes()
        text = b''.join(p.read_bytes() for p in sorted((ROOT / 'installer').glob('*.py')))
        cls.files = {
            'Rekarina-Launcher.exe': b'MZ' + os.urandom(3000),
            'rekarina-fork.txt': b'arnaudcouturier/rekarina\n',
            'installer/all.py': text,                                   # text: dynamic codes, long matches
            'bin/stub.exe': cls.program,                                # a program's bytes
            'data/random.bin': os.urandom(300_000),                     # what deflate stores as it is
            'data/zeros.bin': bytes(5_000_000),                         # one byte copied over itself
            'data/small.txt': b'hi',                                    # the fixed codes
            'data/empty.txt': b'',
            'data/stored.bin': os.urandom(1000) + text[:20000],
            'Папка/файл.txt': 'не только ASCII'.encode('utf-8'),        # a name Windows' tar.exe refuses
            'deep/' + '/'.join(['d' * 50] * 5) + '/long.txt': b'a path longer than 260 characters\n' * 10,
        }
        cls.exe, cls.infos = cls.make('Rekarina.exe', cls.files)
        cls.ascii_files = {k: v for k, v in cls.files.items() if k.isascii()}
        cls.ascii_exe, _ = cls.make('Rekarina-ascii.exe', cls.ascii_files)
        cls.n = 0

    @classmethod
    def make(cls, name, files):
        payload = cls.tmp / (name + '.zip')
        with zipfile.ZipFile(payload, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as z:
            for i, (member, data) in enumerate(files.items()):
                if member == 'data/stored.bin':
                    z.writestr(member, data, compress_type=zipfile.ZIP_STORED)
                else:
                    z.writestr(member, data, compresslevel=(1, 6, 9)[i % 3])
            infos = {i.filename: i for i in z.infolist()}
        zip_bytes = payload.read_bytes()
        exe = cls.tmp / name
        exe.write_bytes(cls.program + zip_bytes + b'RKSFX001' + len(cls.program).to_bytes(8, 'little') +
                        len(zip_bytes).to_bytes(8, 'little') + VERSION.encode().ljust(32, b'\0'))
        return exe, infos

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def run_exe(self, exe=None, tar=None, builtin=True):
        type(self).n += 1
        home = self.tmp / f'home{self.n}'
        env = dict(os.environ)
        env.pop('REKARINA_UNPACK_TAR', None)
        env.pop('REKARINA_UNPACK_BUILTIN', None)
        if tar is not None:
            env['REKARINA_UNPACK_TAR'] = str(tar)
        if not builtin:
            env['REKARINA_UNPACK_BUILTIN'] = 'off'
        t0 = time.monotonic()
        p = subprocess.run([str(exe or self.exe), '--unpack-only', '--home', str(home)], env=env, timeout=300)
        folder = home / 'setup' / VERSION
        log = (folder / 'unpack.log').read_text(encoding='utf-8') if (folder / 'unpack.log').is_file() else ''
        print(f'\n  ({time.monotonic() - t0:.1f} s) ' + log.strip().replace('\n', '\n  '))
        mark = folder / 'unpacked.txt'
        return p.returncode, folder, log, mark.read_text(encoding='utf-8').strip() if mark.is_file() else None

    def check_files(self, folder, files):
        for name, data in files.items():
            path = long_name(folder / name.replace('/', '\\'))
            with open(path, 'rb') as f:
                self.assertEqual(f.read(), data, name)
            y, mo, d, h, mi, s = self.infos[name].date_time    # the zip's time, local
            self.assertLess(abs(os.stat(path).st_mtime - time.mktime((y, mo, d, h, mi, s, 0, 0, -1))), 3, name)

    def test_builtin_first(self):
        code, folder, log, mark = self.run_exe()
        self.assertEqual(code, 0, log)
        self.check_files(folder, self.files)
        self.assertEqual(mark, 'the built-in unpacker')
        self.assertIn(f'the built-in unpacker: {len(self.files)} files', log)
        self.assertNotIn('tar.exe', log)

    def test_tar_takes_over(self):
        if not (SYSTEM32 / 'tar.exe').is_file():
            self.skipTest('no tar.exe in this Windows')
        code, folder, log, mark = self.run_exe(self.ascii_exe, builtin=False)
        self.assertEqual(code, 0, log)
        self.check_files(folder, self.ascii_files)
        self.assertIn('tar.exe: done', log)
        self.assertEqual(mark, 'tar.exe (the built-in unpacker: not tried (REKARINA_UNPACK_BUILTIN=off))')

    def test_both_fail(self):
        code, folder, log, mark = self.run_exe(tar=self.tmp / 'missing' / 'tar.exe', builtin=False)
        self.assertEqual(code, 1)
        self.assertIsNone(mark)
        self.assertIn('the built-in unpacker failed: not tried', log)
        self.assertIn('missing\\tar.exe is not there', log)
        code, folder, log, mark = self.run_exe(tar=SYSTEM32 / 'where.exe', builtin=False)   # exits 2 on tar's options
        self.assertEqual(code, 1)
        self.assertIn('where.exe ended with code 2', log)

    def damaged(self, name):
        """A copy of the program with one byte of name's packed data changed."""
        info = self.infos[name]
        data = bytearray(self.exe.read_bytes())
        local = len(self.program) + info.header_offset
        at = local + 30 + int.from_bytes(data[local + 26:local + 28], 'little') + int.from_bytes(data[local + 28:local + 30], 'little')
        data[at + info.compress_size // 2] ^= 0x5A
        exe = self.tmp / f'damaged{self.n}.exe'
        exe.write_bytes(bytes(data))
        return exe

    def test_damaged_stored(self):
        code, folder, log, mark = self.run_exe(self.damaged('data/stored.bin'), tar='off')
        self.assertEqual(code, 1)
        self.assertIn('data\\stored.bin: its CRC-32 differs', log)
        self.assertIsNone(mark)

    def test_damaged_deflated(self):
        """Windows' tar.exe, where there is one, does not take a damaged zip either."""
        code, folder, log, mark = self.run_exe(self.damaged('installer/all.py'))
        self.assertEqual(code, 1)
        self.assertRegex(log, r'installer\\all\.py: (its packed data is damaged|its CRC-32 differs)')
        self.assertIsNone(mark)


if __name__ == '__main__':
    unittest.main()
