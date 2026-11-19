"""installer/ bookkeeping with fake steps: fresh install, resume after a failure or cancel, transient steps, update
with a newer release, rekarina.ini and the event stream. No game files, no tools; stdlib only.

Run: python tests/installer_test.py
"""
import contextlib
import http.client
import io
import json
import re
import shutil
import sys
import tempfile
import time
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
from installer import ini, runner, state, steps  # noqa: E402
from installer.events import Reporter  # noqa: E402
from installer.layout import Layout  # noqa: E402


class Fake(runner.Step):
    """A step whose output is <dir>/<id>.out holding its key; dir is the temp folder for a transient step."""

    def __init__(self, id, transient=False, always=False, previous=None):
        self.id, self.transient, self.always, self.previous = id, transient, always, previous

    def out(self, ctx):
        return (ctx.temp if self.transient else ctx.dest) / f'{self.id}.out'

    def key(self, ctx):
        upstream = ctx.keys[self.previous] if self.previous else ''
        return f'{self.id}:{ctx.options.inputs.get(self.id, "v1")}:{upstream}'

    def current(self, ctx, record):
        p = self.out(ctx)
        return p.is_file() and p.read_text() == ctx.keys[self.id]

    def verify(self, ctx, record):
        ctx.options.verified.append(self.id)
        return ctx.options.verify_ok.get(self.id, True)

    def run(self, ctx):
        o = ctx.options
        o.ran.append(self.id)
        if self.id in ctx.forced:
            o.forced.append(self.id)
        if self.id == 'check':
            ctx.verify_kept(self.id)
        failure = o.fail.pop(self.id, None)
        if failure == 'error':
            raise runner.InstallError('fake.failed', step=self.id)
        if failure == 'cancel':
            raise KeyboardInterrupt
        if self.id == 'cleanup':
            for p in ctx.temp.glob('*.out'):
                p.unlink()
            return {}
        p = self.out(ctx)
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(ctx.keys[self.id])
        return {'note': self.id}


def fake_steps():
    return [Fake('check', always=True), Fake('extract'), Fake('lift', transient=True, previous='extract'),
            Fake('compile', previous='lift'), Fake('prepare', previous='compile'), Fake('cleanup', always=True)]


class Bed:
    """A dest and temp folder plus one run of the fake steps per call."""

    def __init__(self, base):
        self.dest, self.temp = base / 'dest', base / 'temp'
        self.temp.mkdir()

    def run(self, release='1.0', inputs=None, fail=None, verify_ok=None, verify=False):
        layout = Layout(root=Path('.'), dev=True, release=release, python=Path('python'), tools=Path('tools'),
                        recomp=Path('recomp'), llvm=Path('llvm'),
                        deps=Path('deps'), runtime=Path('rt'), runtime_lib=Path('lib'), ninja=Path('ninja'),
                        hactool=Path('hactool'), xlate=Path('xlate'))
        options = SimpleNamespace(inputs=inputs or {}, fail=dict(fail or {}), verify_ok=verify_ok or {}, ran=[],
                                  verified=[], forced=[])
        out = io.StringIO()
        ctx = runner.Context(layout, self.dest, self.temp, Reporter(out=out), options, state=state.load(self.dest),
                             verify=verify)
        mode = ctx.mode
        result = runner.run_steps(fake_steps(), ctx)
        events = [json.loads(line) for line in out.getvalue().splitlines()]
        return SimpleNamespace(result=result, ran=options.ran, verified=options.verified, forced=options.forced,
                               events=events, mode=mode,
                               state=state.load(self.dest))

    @staticmethod
    def skipped(r):
        return {e['step']: e['reason'] for e in r.events if e['type'] == 'step' and e['state'] == 'skipped'}


ALL = ['check', 'extract', 'lift', 'compile', 'prepare', 'cleanup']


class RunnerTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.bed = Bed(Path(self.tmp.name))

    def tearDown(self):
        self.tmp.cleanup()

    def test_fresh_install_runs_everything_and_records_it(self):
        r = self.bed.run()
        self.assertEqual((r.result, r.mode, r.ran), ('ok', 'install', ALL))
        self.assertEqual(set(r.state['steps']), {'extract', 'lift', 'compile', 'prepare'})
        self.assertEqual(r.state['steps']['compile']['note'], 'compile')
        self.assertEqual(r.state['release'], '1.0')
        started = [e['step'] for e in r.events if e['type'] == 'step' and e['state'] == 'started']
        done = [e['step'] for e in r.events if e['type'] == 'step' and e['state'] == 'done']
        self.assertEqual((started, done), (ALL, ALL))

    def test_rerun_skips_finished_steps_and_unneeded_transient(self):
        self.bed.run()
        r = self.bed.run()
        self.assertEqual((r.result, r.mode, r.ran), ('ok', 'resume', ['check', 'cleanup']))
        self.assertEqual(self.bed.skipped(r), {'extract': 'current', 'lift': 'not_needed', 'compile': 'current',
                                               'prepare': 'current'})
        self.assertEqual(r.verified, [])   # same release: no full checks

    def test_resume_after_failure(self):
        r = self.bed.run(fail={'compile': 'error'})
        self.assertEqual((r.result, r.ran), ('failed', ['check', 'extract', 'lift', 'compile']))
        error = [e for e in r.events if e['type'] == 'error'][0]
        self.assertEqual((error['id'], error['step'], error['params']), ('fake.failed', 'compile', {'step': 'compile'}))
        self.assertEqual(set(r.state['steps']), {'extract', 'lift'})
        r = self.bed.run()
        # lift's output is still in the temp folder (no cleanup ran), so only compile and later run.
        self.assertEqual((r.result, r.mode, r.ran), ('ok', 'resume', ['check', 'compile', 'prepare', 'cleanup']))

    def test_resume_after_cancel_with_transient_output_gone(self):
        r = self.bed.run(fail={'prepare': 'cancel'})
        self.assertEqual(r.result, 'cancelled')
        self.assertNotIn('prepare', r.state['steps'])
        for p in self.bed.temp.glob('*.out'):
            p.unlink()
        r = self.bed.run()
        self.assertEqual(r.ran, ['check', 'prepare', 'cleanup'])
        self.assertEqual(self.bed.skipped(r)['lift'], 'not_needed')

    def test_failed_check_writes_nothing(self):
        r = self.bed.run(fail={'check': 'error'})
        self.assertEqual(r.result, 'failed')
        self.assertFalse(self.bed.dest.exists())

    def test_removed_output_runs_again(self):
        self.bed.run()
        (self.bed.dest / 'compile.out').unlink()
        r = self.bed.run()
        self.assertEqual(r.ran, ['check', 'lift', 'compile', 'cleanup'])

    def test_update_with_new_lifter_keeps_extraction(self):
        self.bed.run(release='1.0')
        r = self.bed.run(release='1.1', inputs={'lift': 'v2'})
        self.assertEqual((r.result, r.mode), ('ok', 'update'))
        self.assertEqual(r.ran, ['check', 'lift', 'compile', 'prepare', 'cleanup'])
        self.assertEqual(r.verified, ['extract'])   # kept from another release: full check
        self.assertEqual(r.state['steps']['extract']['release'], '1.0')
        self.assertEqual(r.state['steps']['compile']['release'], '1.1')
        self.assertEqual(r.state['release'], '1.1')

    def test_update_that_changes_nothing_skips_the_rebuild(self):
        self.bed.run(release='1.0')
        r = self.bed.run(release='1.1')
        self.assertEqual(r.ran, ['check', 'cleanup'])
        self.assertEqual(r.verified, ['extract', 'compile', 'prepare'])

    def test_update_with_changed_kept_files_extracts_again(self):
        self.bed.run(release='1.0')
        r = self.bed.run(release='1.1', verify_ok={'extract': False})
        self.assertEqual(r.ran, ['check', 'extract', 'cleanup'])
        self.assertEqual(r.forced, ['extract'])   # the step knows not to trust what it left behind
        warnings = [e for e in r.events if e['type'] == 'warning']
        self.assertEqual([(w['id'], w['params']) for w in warnings], [('state.output_changed', {'step': 'extract'})])

    def test_verify_option_checks_same_release(self):
        self.bed.run()
        r = self.bed.run(verify=True)
        self.assertEqual(r.verified, ['extract', 'compile', 'prepare'])

    def test_state_errors(self):
        self.bed.dest.mkdir()
        (self.bed.dest / state.NAME).write_text('{not json')
        with self.assertRaises(state.StateError) as e:
            state.load(self.bed.dest)
        self.assertEqual(e.exception.id, 'state.unreadable')
        (self.bed.dest / state.NAME).write_text(json.dumps({'format': 99, 'steps': {}}))
        with self.assertRaises(state.StateError) as e:
            state.load(self.bed.dest)
        self.assertEqual(e.exception.id, 'state.newer_format')

    def test_state_old_name(self):
        # a folder set up before the rename keeps its record under totk-install.json: read, then replaced
        self.bed.dest.mkdir()
        (self.bed.dest / state.OLD_NAME).write_text(json.dumps({'format': 1, 'release': 'old', 'steps': {'extract': {}}}))
        data = state.load(self.bed.dest)
        self.assertEqual(data['release'], 'old')
        state.save(self.bed.dest, data)
        self.assertTrue((self.bed.dest / state.NAME).is_file())
        self.assertFalse((self.bed.dest / state.OLD_NAME).exists())


