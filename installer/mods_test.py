"""Tests of installer/mods.py on made-up game folders. Standard library only.

    python -m unittest installer.mods_test
"""
import tempfile
import unittest
import zipfile
from pathlib import Path

from . import mods


def put(path, data=b'x'):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


class Sandbox(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dest = Path(self.tmp.name) / 'game'
        (self.dest / 'mods').mkdir(parents=True)
        (self.dest / 'rekarina.ini').write_bytes(b'[display]\r\nfullscreen = 1\r\n')

    def tearDown(self):
        self.tmp.cleanup()

    def mod(self, name, *files):
        for f in files or ('romfs/a.bin',):
            put(self.dest / 'mods' / name / f)
        return self.dest / 'mods' / name


class Scan(Sandbox):
    def test_kinds_and_counts(self):
        self.mod('Pack', 'romfs/a.bin', 'romfs/sub/b.bin')
        self.mod('Patch', 'exefs/E0F3CCFA.ips', 'atmosphere/exefs_patches/other/1234.ips')
        self.mod('Cheats', 'cheats/E0F3CCFAF50065AE.txt', 'cheats/readme.md')
        self.mod('Atmos', f'atmosphere/contents/{mods.TITLE}/romfs/a.bin', f'contents/{mods.TITLE}/cheats/x.txt')
        got = {m.name: m for m in mods.scan(self.dest)}
        self.assertEqual((got['Pack'].kinds, got['Pack'].files), ((mods.FILES,), 2))
        self.assertEqual((got['Patch'].kinds, got['Patch'].patches), ((mods.PATCH,), 2))
        self.assertEqual((got['Cheats'].kinds, got['Cheats'].cheats), ((mods.CHEATS,), 1))
        self.assertEqual(got['Atmos'].kinds, (mods.FILES, mods.CHEATS))
        self.assertTrue(all(m.enabled and not m.problem for m in got.values()))

    def test_problems(self):
        self.mod('Plugin', 'exefs/subsdk9', 'romfs/a.bin')
        self.mod('Main', 'exefs/main')
        (self.dest / 'mods' / 'Nothing').mkdir()
        (self.dest / 'mods' / '.hidden').mkdir()
        got = {m.name: m.problem for m in mods.scan(self.dest)}
        self.assertEqual(got, {'Plugin': mods.PLUGIN, 'Main': mods.PLUGIN, 'Nothing': mods.EMPTY})

    def test_title_folder_of_another_game_is_not_found(self):
        self.mod('Other', 'atmosphere/contents/0100000000000000/romfs/a.bin')
        self.assertEqual(mods.scan(self.dest)[0].problem, mods.EMPTY)

    def test_no_folder(self):
        self.assertEqual(mods.scan(self.dest / 'nowhere'), [])


class Order(Sandbox):
    def test_named_first_then_by_name(self):
        for n in ('b', 'A', 'c', 'd'):
            self.mod(n)
        (self.dest / 'rekarina.ini').write_text('mods_order = d|c\nmods_disabled = A\n', encoding='utf-8')
        got = mods.scan(self.dest)
        self.assertEqual([m.name for m in got], ['d', 'c', 'A', 'b'])
        self.assertEqual([m.enabled for m in got], [True, True, False, True])

    def test_switch_and_move_write_the_settings_and_keep_the_rest(self):
        for n in ('one', 'two', 'three'):
            self.mod(n)
        mods.set_enabled(self.dest, 'two', False)
        mods.move(self.dest, 'two', -1)
        text = (self.dest / 'rekarina.ini').read_bytes()
        self.assertIn(b'[display]\r\nfullscreen = 1\r\n', text)
        self.assertIn(b'[mods]\r\n', text)
        self.assertIn(b'mods_order = one|two|three\r\n', text)   # by name at first: one, three, two; then two moved up
        got = mods.scan(self.dest)
        self.assertEqual([m.name for m in got], ['one', 'two', 'three'])
        self.assertEqual({m.name for m in got if not m.enabled}, {'two'})
        self.assertEqual(mods.read_keys(self.dest)[1], {'two'})

    def test_move_at_the_ends_changes_nothing(self):
        self.mod('a'), self.mod('b')
        self.assertEqual([m.name for m in mods.move(self.dest, 'a', -1)], ['a', 'b'])
        self.assertEqual([m.name for m in mods.move(self.dest, 'b', +1)], ['a', 'b'])

    def test_an_existing_line_is_replaced_where_it_stands(self):
        self.mod('a'), self.mod('b')
        (self.dest / 'rekarina.ini').write_text('[mods]\nmods_order = b|a\n# note\nmods_disabled =\n[other]\nx = 1\n', encoding='utf-8')
        mods.set_enabled(self.dest, 'a', False)
        self.assertEqual((self.dest / 'rekarina.ini').read_text(encoding='utf-8'),
                         '[mods]\nmods_order = b|a\n# note\nmods_disabled = a\n[other]\nx = 1\n')

    def test_the_old_settings_file_is_used_until_the_game_renames_it(self):
        (self.dest / 'rekarina.ini').rename(self.dest / 'totk.ini')
        self.mod('a')
        mods.set_enabled(self.dest, 'a', False)
        self.assertFalse((self.dest / 'rekarina.ini').exists())
        self.assertIn('mods_disabled = a', (self.dest / 'totk.ini').read_text(encoding='utf-8'))


class AddRemove(Sandbox):
    def test_add_a_folder_and_a_wrapped_zip(self):
        src = Path(self.tmp.name) / 'My   mod'
        put(src / 'romfs' / 'a.bin')
        m = mods.add(self.dest, src)
        self.assertEqual((m.name, m.kinds), ('My mod', (mods.FILES,)))
        z = Path(self.tmp.name) / 'Pack v2.zip'
        with zipfile.ZipFile(z, 'w') as f:
            f.writestr('Pack v2/Pack/romfs/a.bin', b'x')
            f.writestr('Pack v2/Pack/exefs/E0F3.pchtxt', b'@nsobid-E0F3')
        m = mods.add(self.dest, z)
        self.assertEqual((m.name, m.files, m.patches), ('Pack v2', 1, 1))
        self.assertTrue((self.dest / 'mods' / 'Pack v2' / 'romfs' / 'a.bin').is_file())
        self.assertEqual(mods.add(self.dest, z).name, 'Pack v2 (2)')
        self.assertFalse([p for p in (self.dest / 'mods').iterdir() if p.name.startswith('.adding')])

    def test_what_is_no_mod_is_refused(self):
        src = Path(self.tmp.name) / 'docs'
        put(src / 'readme.txt')
        with self.assertRaises(mods.ModError) as e:
            mods.add(self.dest, src)
        self.assertEqual(e.exception.id, 'mods.not_a_mod')
        z = Path(self.tmp.name) / 'evil.zip'
        with zipfile.ZipFile(z, 'w') as f:
            f.writestr('../outside/romfs/a.bin', b'x')
        with self.assertRaises(mods.ModError) as e:
            mods.add(self.dest, z)
        self.assertEqual(e.exception.id, 'mods.bad_archive')
        self.assertFalse((Path(self.tmp.name) / 'game' / 'outside').exists())

    def test_remove_keeps_the_folder_aside(self):
        self.mod('a'), self.mod('b')
        mods.set_enabled(self.dest, 'a', False)
        kept = mods.remove(self.dest, 'a')
        self.assertTrue((kept / 'romfs' / 'a.bin').is_file())
        self.assertEqual([m.name for m in mods.scan(self.dest)], ['b'])
        self.assertEqual(mods.read_keys(self.dest), (['b'], set()))
        self.assertEqual(mods.summary(mods.scan(self.dest)), (1, 1, 0))


class BuildKeys(Sandbox):
    """installer/oot.py: a mod's code patch asks for the lift, the compile and the package step again, and for no
    second preparation of the shaders; a folder without code patches keeps every key it had."""

    def ctx(self):
        from types import SimpleNamespace
        root = Path(self.tmp.name) / 'setup'
        for f in ('recomp/gen.py', 'runtime/main.c', 'tools/build_game.py', 'tools/cheats.py', 'tools/exefs_patches.py',
                  'tools/package_oot.py', 'recomp/patches.txt'):
            put(root / f)
        layout = SimpleNamespace(root=root, recomp=root / 'recomp', tools=root / 'tools', patches=root / 'recomp' / 'patches.txt',
                                 pgo=None)
        return SimpleNamespace(dest=self.dest, layout=layout, keys={'oot_extract': 'e'}, options=SimpleNamespace(dlls=None))

    def keys(self):
        from . import oot
        ctx = self.ctx()
        ctx.keys['oot_lift'] = oot.Lift().key(ctx)
        ctx.keys['oot_compile'] = oot.Compile().key(ctx)
        ctx.keys['oot_prepare'] = 'p:' + oot.compile_base_key(ctx)
        ctx.keys['oot_package'] = oot.Package().key(ctx)
        return ctx.keys, oot.lift_base_key(ctx), oot.compile_base_key(ctx)

    def test_keys(self):
        self.mod('Files only')
        plain, lift0, compile0 = self.keys()
        self.assertEqual((plain['oot_lift'], plain['oot_compile']), (lift0, compile0))
        self.mod('Patch', 'exefs/E0F3.pchtxt')
        patched, lift1, compile1 = self.keys()
        self.assertEqual((lift1, compile1), (lift0, compile0))
        self.assertNotEqual(patched['oot_lift'], plain['oot_lift'])
        self.assertNotEqual(patched['oot_compile'], plain['oot_compile'])
        self.assertEqual(patched['oot_prepare'], plain['oot_prepare'])
        self.assertNotEqual(patched['oot_package'], plain['oot_package'])
        put(self.dest / 'mods' / 'Patch' / 'exefs' / 'E0F3.pchtxt', b'changed')
        self.assertNotEqual(self.keys()[0]['oot_lift'], patched['oot_lift'])

    def test_rebuild_command_names_the_folder(self):
        from . import oot
        ctx = self.ctx()
        ctx.layout.dev, ctx.layout.python = True, Path('python')
        oot.write_rebuild_command(ctx)
        lines = (self.dest / 'build-cache' / 'rebuild.txt').read_text(encoding='utf-8').splitlines()
        self.assertEqual(lines[-2:], ['--rebuild', str(self.dest)])
        self.assertIn('installer.gui_oot', lines[2])


class Built(Sandbox):
    """build-cache/mods-built.txt: a mod waits while the lift has not seen a code file of it as it is now."""

    def test_waits_until_the_lift_has_seen_the_file(self):
        self.mod('Files only')
        patch = self.mod('Patch', 'exefs/E0F3.pchtxt')
        self.mod('Cheats', 'cheats/E0F3CCFAF50065AE.txt')
        waits = lambda: {m.name for m in mods.scan(self.dest) if m.waits}   # noqa: E731
        self.assertEqual(waits(), {'Patch', 'Cheats'})
        self.assertEqual(mods.summary(mods.scan(self.dest)), (3, 3, 2))
        mods.set_enabled(self.dest, 'Cheats', False)   # a mod switched off asks for no rebuild
        self.assertEqual(mods.summary(mods.scan(self.dest)), (3, 2, 1))
        mods.write_built(self.dest, [f for m in mods.scan(self.dest) for f in mods.patch_files(m)])
        self.assertEqual(waits(), set())
        put(patch / 'exefs' / 'E0F3.pchtxt', b'changed')
        self.assertEqual(waits(), {'Patch'})
        mods.write_built(self.dest, [])
        self.assertFalse((self.dest / mods.BUILT).exists())


class Page(Sandbox):
    """The launcher's Mods page on a made-up game folder, in a window nobody sees."""

    def setUp(self):
        super().setUp()
        try:
            import tkinter as tk
            from . import gui_oot, oot
            from . import shots as screenshots
            gui_oot.fix_tcl_paths()
            self.root = screenshots.unseen_root() if screenshots.AVAILABLE else tk.Tk()
        except Exception as e:   # no display, no Tk
            self.skipTest(f'no window: {e}')
        put(self.dest / oot.GAME_EXE)
        self.app = gui_oot.RekarinaApp(self.root, 'en', oot.find_layout(None, None))
        self.app.vars['dest'].set(str(self.dest))

    def tearDown(self):
        self.root.destroy()
        super().tearDown()

    def shown(self):
        c = self.app.stage
        return [c.itemcget(row[3], 'text') for row in self.app.mod_rows if c.itemcget(row[0], 'state') != 'hidden']

    def test_the_list_is_walked_switched_moved_and_emptied(self):
        app = self.app
        for n in ('a', 'b'):
            self.mod(n)
        self.mod('c', 'exefs/E0F3.pchtxt')
        self.mod('plug', 'exefs/subsdk9')
        app.refresh_home()
        self.assertEqual(app.facts[3][1].cget('text'), '3 of 4 on · waiting for a rebuild: 1')
        app.open_mods()
        self.assertEqual((self.shown(), app.mod_sel), (['a', 'b', 'c', 'plug'], 'a'))
        self.assertFalse(app.mods_rebuild.shown)   # the folder was not built by the setup: no rebuild to offer
        self.assertFalse(app.mods_up.enabled)
        app.arrow(1, True)
        app.press_cursor()   # Enter in the list: the chosen mod's switch
        self.assertEqual((app.mod_sel, mods.read_keys(self.dest)[1]), ('b', {'b'}))
        app.move_mod(-1)
        self.assertEqual((self.shown(), app.mod_sel), (['b', 'a', 'c', 'plug'], 'b'))
        app.arrow(1, False)   # Right: the cursor goes to the plates, Enter presses the one it is on
        self.assertIs(app.cursor_on, app.mods_add_zip)
        for _ in range(3):
            app.arrow(1, False)
        self.assertIs(app.cursor_on, app.mods_down)   # Move up is off at the top of the list and is stepped over
        app.press_cursor()
        self.assertEqual(self.shown(), ['a', 'b', 'c', 'plug'])
        app.arrow(1, True)   # Down: back in the list
        self.assertEqual((app.cursor_on, app.mod_sel), (None, 'c'))
        app.step_mod(1)
        app.toggle_mod()   # a mod the game cannot use is not switched on; the line under the list says why
        self.assertIn('plug', app.stage.itemcget(app.mods_line, 'text'))
        self.assertEqual(mods.read_keys(self.dest)[1], {'b'})
        app.remove_mod()
        self.assertEqual((self.shown(), app.mod_sel), (['a', 'b', 'c'], 'c'))
        self.assertTrue((self.dest / 'mods-removed' / 'plug' / 'exefs' / 'subsdk9').is_file())
        zipped = Path(self.tmp.name) / 'New mod.zip'
        with zipfile.ZipFile(zipped, 'w') as f:
            f.writestr('romfs/a.bin', b'x')
        app.added_mod(mods.add(self.dest, zipped))
        self.assertEqual((self.shown(), app.mod_sel), (['a', 'b', 'c', 'New mod'], 'New mod'))
        app.added_mod(mods.ModError('mods.not_a_mod', path='x.zip'))
        self.assertIn('x.zip', app.stage.itemcget(app.mods_line, 'text'))
        app.panel_escape(None)
        self.assertEqual(app.screen, 'home')

    def test_a_long_list_follows_the_cursor(self):
        app = self.app
        for i in range(10):
            self.mod(f'mod {i}')
        app.open_mods()
        self.assertEqual(self.shown(), [f'mod {i}' for i in range(7)])
        for _ in range(8):
            app.step_mod(1)
        self.assertEqual((app.mod_sel, self.shown()), ('mod 8', [f'mod {i}' for i in range(2, 9)]))
        self.assertEqual(app.stage.itemcget(app.mods_hint, 'text'), '3–9 of 10')
        app.step_mod(1), app.step_mod(1)
        self.assertEqual((app.mod_sel, self.shown()[-1]), ('mod 9', 'mod 9'))


if __name__ == '__main__':
    unittest.main()
