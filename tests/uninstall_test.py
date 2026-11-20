"""installer/uninstall.ps1 on scratch installs: a game folder, setup folders and the application data made up in a
temporary folder (the environment points LOCALAPPDATA and APPDATA there), a Start menu and a desktop of their own and a
test registry key, so nothing of the player's or of a real install is ever touched. Windows only (PowerShell, the
registry, shortcuts and junctions).

Run: python tests/uninstall_test.py
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
SCRIPT = ROOT / 'installer' / 'uninstall.ps1'
WIN = os.name == 'nt'
if WIN:
    import winreg
    from installer import oot


def put(path, data=b'x' * 100):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return path


@unittest.skipUnless(WIN, 'Windows only')
class UninstallTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix='rk-uninstall-'))
        t = self.tmp
        self.local, self.roam = t / 'Local', t / 'Roaming'
        self.home = t / 'Rekarina home'
        self.game = self.home / 'game [1]'                      # brackets: every path must be taken literally
        self.setup = self.home / 'setup' / '1.2.4'
        self.old_setup = self.home / 'setup' / '1.2.3'
        self.foreign_setup = self.home / 'setup' / 'notes'      # no marks: stays
        self.outside = t / 'outside'                             # a junction in the game folder points here
        self.start, self.desk = t / 'Start', t / 'Desktop'
        self.key = rf'Software\RekarinaTest\Uninstall\Rekarina-{os.getpid()}'
        # the game folder
        for name in ('rekarina-install.json', 'Rekarina.exe', 'rekarina.ini', 'shader-cache/shader-cache.pack',
                     'logs/rekarina.log', 'mods/a mod/romfs/x.bin', 'exefs/main'):
            put(self.game / name)
        self.saves = [put(self.game / 'profile/save/fb54/progress.sav', b's' * 5000), put(self.game / 'profile/account.uid'),
                      put(self.game / 'profile/save-tx/work/option.sav')]
        self.dump = put(self.game / 'dump/the game.nup', b'n' * 300)   # a player's dump inside the folder: stays
        put(self.outside / 'keep.txt')
        subprocess.run(['cmd', '/c', 'mklink', '/J', str(self.game / 'linked'), str(self.outside)], check=True,
                       capture_output=True)
        # the setup folders, one with its build's working files
        for s in (self.setup, self.old_setup):
            put(s / 'unpacked.txt', b'1')
            put(s / 'Rekarina-Launcher.exe')
            put(s / 'installer/oot.py')
        put(self.setup / 'temp/oot-abc/obj/main.o', b'o' * 4000)
        put(self.foreign_setup / 'my notes.txt')
        # the application data
        self.local_rk = self.local / 'Rekarina'
        put(self.local_rk / 'home.txt', str(self.home).encode('utf-16-le'))
        put(self.local_rk / 'logs/install-1.log')
        self.roam_rk = self.roam / 'Rekarina'
        for name in ('launcher.json', 'update.json', 'timings.json'):
            put(self.roam_rk / name)
        put(self.roam / 'Other App/settings.json')
        # shortcuts: ours in the Start menu (to the launcher), a foreign one on the desktop with our name
        self.start.mkdir(parents=True)
        self.desk.mkdir(parents=True)
        self.start_link = self.start / 'Rekarina.lnk'
        self.desk_link = self.desk / 'Rekarina Launcher.lnk'
        ok, said = oot.write_shortcut(self.start_link, self.setup / 'Rekarina-Launcher.exe', self.setup, 'test')
        self.assertTrue(ok, said)
        ok, said = oot.write_shortcut(self.desk_link, self.outside / 'keep.txt', self.outside, 'test')
        self.assertTrue(ok, said)
        with winreg.CreateKey(winreg.HKEY_CURRENT_USER, self.key) as k:
            winreg.SetValueEx(k, 'DisplayName', 0, winreg.REG_SZ, 'Rekarina')

    def tearDown(self):
        try:
            winreg.DeleteKey(winreg.HKEY_CURRENT_USER, self.key)
        except OSError:
            pass
        link = self.game / 'linked'
        if link.exists():
            os.rmdir(link)   # the junction alone, before the tree goes
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_ps(self, *args, game=None):
        env = dict(os.environ, LOCALAPPDATA=str(self.local), APPDATA=str(self.roam))
        cmd = ['powershell', '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', str(SCRIPT),
               '-GameDir', str(game or self.game), '-StartMenu', str(self.start), '-Desktop', str(self.desk),
               '-RegKey', 'HKCU:\\' + self.key, *args]
        return subprocess.run(cmd, env=env, capture_output=True, text=True, encoding='utf-8', timeout=120)

    def key_exists(self):
        try:
            winreg.OpenKey(winreg.HKEY_CURRENT_USER, self.key).Close()
            return True
        except OSError:
            return False

    def test_list_names_everything_ours_and_nothing_else(self):
        r = self.run_ps('-List')
        self.assertEqual(r.returncode, 0, r.stderr)
        plan = json.loads(r.stdout)
        kinds = {Path(i['path']): i['kind'] for i in plan['items']}
        self.assertEqual(kinds, {self.game: 'game', self.setup: 'setup', self.old_setup: 'setup',
                                 self.local_rk: 'appdata', self.roam_rk: 'appdata'})
        game_bytes = next(i['bytes'] for i in plan['items'] if i['kind'] == 'game')
        self.assertEqual(game_bytes, 7 * 100)   # the saves, the dump and what the junction points to are not counted
        self.assertEqual(plan['saves_bytes'], 5000 + 100 + 100)
        self.assertEqual(Path(plan['saves']), self.game / 'profile')
        shortcuts = plan['shortcuts'] if isinstance(plan['shortcuts'], list) else [plan['shortcuts']]
        self.assertEqual([Path(p) for p in shortcuts], [self.start_link])
        refused = {Path(x['path']) for x in plan['refused']}
        self.assertIn(self.foreign_setup, refused)
        self.assertIn(self.desk_link, refused)
        dumps = plan['dumps_kept'] if isinstance(plan['dumps_kept'], list) else [plan['dumps_kept']]
        self.assertEqual([Path(p) for p in dumps], [self.dump])
        self.assertTrue(plan['registry'])
        self.assertTrue(self.game.is_dir() and self.setup.is_dir() and self.key_exists())   # -List changes nothing

    def test_keep_saves_removes_everything_ours_but_the_saves(self):
        r = self.run_ps('-Yes')
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn('saves stay', r.stdout)
        for s in self.saves:
            self.assertTrue(s.is_file(), s)
        self.assertTrue(self.dump.is_file())
        left = sorted(str(p.relative_to(self.game)) for p in self.game.rglob('*'))
        self.assertEqual(left, sorted(['profile', 'profile\\save', 'profile\\save\\fb54', 'profile\\save\\fb54\\progress.sav',
                                       'profile\\account.uid', 'profile\\save-tx', 'profile\\save-tx\\work',
                                       'profile\\save-tx\\work\\option.sav', 'dump', 'dump\\the game.nup']))
        self.assertTrue((self.outside / 'keep.txt').is_file())        # what the junction pointed to
        self.assertFalse(self.setup.exists())
        self.assertFalse(self.old_setup.exists())
        self.assertTrue((self.foreign_setup / 'my notes.txt').is_file())
        self.assertFalse(self.local_rk.exists())
        self.assertFalse(self.roam_rk.exists())
        self.assertTrue((self.roam / 'Other App/settings.json').is_file())
        self.assertFalse(self.start_link.exists())
        self.assertTrue(self.desk_link.exists())
        self.assertFalse(self.key_exists())

    def test_delete_everything_takes_the_saves_too_but_not_the_dump(self):
        r = self.run_ps('-Yes', '-KeepSaves', 'no')
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertFalse((self.game / 'profile').exists())
        self.assertTrue(self.dump.is_file())
        self.assertEqual(sorted(p.name for p in self.game.iterdir()), ['dump'])

    def test_a_folder_without_the_marks_is_refused(self):
        foreign = self.tmp / 'Some Game'
        put(foreign / 'Rekarina.exe')   # a name alone is not a mark: no rekarina-install.json
        put(foreign / 'profile/save.sav')
        r = self.run_ps('-Yes', game=foreign)
        self.assertTrue((foreign / 'Rekarina.exe').is_file())
        self.assertTrue((foreign / 'profile/save.sav').is_file())
        plan = json.loads(self.run_ps('-List', game=foreign).stdout)
        self.assertNotIn(str(foreign), [i['path'] for i in plan['items']])
        self.assertIn(str(foreign), [x['path'] for x in plan['refused']])
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)   # the rest of ours still goes

    def test_the_launchers_way_waits_for_the_launcher(self):
        """installer/uninstall.py start, as the launcher's Keep my saves plate calls it: the script waits for the
        launcher's process (here a child that lives two seconds), then removes everything ours but the saves."""
        from installer import uninstall
        waiter = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(2)'])
        env = dict(os.environ, LOCALAPPDATA=str(self.local), APPDATA=str(self.roam))
        proc = uninstall.start(self.game, True, wait_pid=waiter.pid, env=env,
                               extra=('-Quiet', '-StartMenu', str(self.start), '-Desktop', str(self.desk),
                                      '-RegKey', 'HKCU:\\' + self.key))
        self.assertTrue(self.setup.is_dir())   # nothing goes while the launcher still runs
        self.assertEqual(proc.wait(timeout=120), 0)
        self.assertIsNotNone(waiter.poll())
        self.assertFalse(self.setup.exists())
        self.assertFalse(self.local_rk.exists())
        self.assertFalse(self.key_exists())
        for s in self.saves:
            self.assertTrue(s.is_file(), s)

    def test_a_zip_setup_folder_is_found_through_its_shortcut(self):
        """A setup unpacked from the zip release lies anywhere: the Start menu's shortcut to its launcher names it, and
        it goes under the setup guard (Rekarina-Launcher.exe with installer\\oot.py; no unpacked.txt)."""
        zipped = self.tmp / 'Downloads' / 'Rekarina-Setup-1.2.5-windows'
        put(zipped / 'Rekarina-Launcher.exe')
        put(zipped / 'installer/oot.py')
        put(zipped / 'temp/oot-x/obj/a.o', b'o' * 3000)
        ok, said = oot.write_shortcut(self.start_link, zipped / 'Rekarina-Launcher.exe', zipped, 'test')
        self.assertTrue(ok, said)
        plan = json.loads(self.run_ps('-List').stdout)
        self.assertIn(str(zipped), [i['path'] for i in plan['items']])
        r = self.run_ps('-Yes')
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertFalse(zipped.exists())
        self.assertTrue(zipped.parent.is_dir())   # the folder it lay in is not ours
        self.assertFalse(self.start_link.exists())

    def test_a_drive_root_is_never_ours(self):
        plan = json.loads(self.run_ps('-List', game='C:\\').stdout)
        self.assertNotIn('C:', [i['path'] for i in plan['items']])
        self.assertNotIn('C:\\', [i['path'] for i in plan['items']])