class ReleaseFilesTest(unittest.TestCase):
    def test_release_ships_every_tool_the_steps_run(self):
        sys.path.insert(0, str(ROOT / 'tools'))
        import make_release
        text = (ROOT / 'installer' / 'steps.py').read_text(encoding='utf-8')
        used = set(re.findall(r"layout\.tools / '(\w+\.py)'", text))
        for names in re.findall(r"layout\.tools / n for n in \((.*?)\)", text, re.S):
            used |= set(re.findall(r"'(\w+\.py)'", names))
        self.assertIn('cheats.py', used)
        self.assertEqual(used - set(make_release.TOOLS_FILES), set())


class RenamedVersionTest(unittest.TestCase):
    """An install the setup before 0.3.1 recorded as 1.1.1 is the 1.1.0 update (#332)."""

    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        root = Path(__file__).resolve().parents[1]
        layout = Layout(root=root, dev=True, release='9.9', python=Path('python'), tools=root / 'tools',
                        recomp=root / 'recomp', llvm=Path('llvm'), deps=Path('deps'), runtime=Path('rt'),
                        runtime_lib=Path('lib'), ninja=Path('ninja'), hactool=Path('hactool'), xlate=Path('xlate'))
        state = {'format': 1, 'release': '0.2.8', 'version': '1.1.1', 'inputs': {},
                 'steps': {'extract': {'key': 'recorded by 0.2.8', 'release': '0.2.8'}}}
        out = io.StringIO()
        self.ctx = runner.Context(layout, Path(tmp.name), Path(tmp.name), Reporter(out=out), SimpleNamespace(),
                                  state=state)

    def test_old_name_is_read_as_1_1_0(self):
        steps.migrate_state(self.ctx)
        self.assertEqual(steps.game_version(self.ctx), '1.1.0')
        self.assertEqual(steps.version_tag(steps.game_version(self.ctx)), 'v110')
        self.assertEqual(steps.expected_update(self.ctx)['display_version'], '1.1.0')
        self.assertTrue(self.ctx.layout.patches(steps.game_version(self.ctx)).is_file())

    def test_extraction_is_kept_under_the_new_name(self):
        steps.migrate_state(self.ctx)
        self.assertEqual(self.ctx.state['steps']['extract']['key'], steps.Extract().key(self.ctx))
        self.assertEqual(self.ctx.state['steps']['extract']['release'], '0.2.8')   # full check of kept output runs

    def test_other_versions_are_untouched(self):
        self.ctx.state['version'] = '1.4.3'
        steps.migrate_state(self.ctx)
        self.assertEqual(self.ctx.state['version'], '1.4.3')
        self.assertEqual(self.ctx.state['steps']['extract']['key'], 'recorded by 0.2.8')


