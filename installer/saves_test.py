"""Tests of installer/saves.py on made-up game folders and emulator folders. Standard library only.

    python -m unittest installer.saves_test
"""
import json
import os
import tempfile
import unittest
import zipfile
from pathlib import Path

from . import saves

UID = bytes(range(1, 17))
UID_NAME = '100f0e0d0c0b0a090807060504030201'


def put(path, data=b'x'):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def make_save(root, slots=(0,), mark=b'a'):
    put(root / 'account.sav', mark * 4)
    put(root / 'option.sav', mark * 8)
    for n in slots:
        put(root / f'slot_{n:02d}' / 'progress.sav', mark * 16)
        put(root / f'slot_{n:02d}' / 'caption.sav', mark * 2)
    return root


class Sandbox(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.top = Path(self.tmp.name)
        self.dest = self.top / 'game'
        self.profile = self.dest / 'profile'
        put(self.profile / 'account.uid', UID)
        self.own = self.profile / 'save' / UID_NAME

    def tearDown(self):
        self.tmp.cleanup()

    def mark(self, root=None):
        return ((root or self.own) / 'slot_00' / 'progress.sav').read_bytes()[:1]


class What(Sandbox):
    def test_a_save_is_a_folder_with_a_slot(self):
        s = make_save(self.top / 's', slots=(0, 3))
        put(self.top / 'other' / 'slot_00' / 'caption.sav')
        put(self.top / 'named' / 'slot_x' / 'progress.sav')
        self.assertTrue(saves.is_save(s))
        self.assertFalse(saves.is_save(self.top / 'other'))
        self.assertFalse(saves.is_save(self.top / 'named'))
        self.assertFalse(saves.is_save(self.top / 'missing'))
        d = saves.describe(s, saves.EMULATOR, 'yuzu')
        self.assertEqual((d.slots, d.files, d.size, d.name), (2, 6, 4 + 8 + 2 * 18, 'yuzu'))

    def test_find_root_looks_below_the_chosen_folder(self):
        s = make_save(self.top / 'export' / 'Some Title' / 'user' / '0')
        self.assertEqual(saves.find_root(self.top / 'export'), s)
        self.assertEqual(saves.find_root(s), s)
        self.assertIsNone(saves.find_root(self.top / 'export', depth=1))
        self.assertIsNone(saves.find_root(self.dest))

    def test_account_names_the_folder_as_the_game_does(self):
        self.assertEqual(saves.account(self.dest), UID_NAME)
        other = self.top / 'fresh'
        self.assertIsNone(saves.account(other))
        name = saves.account(other, create=True)
        raw = (other / 'profile' / 'account.uid').read_bytes()
        self.assertEqual((len(raw), name), (16, raw[8:][::-1].hex() + raw[:8][::-1].hex()))
        self.assertEqual(saves.account(other), name)
        put(other / 'profile' / 'account.uid', b'short')
        with self.assertRaises(saves.SaveError) as e:
            saves.account(other)
        self.assertEqual(e.exception.code, 'bad_account')


class Places(Sandbox):
    def env(self):
        return {'APPDATA': str(self.top / 'Roaming'), 'LOCALAPPDATA': str(self.top / 'Local'),
                'XDG_CONFIG_HOME': str(self.top / 'config'), 'XDG_DATA_HOME': str(self.top / 'share')}

    def test_emulators_and_ports(self):
        home = self.top / 'home'
        ryu = make_save(self.top / 'Roaming' / 'Ryujinx' / 'bis' / 'user' / 'save' / '0000000000000007' / '0')
        make_save(self.top / 'Roaming' / 'Ryujinx' / 'bis' / 'user' / 'save' / '0000000000000008' / '0', slots=())
        yuzu = make_save(self.top / 'share' / 'suyu' / 'nand' / 'user' / 'save' / '0000000000000000' / 'ABCD' / saves.TITLE)
        make_save(self.top / 'share' / 'suyu' / 'nand' / 'user' / 'save' / '0000000000000000' / 'ABCD' / '0100000000000000')
        port = make_save(home / 'Games' / 'Hookshot' / 'user' / 'save' / ('5a' * 16))
        built = make_save(self.top / 'elsewhere' / 'user' / 'save' / ('11' * 16))
        put(self.top / 'Local' / 'LongshotLauncher' / 'settings.json',
            json.dumps({'install': {'dir': str(self.top / 'elsewhere')}, 'n': 3, 'gone': str(self.top / 'nowhere')}).encode())
        os.utime(yuzu / 'slot_00' / 'progress.sav', (2_000_000_000, 2_000_000_000))
        got = saves.candidates(self.env(), home)
        self.assertEqual({(s.source, s.name, s.path) for s in got},
                         {(saves.EMULATOR, 'Ryujinx', ryu), (saves.EMULATOR, 'suyu', yuzu),
                          (saves.PORT, 'Hookshot', port), (saves.PORT, 'Longshot', built)})
        self.assertEqual(got[0].path, yuzu)   # newest first

    def test_nothing_found_is_an_empty_list(self):
        self.assertEqual(saves.candidates(self.env(), self.top / 'home'), [])


class Import(Sandbox):
    def test_first_import_needs_no_backup(self):
        src = make_save(self.top / 'src', mark=b'n')
        self.assertIsNone(saves.current(self.dest))
        self.assertIsNone(saves.import_save(self.dest, src))
        self.assertEqual((self.mark(), saves.current(self.dest).slots), (b'n', 1))
        self.assertEqual(self.mark(src), b'n')   # the source stays
        self.assertEqual(saves.backups(self.dest), [])

    def test_import_into_a_new_game_folder_makes_the_account(self):
        fresh = self.top / 'fresh'
        saves.import_save(fresh, make_save(self.top / 'src', mark=b'n'))
        self.assertEqual(saves.current(fresh).slots, 1)
        self.assertEqual(len((fresh / 'profile' / 'account.uid').read_bytes()), 16)

    def test_the_replaced_save_and_its_pending_commit_are_kept(self):
        make_save(self.own, mark=b'o')
        put(self.profile / 'save-tx' / 'ready-4' / 'slot_00' / 'progress.sav', b'newer')
        backup = saves.import_save(self.dest, make_save(self.top / 'src', slots=(0, 1), mark=b'n'))
        self.assertEqual(self.mark(), b'n')
        self.assertEqual(self.mark(backup / 'save'), b'o')
        self.assertEqual((backup / 'save-tx' / 'ready-4' / 'slot_00' / 'progress.sav').read_bytes(), b'newer')
        self.assertFalse((self.profile / 'save-tx').exists())   # the game would load a commit left there
        self.assertEqual(saves.backups(self.dest), [backup])

    def test_restore_swaps_the_two(self):
        make_save(self.own, mark=b'o')
        backup = saves.import_save(self.dest, make_save(self.top / 'src', mark=b'n'))
        saves.restore(self.dest, backup)
        self.assertEqual(self.mark(), b'o')
        (again,) = saves.backups(self.dest)
        self.assertEqual(self.mark(again / 'save'), b'n')
        saves.restore(self.dest, again)
        self.assertEqual(self.mark(), b'n')

    def test_refusals_change_nothing(self):
        make_save(self.own, mark=b'o')
        for source, code in ((self.top / 'empty', 'not_a_save'), (self.own, 'same')):
            source.mkdir(exist_ok=True)
            with self.assertRaises(saves.SaveError) as e:
                saves.import_save(self.dest, source)
            self.assertEqual(e.exception.code, code)
        big = make_save(self.top / 'big')
        put(big / 'blob.bin', b'\0' * (saves.MAX_BYTES + 1))
        with self.assertRaises(saves.SaveError) as e:
            saves.import_save(self.dest, big)
        self.assertEqual(e.exception.code, 'too_large')
        with self.assertRaises(saves.SaveError) as e:
            saves.restore(self.dest, self.top / 'empty')
        self.assertEqual(e.exception.code, 'no_backup')
        self.assertEqual((self.mark(), saves.backups(self.dest)), (b'o', []))

    def test_zip_of_an_export(self):
        src = make_save(self.top / 'export' / 'Game' / 'save', mark=b'z')
        z = self.top / 'save.zip'
        with zipfile.ZipFile(z, 'w') as f:
            for p in src.rglob('*'):
                if p.is_file():
                    f.write(p, p.relative_to(self.top / 'export'))
        saves.import_zip(self.dest, z)
        self.assertEqual(self.mark(), b'z')

    def test_zip_refusals(self):
        bad = self.top / 'bad.zip'
        with zipfile.ZipFile(bad, 'w') as f:
            f.writestr('../outside/slot_00/progress.sav', b'x')
        plain = self.top / 'plain.zip'
        with zipfile.ZipFile(plain, 'w') as f:
            f.writestr('readme.txt', b'x')
        put(self.top / 'not.zip', b'not an archive')
        for z, code in ((bad, 'bad_archive'), (plain, 'not_a_save'), (self.top / 'not.zip', 'bad_archive')):
            with self.assertRaises(saves.SaveError) as e:
                saves.import_zip(self.dest, z)
            self.assertEqual(e.exception.code, code)
        self.assertFalse((self.top / 'outside').exists())


if __name__ == '__main__':
    unittest.main()