@unittest.skipUnless(WIN, 'Windows only')
class InstallTest(unittest.TestCase):
    """installer/uninstall.py: what a setup run leaves for the uninstaller in the game folder, the "Apps & features"
    entry (a test key here), and the game folder's own copy working by itself (it takes its own folder)."""

    def setUp(self):
        from installer import uninstall
        self.u = uninstall
        self.tmp = Path(tempfile.mkdtemp(prefix='rk-uninstall-install-'))
        self.game = self.tmp / 'game'
        self.setup = self.tmp / 'setup' / '1.2.5'
        for name in ('rekarina-install.json', 'Rekarina.exe', 'profile/save/a.sav'):
            put(self.game / name)
        put(self.setup / 'unpacked.txt', b'1' * 2048)
        self.key = rf'Software\RekarinaTest\Uninstall\Rekarina-{os.getpid()}'
        # a Start menu and a desktop of the test's own: the real ones may hold a player's shortcuts
        self.own = ('-StartMenu', str(self.tmp / 'Start'), '-Desktop', str(self.tmp / 'Desktop'))

    def tearDown(self):
        try:
            winreg.DeleteKey(winreg.HKEY_CURRENT_USER, self.key)
        except OSError:
            pass
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_install_writes_script_shortcut_and_entry(self):
        lines = []
        done = self.u.install(self.game, '1.2.5', setup=self.setup, key=self.key, log=lines.append)
        self.assertEqual(done, [self.u.GAME_SCRIPT, self.u.LINK, 'registry'], lines)
        script = self.game / self.u.GAME_SCRIPT
        self.assertEqual(script.read_bytes(), self.u.SCRIPT.read_bytes())
        self.assertTrue(script.read_bytes().startswith(b'\xef\xbb\xbf'))   # read as UTF-8 by Windows PowerShell
        read = subprocess.run(['powershell', '-NoProfile', '-NonInteractive', '-Command',
                               '$s = (New-Object -ComObject WScript.Shell).CreateShortcut($env:L); '
                               '$s.TargetPath; $s.Arguments'], env=dict(os.environ, L=str(self.game / self.u.LINK)),
                              capture_output=True, text=True, timeout=60)
        target, args = [x for x in read.stdout.split('\n') if x.strip()]
        self.assertTrue(target.lower().endswith('powershell.exe'))
        self.assertIn(str(script), args)
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, self.key) as k:
            get = lambda name: winreg.QueryValueEx(k, name)[0]   # noqa: E731
            self.assertEqual(get('DisplayName'), 'Rekarina')
            self.assertEqual(get('DisplayVersion'), '1.2.5')
            self.assertEqual(get('Publisher'), 'Rekarina')
            self.assertEqual(get('DisplayIcon'), str(self.game / 'Rekarina.exe'))
            self.assertIn(str(script), get('UninstallString'))
            self.assertGreaterEqual(get('EstimatedSize'), 2)   # KiB of the game and the setup folder
            self.assertEqual(get('NoModify'), 1)

    def test_the_game_folders_copy_works_alone(self):
        self.u.install(self.game, '1.2.5', setup=self.setup, key=self.key)
        env = dict(os.environ, LOCALAPPDATA=str(self.tmp / 'Local'), APPDATA=str(self.tmp / 'Roaming'))
        r = subprocess.run(['powershell', '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File',
                            str(self.game / self.u.GAME_SCRIPT), '-List', '-RegKey', 'HKCU:\\' + self.key, *self.own],
                           env=env, capture_output=True, text=True, encoding='utf-8', timeout=120)
        self.assertEqual(r.returncode, 0, r.stderr)
        plan = json.loads(r.stdout)
        self.assertEqual(Path(plan['game']), self.game)   # no -GameDir: the script's own folder
        self.assertIn(str(self.game), [i['path'] for i in self.u.as_list(plan['items'])])
        self.assertTrue(plan['registry'])

    def test_plan_from_the_launcher(self):
        env = dict(os.environ, LOCALAPPDATA=str(self.tmp / 'Local'), APPDATA=str(self.tmp / 'Roaming'))
        p = self.u.plan(self.game, env=env, extra=('-RegKey', 'HKCU:\\' + self.key, *self.own))
        self.assertEqual([Path(i['path']) for i in p['items']], [self.game])
        self.assertEqual(p['shortcuts'], [])
        self.assertGreater(p['saves_bytes'], 0)


if __name__ == '__main__':
    unittest.main()