class LiftKeyTest(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.dest = Path(tmp.name)
        layout = SimpleNamespace(recomp=self.dest / 'no-recomp', tools=self.dest / 'no-tools',
                                 patches=lambda version: self.dest / 'no-patches')
        self.ctx = SimpleNamespace(dest=self.dest, layout=layout, keys={'extract': 'x'}, state={'version': '1.4.3'})

    def key(self):
        return steps.Lift().key(self.ctx)

    def add_cheats(self, name, text, mod='Cheats', where='cheats'):
        folder = self.dest / 'mods' / mod / where
        folder.mkdir(parents=True, exist_ok=True)
        (folder / name).write_text(text)

    def test_extracting_main_keeps_the_key(self):
        before = self.key()
        (self.dest / 'exefs').mkdir()
        (self.dest / 'exefs' / 'main').write_bytes(bytes(0x48))
        self.assertEqual(self.key(), before)

    def test_cheat_files_change_the_key(self):
        before = self.key()
        self.add_cheats('277178B7DBA1B6D4.txt', '[a]')
        added = self.key()
        self.assertNotEqual(added, before)
        self.add_cheats('277178B7DBA1B6D4.txt', '[b]')
        edited = self.key()
        self.assertNotEqual(edited, added)
        self.add_cheats('277178B7DBA1B6D4.txt', '[c]', mod='Other', where='atmosphere/contents/0100F2C0115B6000/cheats')
        self.assertNotEqual(self.key(), edited)

    def test_exefs_patch_files_change_the_key(self):
        before = self.key()
        self.add_cheats('dynamic-fps.pchtxt', '@nsobid-277178B7DBA1B6D4', mod='Fps', where='exefs')
        added = self.key()
        self.assertNotEqual(added, before)
        self.add_cheats('277178B7DBA1B6D4.ips', 'PATCHEOF', mod='Ips', where='atmosphere/exefs_patches/x')
        both = self.key()
        self.assertNotEqual(both, added)
        self.add_cheats('main.npdm', 'not a patch', mod='Fps', where='exefs')
        self.assertEqual(self.key(), both)

    def test_renaming_a_mod_folder_changes_the_key(self):
        # tools/cheats.py and tools/exefs_patches.py name a patch set after "<mod folder>/<cheat or file>": a renamed
        # folder needs a lift even though its files are the same
        self.add_cheats('277178B7DBA1B6D4.txt', '[a]', mod='MyNXCheats')
        self.add_cheats('dynamic-fps.pchtxt', '@nsobid-277178B7DBA1B6D4', mod='Fps', where='exefs')
        before = self.key()
        mods = self.dest / 'mods'
        (mods / 'MyNXCheats').rename(mods / 'MyNXCheats 1.4.3')
        cheats_renamed = self.key()
        self.assertNotEqual(cheats_renamed, before)
        (mods / 'Fps').rename(mods / 'Fps 2')
        self.assertNotEqual(self.key(), cheats_renamed)

    def test_status_and_other_files_keep_the_key(self):
        self.add_cheats('277178B7DBA1B6D4.txt', '[a]')
        before = self.key()
        (self.dest / 'build-cache').mkdir()
        (self.dest / 'build-cache' / 'cheats.status').write_text('written by lift')
        self.add_cheats('readme.md', 'not a cheat file')
        (self.dest / 'mods' / 'Cheats' / 'romfs').mkdir()
        (self.dest / 'mods' / 'Cheats' / 'romfs' / 'a.txt').write_text('a RomFS file')
        self.assertEqual(self.key(), before)


class IniTest(unittest.TestCase):
    EXAMPLE = '[display]\n# vk_present = mailbox\n\n[language]\n# locale = en-US\n# lang = en-US\n# x = 1\n'

    def test_example_line_is_activated(self):
        out = ini.set_language(self.EXAMPLE, 'ru')
        self.assertIn('\nlang = ru\n', out)
        self.assertNotIn('# lang', out)
        self.assertIn('# locale = en-US', out)

    def test_active_line_is_rewritten_and_the_rest_kept(self):
        text = '[display]\nvk_present = fifo\n[language]\n  LANG =  ja  \nfps60 = 0\n'
        out = ini.set_language(text, 'de')
        self.assertEqual(out, '[display]\nvk_present = fifo\n[language]\n  lang =  de\nfps60 = 0\n')
        self.assertEqual(ini.get_language(out), 'de')

    def test_missing_section_is_added(self):
        self.assertEqual(ini.set_language('fps60 = 0\n', 'fr'), 'fps60 = 0\n\n[language]\nlang = fr\n')
        self.assertEqual(ini.set_language('[language]\nlocale = de-DE\n', 'fr'),
                         '[language]\nlang = fr\nlocale = de-DE\n')

    def test_old_chinese_codes_are_written_as_the_canonical_ones(self):
        self.assertEqual(ini.canonical_language('zh-CN'), 'zh-Hans')
        self.assertEqual(ini.canonical_language('zh-TW'), 'zh-Hant')
        self.assertEqual(ini.canonical_language('ja'), 'ja')
        self.assertNotIn('zh-CN', ini.LANGUAGES)
        self.assertNotIn('zh-TW', ini.LANGUAGES)
        out = ini.set_language(self.EXAMPLE, 'zh-CN')
        self.assertEqual(ini.get_language(out), 'zh-Hans')
        self.assertEqual(ini.get_language(ini.set_language(out, 'zh-TW')), 'zh-Hant')

    def test_write_keeps_user_lines(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            example = d / 'rekarina.ini.example'
            example.write_text(self.EXAMPLE)
            path = ini.write(d / '', 'en-GB', example)
            self.assertEqual(ini.get_language(path.read_text()), 'en-GB')
            path.write_text(path.read_text().replace('# vk_present = mailbox', 'vk_present = fifo'))
            ini.write(d, 'ja', example)
            text = path.read_text()
            self.assertIn('vk_present = fifo', text)
            self.assertEqual(ini.get_language(text), 'ja')


class CleanupTest(unittest.TestCase):
    """The real Cleanup step on a finished install whose romfs/ is intact, emptied or gone (#107)."""

    def setUp(self):
        self.steps = steps
        self.tmp = tempfile.TemporaryDirectory()
        base = Path(self.tmp.name)
        self.dest, temp = base / 'dest', base / 'temp'
        (self.dest / 'romfs').mkdir(parents=True)
        temp.mkdir()
        image = b'romfs image'
        (self.dest / 'romfs' / 'romfs.bin').write_bytes(image)
        digest = steps.extract.sha256(self.dest / 'romfs' / 'romfs.bin')
        (self.dest / 'romfs' / 'index.json').write_text(json.dumps({'romfs_size': len(image), 'romfs_sha256': digest}))
        self.expected = {'romfs': {'size': len(image), 'sha256': digest}}
        self.ctx = SimpleNamespace(reporter=Reporter(out=io.StringIO(), log_path=base / 'x.log'), dest=self.dest,
                                   temp=temp, lang='en-US', state={},
                                   layout=SimpleNamespace(ini_example=ROOT / 'rekarina.ini.example', dev=False,
                                                          python=base / 'python' / 'python.exe', root=base / 'release'))
        self.saved = steps.expected_update
        steps.expected_update = lambda ctx: self.expected

    def tearDown(self):
        self.steps.expected_update = self.saved
        self.ctx.reporter.close()
        self.tmp.cleanup()

    def run_cleanup(self):
        try:
            self.steps.Cleanup().run(self.ctx)
            return 'ok'
        except self.steps.InstallError as e:
            return e.id

    def test_intact_romfs_passes(self):
        self.assertEqual(self.run_cleanup(), 'ok')

    def test_keeps_build_and_writes_rebuild_command(self):
        for name in ('gen', 'build', 'check'):
            (self.ctx.temp / name).mkdir()
        self.assertEqual(self.run_cleanup(), 'ok')
        self.assertTrue((self.ctx.temp / 'gen').is_dir() and (self.ctx.temp / 'build').is_dir())
        self.assertFalse((self.ctx.temp / 'check').exists())
        lines = (self.dest / 'build-cache' / 'rebuild.txt').read_text(encoding='utf-8').splitlines()
        self.assertEqual(lines[1:], ['-c', steps.REBUILD_BOOT, str(self.ctx.layout.root), '--root',
                                     str(self.ctx.layout.root), '--rebuild', str(self.dest)])

    def test_emptied_romfs_fails(self):
        for p in (self.dest / 'romfs').iterdir():
            p.unlink()
        self.assertEqual(self.run_cleanup(), 'cleanup.romfs_unreadable')

    def test_missing_romfs_fails(self):
        self.steps.remove(self.dest / 'romfs')
        self.assertEqual(self.run_cleanup(), 'cleanup.romfs_unreadable')

    def test_romfs_of_another_size_fails(self):
        (self.dest / 'romfs' / 'romfs.bin').write_bytes(b'short')
        self.assertEqual(self.run_cleanup(), 'cleanup.romfs_unreadable')


class BaseGameCheckTest(unittest.TestCase):
    """Issue #261: an update NSP chosen as the base game was reported as "not TotK"."""
    BASE, UPDATE = '0100F2C0115B6000', '0100F2C0115B6800'

    @staticmethod
    def nca(content_type, title_id):
        return ({'name': 'x.nca', 'nca_offset': 0, 'size': 0xc00, 'content_type': content_type, 'title_id': title_id}, b'')

    def test_update_package_names_the_update(self):
        # An update NSP keeps the base title id on its Program NCA; only its Meta NCA carries the update id.
        records = [self.nca(0, self.BASE), self.nca(1, self.UPDATE)]
        with self.assertRaises(ValueError):
            steps.program_nca(records)
        self.assertEqual(steps.base_misuse(records), 'check.base_is_update')

    def test_combined_package_is_named(self):
        records = [self.nca(0, self.BASE), self.nca(1, self.BASE), self.nca(1, self.UPDATE)]
        self.assertEqual(steps.base_misuse(records), 'check.base_combined')

    def test_plain_base_game_passes(self):
        records = [self.nca(0, self.BASE), self.nca(1, self.BASE), self.nca(3, self.BASE)]
        self.assertIsNone(steps.base_misuse(records))
        self.assertEqual(steps.program_nca(records)[0]['title_id'], self.BASE)


class FontsTest(unittest.TestCase):
    """The fonts step: keyed by language and firmware, a no-op without firmware, writes dest/shared-fonts."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        base = Path(self.tmp.name)
        self.dest, temp, self.firmware = base / 'dest', base / 'temp', base / 'firmware'
        temp.mkdir()
        self.firmware.mkdir()
        (self.firmware / 'a.nca').write_bytes(b'nca')
        self.keys = base / 'prod.keys'
        self.keys.write_text('header_key = 00\n')
        self.ctx = SimpleNamespace(dest=self.dest, temp=temp, lang='zh-Hans', state={},
                                   layout=SimpleNamespace(tools=ROOT / 'tools', root=ROOT),
                                   reporter=Reporter(out=io.StringIO()),
                                   options=SimpleNamespace(lang='zh-Hans', firmware=self.firmware, keys=str(self.keys)))
        self.saved_run_tool = steps.run_tool

    def tearDown(self):
        steps.run_tool = self.saved_run_tool
        self.ctx.reporter.close()
        self.tmp.cleanup()

    def fake_tool(self, files=None, code=0):
        """Stand-in for extract_shared_fonts.py: writes the requested files where --output points."""
        def run(ctx, step, script, args):
            out = Path(args[args.index('--output') + 1])
            out.mkdir(parents=True)
            for name in steps.SHARED_FONT_FILES if files is None else files:
                (out / name).write_bytes(b'bfttf')
            (out / 'manifest.json').write_text('{}')
            log = ctx.temp / 'fonts.log'
            log.write_text('error line\n')
            return code, log
        steps.run_tool = run

    def test_all_steps_order(self):
        self.assertEqual([s.id for s in steps.all_steps()],
                         ['check', 'fonts', 'extract', 'lift', 'compile', 'prepare', 'dlss', 'cleanup'])

    def test_extracts_into_the_install_folder(self):
        self.fake_tool()
        self.assertEqual(steps.Fonts().run(self.ctx), {'files': 6})
        self.assertTrue(steps.shared_fonts_ready(self.dest))
        self.assertEqual(sorted(p.name for p in (self.dest / 'shared-fonts').iterdir()),
                         sorted((*steps.SHARED_FONT_FILES, 'manifest.json')))

    def test_noop_without_firmware(self):
        self.ctx.options.firmware = None
        self.fake_tool()
        self.assertEqual(steps.Fonts().run(self.ctx), {})
        self.assertFalse((self.dest / 'shared-fonts').exists())

    def test_noop_for_other_languages(self):
        self.ctx.lang, self.ctx.options.lang = 'en-US', 'en-US'
        self.fake_tool()
        self.assertEqual(steps.Fonts().run(self.ctx), {})
        self.assertFalse((self.dest / 'shared-fonts').exists())

    def test_failed_extraction_keeps_the_old_folder(self):
        self.fake_tool()
        steps.Fonts().run(self.ctx)
        self.fake_tool(code=1)
        with self.assertRaises(steps.InstallError) as e:
            steps.Fonts().run(self.ctx)
        self.assertEqual(e.exception.id, 'fonts.failed')
        self.assertTrue(steps.shared_fonts_ready(self.dest))

    def test_incomplete_output_is_an_error(self):
        self.fake_tool(files=steps.SHARED_FONT_FILES[:1])
        with self.assertRaises(steps.InstallError) as e:
            steps.Fonts().run(self.ctx)
        self.assertEqual(e.exception.id, 'fonts.incomplete')

    def test_current_tracks_language_and_output(self):
        fonts = steps.Fonts()
        self.assertFalse(fonts.current(self.ctx, {}))
        self.fake_tool()
        fonts.run(self.ctx)
        self.assertTrue(fonts.current(self.ctx, {}))
        self.ctx.options.lang = 'en-US'
        self.assertTrue(fonts.current(self.ctx, {}))
        self.ctx.options.lang = 'zh-Hans'
        (self.dest / 'shared-fonts' / '0-standard.bfttf').unlink()
        self.assertFalse(fonts.current(self.ctx, {}))

    def test_key_changes_with_language_and_firmware(self):
        fonts = steps.Fonts()
        zh = fonts.key(self.ctx)
        self.ctx.options.lang = 'en-US'
        self.assertNotEqual(zh, fonts.key(self.ctx))
        self.ctx.options.lang = 'zh-Hans'
        (self.firmware / 'b.nca').write_bytes(b'other')
        self.assertNotEqual(zh, fonts.key(self.ctx))

    def test_registered_child_is_used(self):
        nested = self.firmware / 'registered'
        nested.mkdir()
        (nested / 'c.nca').write_bytes(b'nca')
        self.assertEqual(steps.firmware_dir(self.firmware), nested)


class OotShaderStepsTest(unittest.TestCase):
    """The prompts step and the shader step of the OoT setup: each is redone for its own inputs only, an old record of
    the shader step stands, every setup run prepares every variant (a folder of 1.2.1-1.2.3 that prepared the variants
    play met gets the rest from what it holds), and a game folder that holds every variant under this translator
    prepares nothing, whatever the temp folder holds."""

    def setUp(self):
        from installer import oot, runner
        self.oot, self.runner = oot, runner
        self.tmp = tempfile.TemporaryDirectory()
        self.base = base = Path(self.tmp.name)
        tools = base / 'root' / 'tools'
        (tools / 'data').mkdir(parents=True)
        for n in (*oot.SHADER_TOOLS, 'make_oot_prompts.py', 'oot_hint_layout.py', 'button_icons.py', 'make_ext_font.py'):
            (tools / n).write_text(n)
        (tools / 'data' / oot.SHADER_DATA[0]).write_bytes(b'seed')
        (base / 'bin').mkdir()
        self.xlate = base / 'bin' / 'rekarina_xlate.exe'
        self.xlate.write_bytes(b'x')
        self.xlate.with_suffix('.id').write_text('identity-1')
        (base / 'temp' / 'build').mkdir(parents=True)
        self.layout = SimpleNamespace(tools=tools, xlate=self.xlate, libs=(), root=base / 'root', release='1.2.1')
        self.options = SimpleNamespace(all_shaders=False)
        self.calls = []
        build = base / 'temp' / 'build'

        def run(ctx, step, script, args, parse=None, env=None):
            self.calls.append((script.name, list(map(str, args))))
            if script.name == 'oot_prepare_shaders.py':
                (build / 'shader-prepared.bin').write_bytes(b'p')
            return 0, ctx.temp / 'tool.log'
        self.saved = oot.run_tool
        oot.run_tool = run

    def tearDown(self):
        self.oot.run_tool = self.saved
        self.tmp.cleanup()

    def ctx(self, state=None):
        ctx = self.runner.Context(self.layout, self.base / 'dest', self.base / 'temp', Reporter(out=io.StringIO()), self.options,
                                  state=state)
        ctx.nup = self.base / 'game.nup'
        ctx.keys['oot_extract'] = 'dump-1'
        return ctx

    def keys(self, ctx=None):
        ctx = ctx or self.ctx()
        return self.oot.Prompts().key(ctx), self.oot.Prepare().key(ctx)

    def test_each_step_follows_its_own_tools(self):
        prompts, shaders = self.keys()
        (self.layout.tools / 'oot_hint_layout.py').write_text('changed')
        again = self.keys()
        self.assertNotEqual(again[0], prompts)
        self.assertEqual(again[1], shaders)          # a prompt drawing change does not prepare the shaders again
        prompts = again[0]
        for name in ('oot_prepare_shaders.py', 'prepare_shaders.py'):
            (self.layout.tools / name).write_text('changed ' + name)
            again = self.keys()
            self.assertEqual(again[0], prompts)
            self.assertNotEqual(again[1], shaders)
            shaders = again[1]
        (self.layout.tools / 'data' / self.oot.SHADER_DATA[0]).write_bytes(b'seed 2')
        self.assertNotEqual(self.keys()[1], shaders)

    def test_the_shader_key_follows_the_translator_and_the_dump_not_the_build(self):
        _, shaders = self.keys()
        self.xlate.with_suffix('.id').write_text('identity-2')
        self.assertNotEqual(self.keys()[1], shaders)
        self.xlate.with_suffix('.id').write_text('identity-1')
        self.assertEqual(self.keys()[1], shaders)
        ctx = self.ctx()
        ctx.keys['oot_extract'] = 'dump-2'
        self.assertNotEqual(self.oot.Prepare().key(ctx), shaders)
        (self.layout.tools / 'build_game.py').write_text('a new compile')   # nothing of the build is in it
        self.assertEqual(self.keys()[1], shaders)

    def dest_prepared(self, identity='identity-1', pack=b'a pack', state=True):
        """The game folder as a package step leaves it: the translator's identity, the pack, shader-prepared.bin and,
        with state, the pipeline cache and the driver's cache."""
        dest = self.base / 'dest'
        (dest / 'shader-cache').mkdir(parents=True, exist_ok=True)
        (dest / 'rekarina_xlate.id').write_text(identity)
        (dest / 'shader-prepared.bin').write_bytes(b'p')
        if pack is not None:
            (dest / 'shader-cache' / 'shader-cache.pack').write_bytes(pack)
        if state:
            (dest / 'pipeline-cache.bin').write_bytes(b'the pipelines play met')
            (dest / 'nv-cache').mkdir(exist_ok=True)
            (dest / 'nv-cache' / 'Rekarina.exe.bin').write_bytes(b'the driver')
        return dest

    def test_every_setup_run_prepares_every_variant(self):
        """The owner's call of 2026-11-19: no background preparation, every variant before play, whatever the folder
        prepared before and whatever an older launcher passes."""
        mode = self.oot.shader_mode
        met = {'format': 1, 'release': '1.2.3', 'inputs': {}, 'shaders': 'met',
               'steps': {'oot_prepare': {'key': 'k', 'shaders': 'met'}}}
        old = {'format': 1, 'release': '1.2.0', 'inputs': {}, 'steps': {'oot_prepare': {'key': 'k', 'release': '1.2.0'}}}
        for later in (False, True):
            for flag in (False, True):
                self.options.all_shaders, self.options.shaders_later = flag, later
                for saved in (None, met, old):
                    self.assertEqual(mode(self.ctx(saved)), 'all')
        self.options.all_shaders, self.options.shaders_later = False, False
        self.assertNotEqual(self.oot.Prepare().key(self.ctx(), 'met'), self.keys()[1])   # a met record is not current

    def test_an_old_record_stands_while_the_translator_is_the_one_that_prepared(self):
        build = self.base / 'temp' / 'build'
        (build / 'shader-prepared.bin').write_bytes(b'p')
        (build / 'rekarina_xlate.id').write_text('identity-1')

        def old(release='1.2.0'):
            return {'format': 1, 'release': release, 'inputs': {}, 'steps': {
                'oot_prepare': {'key': 'the old form', 'release': release, 'finished': 't', 'seconds': 1.0}}}
        extract = SimpleNamespace(id='oot_extract', transient=False, always=False, key=lambda ctx: 'dump-1',
                                  current=lambda ctx, rec: True, adopts=lambda ctx, rec: False)

        def plan(state):
            ctx = self.ctx(state)
            ctx.keys.clear()
            ctx.steps = [extract, self.oot.Prepare()]
            ctx.plan()
            return ctx
        ctx = plan(old())
        self.assertFalse(ctx.runs['oot_prepare'])                                   # the key form changed, nothing else
        rec = ctx.state['steps']['oot_prepare']
        self.assertEqual((rec['key'], rec['shaders'], ctx.state['shaders']), (ctx.keys['oot_prepare'], 'all', 'all'))
        again = plan(ctx.state)                                                     # and it is an ordinary record now
        self.assertFalse(again.runs['oot_prepare'])
        self.assertFalse(plan(old('1.2.1')).runs['oot_prepare'])
        self.assertTrue(plan(old('1.1.0')).runs['oot_prepare'])                     # a release with another seed
        (build / 'rekarina_xlate.id').write_text('identity-0')
        self.assertTrue(plan(old()).runs['oot_prepare'])                            # another translator
        (build / 'rekarina_xlate.id').write_text('identity-1')
        (build / 'shader-prepared.bin').unlink()
        self.assertTrue(plan(old()).runs['oot_prepare'])                            # output gone
        (build / 'shader-prepared.bin').write_bytes(b'p')
        (self.layout.tools / 'oot_romfs.py').write_text('another reader')
        self.assertTrue(plan(ctx.state).runs['oot_prepare'])                        # a new-form record follows its key

    def run_prepare(self, saved):
        """run_steps over the extraction (a stand-in) and the shader step: the scripts the step ran, in order."""
        extract = SimpleNamespace(id='oot_extract', transient=False, always=False, key=lambda ctx: 'dump-1',
                                  current=lambda ctx, rec: True, adopts=lambda ctx, rec: False, run=lambda ctx: {})
        ctx = self.ctx(saved)
        ctx.keys.clear()
        self.calls.clear()
        self.assertEqual(self.runner.run_steps([extract, self.oot.Prepare()], ctx), 'ok')
        self.assertFalse((self.base / 'temp' / 'build' / self.oot.Prepare.PREPARING).exists())   # the run finished
        return ctx, [name for name, _ in self.calls]

    def fresh_temp(self):
        build = self.base / 'temp' / 'build'
        shutil.rmtree(build)
        build.mkdir()
        return build

    def test_a_first_install_prepares_every_variant(self):
        for later in (False, True):   # the launcher's run screen passed --shaders-later until 1.2.3
            self.options.shaders_later = later
            ctx = self.ctx()
            self.calls.clear()
            record = self.oot.Prepare().run(ctx)
            prepare = next(a for n, a in self.calls if n == 'oot_prepare_shaders.py')
            self.assertNotIn('--met-only', prepare)
            self.assertEqual((record, ctx.state.get('shaders')), ({'shaders': 'all'}, 'all'))
            self.assertEqual([n for n, _ in self.calls], ['oot_prepare_shaders.py', 'pack_shader_cache.py'])   # no folder

    def test_a_folder_that_prepared_the_variants_play_met_prepares_the_rest_from_what_it_holds(self):
        """A folder of 1.2.1-1.2.3 recorded 'met': its next setup run prepares every variant, from a fresh temp folder
        or with the met run's output still in it, and what the game folder holds goes into the build folder first (its
        pack merged in, its pipeline cache and the driver's cache), so the run translates and builds only the rest and
        the package copies back everything the folder held. Another translator's folder gives nothing."""
        build = self.base / 'temp' / 'build'

        def met():
            return {'format': 1, 'release': '1.2.3', 'inputs': {}, 'shaders': 'met', 'steps': {
                'oot_extract': {'key': 'dump-1'},
                'oot_prepare': {'key': self.oot.Prepare().key(self.ctx(), 'met'), 'release': '1.2.3', 'shaders': 'met'}}}
        self.assertEqual(self.oot.shader_case(self.ctx(met())), 'all')
        for leftover in (False, True):
            self.fresh_temp()
            if leftover:
                (build / 'shader-prepared.bin').write_bytes(b'p')
            dest = self.dest_prepared(pack=b'the variants play met')
            ctx, ran = self.run_prepare(met())
            self.assertEqual(ran, ['pack_shader_cache.py', 'oot_prepare_shaders.py', 'pack_shader_cache.py'])
            self.assertEqual(self.calls[0][1], [str(build / 'shader-cache'), '--merge-from',
                                                str(dest / 'shader-cache' / 'shader-cache.pack')])
            self.assertNotIn('--met-only', self.calls[1][1])
            self.assertEqual((build / 'pipeline-cache.bin').read_bytes(), b'the pipelines play met')
            self.assertEqual((build / 'nv-cache' / 'Rekarina.exe.bin').read_bytes(), b'the driver')
            rec = ctx.state['steps']['oot_prepare']
            self.assertEqual((rec['shaders'], ctx.state['shaders']), ('all', 'all'))
            self.assertFalse(self.oot.shaders_only_met(ctx.state))
        self.fresh_temp()
        self.dest_prepared(identity='identity-0')                                 # another translator's folder
        self.assertEqual(self.run_prepare(met())[1], ['oot_prepare_shaders.py', 'pack_shader_cache.py'])
        self.assertFalse((build / 'pipeline-cache.bin').exists())
        self.assertFalse((build / 'nv-cache').exists())

    def test_a_folder_that_holds_every_variant_prepares_nothing(self):
        """A record of every variant whose key stands and a game folder that holds the pack under this translator: the
        step is done with no output in the temp folder (a setup unpacked again, a cleaned temp folder), and the
        package step leaves the folder's pack as it is. 1.2.3 prepared the variants play met there and its package
        replaced a pack of every variant (2 GB) with them (340 MB). A run cut short in the temp folder, another
        translator's folder, a folder without its pack and a changed input prepare every variant."""
        build = self.base / 'temp' / 'build'
        key_all = self.oot.Prepare().key(self.ctx(), 'all')

        def folder():
            return {'format': 1, 'release': '1.2.3', 'inputs': {}, 'shaders': 'all', 'steps': {
                'oot_extract': {'key': 'dump-1'}, 'oot_prepare': {'key': key_all, 'release': '1.2.3', 'shaders': 'all'}}}
        dest = self.dest_prepared()
        self.assertEqual(self.oot.shader_case(self.ctx(folder())), 'rerun')
        ctx, ran = self.run_prepare(folder())
        self.assertEqual(ran, [])
        self.assertEqual(ctx.state['steps']['oot_prepare']['shaders'], 'all')
        self.assertEqual(sorted(p.name for p in build.iterdir()), [])            # nothing for the package to copy
        for leftover in (self.oot.Prepare.PREPARING, self.oot.Prepare.JOURNAL):   # a run cut short
            (build / leftover).write_bytes(b'')
            self.assertEqual(self.oot.shader_case(self.ctx(folder())), 'all')
            self.assertEqual(self.run_prepare(folder())[1], ['pack_shader_cache.py', 'oot_prepare_shaders.py',
                                                             'pack_shader_cache.py'])
            self.fresh_temp()
        self.assertEqual(self.run_prepare(folder())[1], [])
        (dest / 'rekarina_xlate.id').write_text('identity-0')                    # another translator's folder
        self.assertEqual(self.run_prepare(folder())[1], ['oot_prepare_shaders.py', 'pack_shader_cache.py'])
        self.fresh_temp()
        (dest / 'totk_xlate.id').write_text('identity-1')                         # its name before the rename
        (dest / 'rekarina_xlate.id').unlink()
        self.assertEqual(self.run_prepare(folder())[1], [])
        (dest / 'shader-cache' / 'shader-cache.pack').unlink()                    # no pack
        self.assertEqual(self.run_prepare(folder())[1], ['oot_prepare_shaders.py', 'pack_shader_cache.py'])
        self.fresh_temp()
        self.dest_prepared()
        (self.layout.tools / 'prepare_shaders.py').write_text('a change of the inputs')
        self.assertEqual(self.run_prepare(folder())[1], ['pack_shader_cache.py', 'oot_prepare_shaders.py',
                                                         'pack_shader_cache.py'])

    def test_only_met_is_offered_the_rest(self):
        met = {'steps': {'oot_prepare': {'shaders': 'met'}}}
        self.assertTrue(self.oot.shaders_only_met(met))
        self.assertFalse(self.oot.shaders_only_met({**met, 'shaders': 'all'}))
        self.assertFalse(self.oot.shaders_only_met({'steps': {'oot_prepare': {'release': '1.2.0'}}}))   # an old record: all
        self.assertFalse(self.oot.shaders_only_met({'steps': {}}))
        self.assertFalse(self.oot.shaders_only_met(None))

    def test_prompts_draw_the_overlays_and_the_fonts(self):
        ctx = self.ctx()
        step = self.oot.Prompts()
        self.assertFalse(step.current(ctx, {}))
        step.run(ctx)
        self.assertEqual([n for n, _ in self.calls], ['make_oot_prompts.py'] + ['make_ext_font.py'] * len(self.oot.STYLES))

    def test_the_package_follows_the_build_the_shaders_do_not(self):
        ctx = self.ctx()
        ctx.layout.recomp, ctx.layout.patches, ctx.layout.pgo = self.base / 'root', self.base / 'p', None
        ctx.options = SimpleNamespace(dlls=None, all_shaders=False)
        (self.base / 'root' / 'runtime').mkdir()
        ctx.keys.update({'oot_prepare': self.oot.Prepare().key(ctx), 'oot_prompts': 'p', 'oot_lift': 'l', 'oot_compile': 'c'})
        before = self.oot.Package().key(ctx)
        (self.base / 'root' / 'runtime' / 'main.c').write_text('a new runtime')
        self.assertNotEqual(self.oot.Package().key(ctx), before)
        self.assertEqual(self.oot.Prepare().key(ctx), ctx.keys['oot_prepare'])


class LauncherShortcutTest(unittest.TestCase):
    """installer/oot.py launcher_shortcuts: "Rekarina Launcher.lnk" in the game folder and "Rekarina.lnk" in the
    player's Start menu, both to the launcher of the setup that runs, written at every run (the writer is mocked here;
    test_real_shortcut writes one through the shell)."""

    def setUp(self):
        from installer import oot
        self.oot = oot
        self.tmp = tempfile.TemporaryDirectory()
        base = self.base = Path(self.tmp.name)
        self.setup, self.dest = base / 'home' / 'setup' / '1.2.4', base / 'game'
        self.setup.mkdir(parents=True)
        self.dest.mkdir()
        (self.dest / oot.GAME_EXE).write_bytes(b'the game')

    def tearDown(self):
        self.tmp.cleanup()

    def run_it(self, with_launcher=True, desktop=True):
        if with_launcher:
            (self.setup / self.oot.LAUNCHER_EXE).write_bytes(b'launcher')
        ctx = SimpleNamespace(layout=SimpleNamespace(root=self.setup), dest=self.dest, reporter=Reporter(out=io.StringIO()),
                              options=SimpleNamespace(no_desktop_shortcut=not desktop))
        calls = []

        def fake(link, target, workdir, description):
            calls.append((Path(link), Path(target), Path(workdir)))
            Path(link).write_bytes(b'lnk')
            return True, ''
        # the player's own folders are the test's: never the real desktop or Start menu
        with mock.patch.object(self.oot, 'WIN', True), mock.patch.object(self.oot, 'write_shortcut', fake), \
                mock.patch.object(self.oot, 'start_menu_folder', lambda: self.base / 'start'), \
                mock.patch.object(self.oot, 'desktop_folder', lambda: self.base / 'desktop'):
            made = self.oot.launcher_shortcuts(ctx)
        return made, calls

    def test_game_folder_start_menu_desktop(self):
        made, calls = self.run_it()
        launcher = self.setup / self.oot.LAUNCHER_EXE
        links = [self.dest / 'Rekarina Launcher.lnk', self.base / 'start' / 'Rekarina.lnk',
                 self.base / 'desktop' / 'Rekarina Launcher.lnk']
        self.assertEqual(calls, [(link, launcher, self.setup) for link in links])
        self.assertEqual(made, links)

    def test_no_desktop_when_the_form_says_no(self):
        _, calls = self.run_it(desktop=False)
        self.assertEqual([link.parent for link, _, _ in calls], [self.dest, self.base / 'start'])

    def test_newest_setup_wins(self):
        self.run_it()
        self.setup = self.base / 'home' / 'setup' / '1.2.5'   # a setup downloaded anew runs next
        self.setup.mkdir(parents=True)
        _, calls = self.run_it()
        self.assertTrue(all(target == self.setup / self.oot.LAUNCHER_EXE for _, target, _ in calls))

    def test_no_launcher_no_shortcut(self):   # a source checkout
        self.assertEqual(self.run_it(with_launcher=False), ([], []))

    @unittest.skipUnless(sys.platform == 'win32', 'Windows shortcuts')
    def test_real_shortcut(self):
        import subprocess
        target = Path(sys.executable)
        link = self.base / 'Rekarina Launcher.lnk'
        ok, said = self.oot.write_shortcut(link, target, target.parent, 'test')
        self.assertTrue(ok, said)
        read = subprocess.run(['powershell', '-NoProfile', '-NonInteractive', '-Command',
                               '$s = (New-Object -ComObject WScript.Shell).CreateShortcut($env:L); '
                               '$s.TargetPath; $s.WorkingDirectory'], env=dict(__import__('os').environ, L=str(link)),
                              capture_output=True, text=True, timeout=60)
        self.assertEqual([Path(p) for p in read.stdout.split('\n') if p.strip()], [target, target.parent])


class OotComponentsShimTest(unittest.TestCase):
    """Our DLSS Frame Generation library (rekarina_dlssg.dll) reaches the game folder of an RTX 40 or 50 card. Releases
    1.2.0 to 1.2.3 carried none, and the game offered such a card AMD's frame generation only: a release that carries
    it changes the components step's key for those cards, so a folder set up before gets it at its next update, and
    the record names it, so the step runs again when it is gone."""

    def setUp(self):
        from installer import components, oot
        self.oot, self.components = oot, components
        self.tmp = tempfile.TemporaryDirectory()
        base = Path(self.tmp.name)
        self.shim = base / 'bin' / oot.SHIM
        self.shim.parent.mkdir()
        self.dest = base / 'game'
        self.dest.mkdir()
        self.layout = SimpleNamespace(shim=self.shim, release='1.2.4')

    def tearDown(self):
        self.tmp.cleanup()

    def ctx(self, gpu):
        ctx = runner.Context(self.layout, self.dest, Path(self.tmp.name) / 'temp', Reporter(out=io.StringIO()),
                             SimpleNamespace(), state=None)
        ctx.gpu = gpu   # installer/oot.py host_gpu: asked once
        return ctx

    @unittest.skipUnless(sys.platform == 'win32', 'the libraries are Windows ones (components.groups_for)')
    def test_the_shim_is_in_the_key_and_the_record_of_a_card_that_runs_it(self):
        rtx40 = self.components.Gpu('NVIDIA GeForce RTX 4070 SUPER', 'nvidia', 12 << 30)
        rtx30 = self.components.Gpu('NVIDIA GeForce RTX 3080', 'nvidia', 10 << 30)
        step = self.oot.Components()
        without = {g.name: step.key(self.ctx(g)) for g in (rtx40, rtx30)}
        self.shim.write_bytes(b'our library')
        self.assertNotEqual(step.key(self.ctx(rtx40)), without[rtx40.name])   # a release that carries it: once more
        self.assertEqual(step.key(self.ctx(rtx30)), without[rtx30.name])      # a card without it: nothing to do
        (self.dest / 'nvngx_dlssg.dll').write_bytes(b'NVIDIA')
        installed = {'installed': [], 'present': ['nvngx_dlssg.dll'], 'kept': [], 'failed': {}}
        with mock.patch.object(self.components, 'install', return_value=installed):
            record = step.run(self.ctx(rtx40))
        self.assertTrue((self.dest / self.oot.SHIM).is_file())
        self.assertIn(self.oot.SHIM, record['files'])
        self.assertTrue(step.current(self.ctx(rtx40), record))
        (self.dest / self.oot.SHIM).unlink()
        self.assertFalse(step.current(self.ctx(rtx40), record))                # gone: the step runs again


class ToolSteadyTest(unittest.TestCase):
    """installer/oot.py tool_steady: a tool that failed runs again; the same failure twice is the tool's own, failures
    that differ every time are reported as an unstable machine. Real processes of this Python, made to fail."""

    def setUp(self):
        from installer import oot
        self.oot = oot
        self.tmp = tempfile.TemporaryDirectory()
        base = self.base = Path(self.tmp.name)
        self.ctx = SimpleNamespace(temp=base / 'temp', reporter=Reporter(out=io.StringIO(), log_path=base / 'setup.log'),
                                   layout=SimpleNamespace(python=Path(sys.executable), root=base))
        self.count = base / 'count'

    def tearDown(self):
        self.ctx.reporter.log_file.close()
        self.tmp.cleanup()

    def tool(self, body):
        """A tool script that counts its runs in self.count (n: this run's number, from 1) and then runs body."""
        script = self.base / 'tool.py'
        head = ('import os, sys\n'
                'from pathlib import Path\n'
                'c = Path(sys.argv[1])\n'
                'n = int(c.read_text()) + 1 if c.exists() else 1\n'
                'c.write_text(str(n))\n')
        script.write_text(head + body, encoding='utf-8')
        return script

    def test_fails_once_then_works(self):
        script = self.tool('sys.exit(3 if n == 1 else 0)\n')
        code, log = self.oot.tool_steady(self.ctx, 'step', script, [self.count])
        self.assertEqual((code, self.count.read_text()), (0, '2'))
        self.assertTrue(log.with_name(log.stem + '.1.log').is_file())   # the failed run's log is kept

    def test_same_failure_twice_is_the_tools(self):
        script = self.tool('raise ValueError("bad input")\n')
        code, log = self.oot.tool_steady(self.ctx, 'step', script, [self.count])
        self.assertEqual((code, self.count.read_text()), (1, '2'))
        self.assertIn('ValueError: bad input', log.read_text(encoding='utf-8'))

    def test_different_failures_are_unstable(self):
        script = self.tool('raise TypeError(f"run {n}")\n')   # each run ends another way, as the player's did
        with self.assertRaises(self.oot.Unstable) as caught:
            self.oot.tool_steady(self.ctx, 'step', script, [self.count])
        self.assertEqual((caught.exception.tries, caught.exception.tool, self.count.read_text()), (3, 'tool.py', '3'))
        error = caught.exception.error()
        self.assertEqual(error.id, 'oot.unstable')
        from installer.guilogic import Catalog
        self.assertIn('tool.py', Catalog('en')(error.id, **error.params))

    def test_a_crash_leaves_its_stack(self):
        """An access violation in the first run: the exit code says it crashed, faulthandler wrote where."""
        script = self.tool('import faulthandler\n'
                           'def deep():\n'
                           '    faulthandler._read_null()   # a read of address 0, as the player\'s crash was\n'
                           'if n == 1:\n'
                           '    deep()\n')
        code, log = self.oot.tool_steady(self.ctx, 'step', script, [self.count])
        self.assertEqual(code, 0)
        first = log.with_name(log.stem + '.1.log').read_text(encoding='utf-8')
        self.assertIn('access violation', first)
        self.assertIn('tool.py", line 8 in deep', first)
        said = (self.base / 'setup.log').read_text(encoding='utf-8')
        self.assertIn('crashed', said)
        self.assertTrue(self.oot.crashed(int(re.search(r'exit code (-?\d+)', said).group(1))))

    def test_long_failure_runs_once(self):
        script = self.tool('sys.exit(3)\n')
        code, _ = self.oot.tool_steady(self.ctx, 'step', script, [self.count], tries=2, within=-1)
        self.assertEqual((code, self.count.read_text()), (3, '1'))

    def test_env_reaches_the_tool(self):
        script = self.tool('sys.exit(0 if os.environ.get("REKARINA_FONTTOOLS_PURE") == "1" else 5)\n')
        code, _ = self.oot.tool_steady(self.ctx, 'step', script, [self.count], env={'REKARINA_FONTTOOLS_PURE': '1'})
        self.assertEqual(code, 0)


class FontToolsPureTest(unittest.TestCase):
    """tools/fonttools_pure.py: with REKARINA_FONTTOOLS_PURE=1 fontTools' compiled modules are not loaded, its Python
    source is (skipped where fontTools has none compiled)."""

    def test_source_modules(self):
        import subprocess
        probe = ('import sys; sys.path.insert(0, sys.argv[1]); import fonttools_pure; '
                 'import fontTools.misc.bezierTools as b, fontTools.pens.boundsPen; print(b.__file__)')
        try:
            import fontTools.misc.bezierTools as b
        except ImportError:
            self.skipTest('no fontTools')
        if b.__file__.endswith('.py'):
            self.skipTest('this fontTools has no compiled modules')
        for value, compiled in (('1', False), ('', True)):
            env = dict(__import__('os').environ, REKARINA_FONTTOOLS_PURE=value)
            out = subprocess.run([sys.executable, '-c', probe, str(ROOT / 'tools')], capture_output=True, text=True, env=env)
            self.assertEqual(out.returncode, 0, out.stderr)
            self.assertEqual(out.stdout.strip().endswith('.py'), not compiled, out.stdout)


class OotMainTest(unittest.TestCase):
    """installer/oot.py main() up to the steps, started the way the launcher's run screen starts it (--shaders-later) on
    folders that earlier setups made. The estimate before the steps runs before ctx.plan() fills the steps' keys: 1.2.1
    stopped there with a KeyError on every folder whose shaders were prepared all, before its first step. The estimate
    names what the shader step will do: 1.2.2 prepared every variant, for an hour and more under a "2 min" estimate, when
    a setup unpacked again (a fresh temp folder) met a folder that had prepared all. And a failure outside the steps
    reaches the window as an error and the end of the run, with the traceback in the log."""

    def setUp(self):
        from installer import oot
        self.oot = oot
        self.tmp = tempfile.TemporaryDirectory()
        base = self.base = Path(self.tmp.name)
        root = self.root = base / 'release'   # a release folder: VERSION, its own Python, the translator
        python = root / 'python' / ('python.exe' if oot.WIN else 'bin/python3')
        python.parent.mkdir(parents=True)
        python.write_bytes(b'')
        (root / 'VERSION').write_text('1.2.1\n')
        (root / 'bin').mkdir()
        xlate = root / 'bin' / ('rekarina_xlate' + oot.EXE)
        xlate.write_bytes(b'x')
        xlate.with_suffix('.id').write_text('identity-1')
        (root / 'tools' / 'data').mkdir(parents=True)
        (root / 'tools' / 'data' / 'expected-oot.json').write_text(json.dumps({'version': '1.0.0', 'exefs': {'main': '00'}}))
        self.dest, self.temp, self.toolchain = base / 'game', base / 'work', base / 'toolchain'
        self.dest.mkdir()
        # the copy of the translator's identity that the shader step left beside the build (Prepare.adopts)
        self.build = oot.work_folder(None, self.temp, self.dest.resolve())[0] / 'build'
        self.build.mkdir(parents=True)
        (self.build / 'rekarina_xlate.id').write_text('identity-1')
        (self.build / 'shader-prepared.bin').write_bytes(b'p')   # the output of a finished preparation

    def tearDown(self):
        self.tmp.cleanup()

    def start(self, saved, flag='--shaders-later'):
        """main() with the launcher's flag and the steps left out: (exit code, events, what the steps were handed)."""
        if saved is not None:
            (self.dest / state.NAME).write_text(json.dumps(saved), encoding='utf-8')
        seen = {}

        def steps(steps, ctx):
            seen['ctx'], seen['mode'] = ctx, self.oot.shader_mode(ctx)
            return 'ok'
        out, err = io.StringIO(), io.StringIO()
        with mock.patch.object(self.oot, 'run_steps', steps), mock.patch.object(self.oot.eta, 'load_timings', return_value={}), \
                contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = self.oot.main(['--dest', str(self.dest), '--root', str(self.root), '--temp', str(self.temp),
                                  '--toolchain', str(self.toolchain), flag])
        self.stderr = err.getvalue()
        return code, [json.loads(line) for line in out.getvalue().splitlines() if line.startswith('{')], seen

    def prepared_all_key(self):
        """The shader step's key for every variant, computed after the keys of the steps before it exist."""
        layout = self.oot.find_layout(self.root, self.toolchain)
        ctx = runner.Context(layout, self.dest, self.build.parent, Reporter(out=io.StringIO()),
                             SimpleNamespace(all_shaders=True), state=None)
        ctx.keys['oot_extract'] = self.oot.Extract().key(ctx)
        return self.oot.Prepare().key(ctx, 'all')

    def test_every_kind_of_folder_reaches_the_steps_with_the_estimate_of_what_it_prepares(self):
        """Every variant, and the estimate of every variant unless the step has nothing to do: its output in the temp
        folder, or (a fresh temp folder) a game folder that holds the pack under this translator. Whatever the launcher
        passes: --shaders-later (1.2.1-1.2.3) and --all-shaders (since) change nothing."""
        old = {'format': 1, 'release': '1.2.0', 'inputs': {}, 'steps': {   # 1.2.0 wrote no 'shaders' field: all
            'oot_prepare': {'key': 'the old form', 'release': '1.2.0', 'finished': 't', 'seconds': 1.0}}}

        def prepared(shaders, key='k'):
            return {'format': 1, 'release': '1.2.1', 'inputs': {}, **({'shaders': 'all'} if shaders == 'all' else {}),
                    'steps': {'oot_prepare': {'key': key, 'release': '1.2.1', 'shaders': shaders}}}
        unchanged = prepared('all', self.prepared_all_key())
        # (name, state, what the temp folder's build and the game folder hold, the estimate's case of the shader step)
        cases = (
            ('a 1.2.0 folder, the translator that prepared it', old, 'output', 'rerun'),
            ('all, the inputs unchanged', unchanged, 'output', 'rerun'),
            ('all, the inputs unchanged, a fresh temp folder', unchanged, '', 'all'),
            ('all, the inputs unchanged, a fresh temp folder, the pack in the game folder', unchanged, 'folder', 'rerun'),
            ('all, the inputs unchanged, a run cut short', unchanged, 'output marker folder', 'all'),
            ("all, the inputs unchanged, the driver's phase of an earlier setup cut short", unchanged,
             'output journal folder', 'all'),
            ('all, the inputs changed', prepared('all'), 'output folder', 'all'),
            ('the variants play met', prepared('met'), 'output folder', 'all'),
            ('a new folder', None, '', 'all'),
        )
        files = {'output': 'shader-prepared.bin', 'marker': self.oot.Prepare.PREPARING, 'journal': self.oot.Prepare.JOURNAL}
        folder = {'rekarina_xlate.id': b'identity-1', 'shader-prepared.bin': b'p', 'shader-cache/shader-cache.pack': b'a pack'}
        for flag in ('--shaders-later', '--all-shaders'):
            for name, saved, holds, case in cases:
                with self.subTest(name, flag=flag):
                    (self.dest / state.NAME).unlink(missing_ok=True)
                    for what, file in files.items():
                        (self.build / file).unlink(missing_ok=True)
                        if what in holds.split():
                            (self.build / file).write_bytes(b'p')
                    for file, data in folder.items():
                        (self.dest / file).unlink(missing_ok=True)
                        if 'folder' in holds.split():
                            (self.dest / file).parent.mkdir(exist_ok=True)
                            (self.dest / file).write_bytes(data)
                    code, events, seen = self.start(saved, flag)
                    self.assertEqual(code, 0, self.stderr)
                    self.assertEqual([e['type'] for e in events], ['start', 'eta', 'end'])
                    self.assertEqual(events[-1]['result'], 'ok')
                    self.assertEqual(events[1]['steps']['oot_prepare']['case'], case)
                    self.assertEqual(seen['mode'], 'all')
        (self.build / 'shader-prepared.bin').write_bytes(b'p')
        (self.build / 'rekarina_xlate.id').write_text('identity-0')   # another translator prepared the old folder
        self.assertEqual(self.start(old)[1][1]['steps']['oot_prepare']['case'], 'all')

    def test_all_shaders_prepares_all_whatever_the_temp_folder_holds(self):
        """--all-shaders (the launcher's flag after 1.2.3, and installer/shaderjob.py's): every variant, with the
        estimate of every variant unless the step has nothing to do."""
        met = {'format': 1, 'release': '1.2.1', 'inputs': {}, 'steps': {'oot_prepare': {'key': 'k', 'shaders': 'met'}}}
        code, events, seen = self.start(met, '--all-shaders')
        self.assertEqual((code, seen['mode'], events[1]['steps']['oot_prepare']['case']), (0, 'all', 'all'))
        done = {'format': 1, 'release': '1.2.1', 'inputs': {}, 'shaders': 'all',
                'steps': {'oot_prepare': {'key': self.prepared_all_key(), 'shaders': 'all'}}}
        self.assertEqual(self.start(done, '--all-shaders')[1][1]['steps']['oot_prepare']['case'], 'rerun')
        (self.build / 'shader-prepared.bin').unlink()
        self.assertEqual(self.start(done, '--all-shaders')[1][1]['steps']['oot_prepare']['case'], 'all')

    def test_a_failure_before_the_steps_is_reported_with_its_traceback(self):
        with mock.patch.object(self.oot.eta, 'estimates', side_effect=RuntimeError('a broken estimate')):
            code, events, seen = self.start(None)
        self.assertEqual(code, 1)
        self.assertNotIn('ctx', seen)
        error = next(e for e in events if e['type'] == 'error')
        self.assertEqual((error['id'], error['params']['exception'], error['params']['detail']),
                         ('internal', 'RuntimeError', 'a broken estimate'))
        self.assertEqual((events[-1]['type'], events[-1]['result']), ('end', 'failed'))
        log = Path(events[0]['log']).read_text(encoding='utf-8')
        self.assertIn('Traceback', log)
        self.assertIn('a broken estimate', log)

    def test_a_failure_before_the_log_is_reported_too(self):
        with mock.patch.object(self.oot, 'find_layout', side_effect=OSError('no layout')):
            code, events, _ = self.start(None)
        self.assertEqual(code, 1)
        self.assertEqual([(e['type'], e.get('id'), e.get('result')) for e in events],
                         [('error', 'internal', None), ('end', None, 'failed')])
        self.assertIn('Traceback', self.stderr)   # no log file yet: the traceback goes to stderr, which the window logs


class ShaderJobTest(unittest.TestCase):
    """installer/shaderjob.py with a stand-in for the setup: start, stop, resume from the built count, the game's name."""

    SETUP = """import json, os, sys, time
f = sys.argv[1]
n = int(open(f).read()) if os.path.exists(f) else 0
e = lambda **k: print(json.dumps(k), flush=True)
e(type='progress', step='oot_prepare', phase='libraries', done=n, total=100, unit='shaders')
while n < 100:
    time.sleep(0.02); n += 1; open(f, 'w').write(str(n))
    if n % 10 == 0: e(type='progress', step='oot_prepare', phase='libraries', done=n, total=100, unit='shaders')
e(type='end', result='ok')
"""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        (self.dir / 'setup.py').write_text(self.SETUP)
        self.built = self.dir / 'built.txt'

    def tearDown(self):
        self.tmp.cleanup()

    def job(self):
        from installer import shaderjob
        return shaderjob.ShaderJob([sys.executable, str(self.dir / 'setup.py'), str(self.built)], str(self.dir))

    def until(self, job, test, seconds=30):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            job.poll()
            if test():
                return True
            time.sleep(0.02)
        return False

    def test_stop_and_resume_go_on_from_what_is_built(self):
        job = self.job()
        self.assertTrue(job.start())
        self.assertTrue(self.until(job, lambda: (job.done or 0) >= 30))
        job.stop()
        self.assertEqual(job.state, 'idle')
        built = int(self.built.read_text())
        self.assertTrue(30 <= built < 100)
        self.assertTrue(job.start())
        self.assertTrue(self.until(job, lambda: job.done is not None))
        self.assertGreaterEqual(job.done, built)                                    # not from the start again
        self.assertTrue(self.until(job, lambda: job.state == 'done'))
        self.assertEqual((job.done, job.total), (100, 100))
        self.assertFalse(job.start())                                               # done: not run again

    def test_a_pause_holds_until_resumed_and_a_failure_is_not_retried(self):
        job = self.job()
        job.pause()
        self.assertFalse(job.start())
        job.resume()
        self.assertTrue(job.start())
        job.stop()
        (self.dir / 'setup.py').write_text('import sys; sys.exit(3)')
        job = self.job()
        job.start()
        self.assertTrue(self.until(job, lambda: job.state == 'failed'))
        self.assertFalse(job.start())

    def test_games_are_the_processes_of_that_name_outside_the_job(self):
        from installer import shaderjob
        table = {10: (1, 'explorer.exe'), 20: (10, 'launcher.exe'), 30: (20, 'python.exe'), 31: (30, 'Rekarina.exe'),
                 40: (1, 'rekarina.exe')}
        self.assertEqual(shaderjob.games_running('Rekarina.exe', 30, table), [40])   # 31 is the job's own warm-up
        self.assertEqual(shaderjob.games_running('Rekarina.exe', None, table), [31, 40])


class LooseShaderFilesTest(unittest.TestCase):
    """installer/oot.py has_loose_shader_files: what tells the package step that a folder's cache still needs packing."""

    def test_cases(self):
        from installer import oot
        with tempfile.TemporaryDirectory() as t:
            cache = Path(t) / 'shader-cache'
            self.assertFalse(oot.has_loose_shader_files(cache))                    # no cache at all
            cache.mkdir()
            (cache / 'shader-cache.pack').write_bytes(b'TKSPACK1')
            (cache / 'in').mkdir()
            self.assertFalse(oot.has_loose_shader_files(cache))                    # a pack only: a new install
            (cache / 'in' / ('ab' * 32 + '.in')).write_bytes(b'x')
            self.assertTrue(oot.has_loose_shader_files(cache))                     # an input left beside it
            (cache / 'in' / ('ab' * 32 + '.in')).unlink()
            (cache / ('cd' * 32 + '.res.json')).write_text('{}')
            self.assertTrue(oot.has_loose_shader_files(cache))                     # a folder of an earlier release


class EventsTest(unittest.TestCase):
    def test_lines_are_json_and_the_log_is_english(self):
        with tempfile.TemporaryDirectory() as d:
            out = io.StringIO()
            rep = Reporter(out=out, log_path=Path(d) / 'x.log')
            rep.step('check', 'started')
            rep.progress('extract', 'copy', 0, 10, 'bytes')
            rep.progress('extract', 'copy', 5, 10, 'bytes')    # throttled
            rep.progress('extract', 'copy', 10, 10, 'bytes')   # last one always goes out
            rep.error('check.disk_space', {'path': 'D:\\x', 'need_gb': 40, 'free_gb': 3}, step='check')
            rep.close()
            events = [json.loads(line) for line in out.getvalue().splitlines()]
            self.assertEqual([e['type'] for e in events], ['step', 'progress', 'progress', 'error'])
            self.assertEqual([e.get('done') for e in events[1:3]], [0, 10])
            self.assertTrue(all('t' in e for e in events))
            log = (Path(d) / 'x.log').read_text(encoding='utf-8')
            self.assertIn('40 GB needed, 3 GB free', log)
            self.assertIn('[check.disk_space]', log)

    def test_catalog_has_every_id_the_code_uses(self):
        catalog = json.loads((ROOT / 'installer' / 'text' / 'en.json').read_text(encoding='utf-8'))
        used = set()
        for path in (ROOT / 'installer').glob('*.py'):
            text = path.read_text(encoding='utf-8')
            used |= set(re.findall(r"InstallError\('([a-z_.]+)'", text))
            used |= set(re.findall(r"warning\('([a-z_.]+)'", text))
            used |= {f'phase.{s}.{p}' for s, p in re.findall(r"progress\('(\w+)', '(\w+)'[,)]", text)}
        self.assertEqual(sorted(i for i in used if i not in catalog), [])


@unittest.skipUnless(steps.os.name == 'nt', 'DLSS is Windows-only')
class DlssTest(unittest.TestCase):
    """The DLSS step: off without --dlss, needs an RTX GPU, keeps a player's library, verifies the download."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        base = Path(self.tmp.name)
        self.dest, temp = base / 'dest', base / 'temp'
        self.dest.mkdir()
        temp.mkdir()
        self.src = base / 'nvngx_dlss.dll'
        self.src.write_bytes(b'dlss' * 1000)
        self.saved = steps.DLSS_URL, steps.DLSS_SIZE, steps.DLSS_SHA256
        steps.DLSS_URL = self.src.as_uri()   # urllib reads file: URLs as it reads https: ones
        steps.DLSS_SIZE, steps.DLSS_SHA256 = 4000, steps.file_sha256(self.src)
        rtx = {'devices': [{'name': 'NVIDIA GeForce RTX 4080 SUPER', 'api': [1, 4, 0], 'type': 2}]}
        self.ctx = SimpleNamespace(dest=self.dest, temp=temp, host=rtx, reporter=Reporter(out=io.StringIO()),
                                   options=SimpleNamespace(dlss=True))

    def tearDown(self):
        steps.DLSS_URL, steps.DLSS_SIZE, steps.DLSS_SHA256 = self.saved
        self.ctx.reporter.close()
        self.tmp.cleanup()

    def warnings(self):
        return [json.loads(line)['id'] for line in self.ctx.reporter.out.getvalue().splitlines()
                if json.loads(line)['type'] == 'warning']

    def test_off_without_the_option(self):
        self.ctx.options.dlss = False
        self.assertEqual(steps.Dlss().run(self.ctx), {})
        self.assertFalse((self.dest / 'nvngx_dlss.dll').exists())
        self.assertNotEqual(steps.Dlss().key(self.ctx), steps.Dlss().key(SimpleNamespace(options=SimpleNamespace(dlss=True))))

    def test_downloads_and_checks_the_library(self):
        rec = steps.Dlss().run(self.ctx)
        self.assertEqual(rec, {'installed': steps.DLSS_VERSION})
        self.assertEqual((self.dest / 'nvngx_dlss.dll').read_bytes(), self.src.read_bytes())
        self.assertFalse(list(self.ctx.temp.iterdir()))
        self.assertTrue(steps.Dlss().current(self.ctx, rec))
        (self.dest / 'nvngx_dlss.dll').unlink()
        self.assertFalse(steps.Dlss().current(self.ctx, rec))

    def test_a_wrong_file_is_not_installed_and_tried_again(self):
        steps.DLSS_SHA256 = '0' * 64
        rec = steps.Dlss().run(self.ctx)
        self.assertEqual(rec, {'failed': True})
        self.assertFalse((self.dest / 'nvngx_dlss.dll').exists())
        self.assertEqual(self.warnings(), ['warn.dlss_download_failed'])
        self.assertFalse(steps.Dlss().current(self.ctx, rec))

    def test_a_cut_connection_warns(self):
        class Cut(io.BytesIO):   # a response whose connection drops mid-file
            def read(self, n=-1):
                raise http.client.IncompleteRead(b'dl', 3998)

        with mock.patch.object(steps.urllib.request, 'urlopen', return_value=Cut()):
            self.assertEqual(steps.Dlss().run(self.ctx), {'failed': True})
        self.assertFalse((self.dest / 'nvngx_dlss.dll').exists())
        self.assertFalse(list(self.ctx.temp.iterdir()))
        self.assertEqual(self.warnings(), ['warn.dlss_download_failed'])

    def test_keeps_the_players_library(self):
        (self.dest / 'nvngx_dlss.dll').write_bytes(b'newer')
        self.assertEqual(steps.Dlss().run(self.ctx), {'kept': True})
        self.assertEqual((self.dest / 'nvngx_dlss.dll').read_bytes(), b'newer')
        self.assertEqual(self.warnings(), ['warn.dlss_kept'])

    def test_skipped_without_an_rtx_gpu(self):
        self.ctx.host = {'devices': [{'name': 'AMD Radeon RX 7800 XT', 'api': [1, 3, 0], 'type': 2}]}
        self.assertEqual(steps.Dlss().run(self.ctx), {'skipped': 'no_rtx'})
        self.assertEqual(self.warnings(), [])


if __name__ == '__main__':
    unittest.main()
