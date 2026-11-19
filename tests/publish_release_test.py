"""website/publish_release.py without the network: the Markdown the release notes use, the release history (a new
version is added, the same release again changes nothing, a published version is never replaced) and the releases
page made from it. Stdlib only.

Run: python tests/publish_release_test.py
"""
import datetime
import hashlib
import html
import json
import re
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'website'))
import publish_release as pr  # noqa: E402


def entry(version, exe='a' * 64, date='2026-11-19', notes='## Rekarina\n\n- one'):
    return {'version': version, 'date': date, 'notes': notes,
            'files': [{'name': 'Rekarina.exe', 'size': 155_219_533, 'sha256': exe},
                      {'name': f'Rekarina-Setup-{version}-windows.zip', 'size': 154_787_321, 'sha256': 'b' * 64},
                      {'name': 'SHA256SUMS', 'size': 178, 'sha256': 'c' * 64}]}


class MarkdownTest(unittest.TestCase):
    def test_heading_is_below_the_pages_own(self):
        self.assertEqual(pr.markdown('## Rekarina 1.2.3: fixed'), '<h3>Rekarina 1.2.3: fixed</h3>')
        self.assertEqual(pr.markdown('# Top'), '<h3>Top</h3>')
        self.assertEqual(pr.markdown('### Smaller'), '<h4>Smaller</h4>')
        self.assertEqual(pr.markdown('#hashtag'), '<p>#hashtag</p>')

    def test_paragraph_lines_join_until_a_blank_line(self):
        self.assertEqual(pr.markdown('one\ntwo\n\nthree'), '<p>one two</p>\n<p>three</p>')

    def test_list_with_continued_items(self):
        text = '- first\n  goes on\n* second\n\nafter'
        self.assertEqual(pr.markdown(text),
                         '<ul>\n  <li>first goes on</li>\n  <li>second</li>\n</ul>\n<p>after</p>')

    def test_list_right_after_a_paragraph_and_a_paragraph_right_after_a_list(self):
        self.assertEqual(pr.markdown('**New**\n- a\n- b\nFixed\n- c'),
                         '<p><b>New</b></p>\n<ul>\n  <li>a</li>\n  <li>b</li>\n</ul>\n<p>Fixed</p>\n'
                         '<ul>\n  <li>c</li>\n</ul>')

    def test_bold_and_code(self):
        self.assertEqual(pr.inline('a **bold** and `x <y> **z**`'),
                         'a <b>bold</b> and <code>x &lt;y&gt; **z**</code>')
        self.assertEqual(pr.inline('**a** and **b**'), '<b>a</b> and <b>b</b>')

    def test_everything_else_is_text(self):
        self.assertEqual(pr.inline('<script>alert(1)</script> & [link](https://x)'),
                         '&lt;script&gt;alert(1)&lt;/script&gt; &amp; [link](https://x)')
        self.assertEqual(pr.inline('2 * 3 ** 4'), '2 * 3 ** 4')
        self.assertEqual(pr.markdown('\r\n- crlf\r\n'), '<ul>\n  <li>crlf</li>\n</ul>')

    def test_published_notes_keep_every_word(self):
        """Each published release's notes: every word is on the page and no Markdown mark is left over."""
        history = pr.load_history(ROOT / 'website' / 'public' / 'releases' / 'releases.json')
        self.assertTrue(history)
        for e in history:
            out = pr.markdown(e['notes'])
            self.assertNotIn('**', out, e['version'])
            self.assertNotRegex(out, r'(?m)^<p>[-*#] ', e['version'])
            text = html.unescape(re.sub(r'<[^>]+>', ' ', out))
            for word in re.findall(r'\w+', e['notes']):
                self.assertIn(word, text, e['version'])


class HistoryTest(unittest.TestCase):
    def test_new_version_is_added(self):
        history, added = pr.add_release([entry('1.2.3')], entry('1.2.4', exe='d' * 64))
        self.assertTrue(added)
        self.assertEqual([e['version'] for e in history], ['1.2.3', '1.2.4'])

    def test_same_release_again_changes_nothing(self):
        old = [entry('1.2.3', date='2026-11-19')]
        history, added = pr.add_release(old, entry('1.2.3', date='2026-11-21', notes='other words'))
        self.assertFalse(added)
        self.assertEqual(history, old)
        self.assertEqual(history[0]['date'], '2026-11-19')

    def test_published_version_is_not_replaced(self):
        with self.assertRaises(ValueError):
            pr.add_release([entry('1.2.3')], entry('1.2.3', exe='e' * 64))

    def test_entry_from_a_release_folder(self):
        with tempfile.TemporaryDirectory() as tmp:
            dist = Path(tmp)
            names = ['Rekarina.exe', 'Rekarina-Setup-9.9.9-windows.zip', 'SHA256SUMS', 'SHA256SUMS.sig']
            for n, name in enumerate(names):
                (dist / name).write_bytes(b'x' * (n + 1))
            sums = {'Rekarina.exe': '1' * 64, 'Rekarina-Setup-9.9.9-windows.zip': '2' * 64}
            e = pr.history_entry(dist, '9.9.9', names, sums, {'body': 'notes'}, datetime.date(2026, 11, 20))
        self.assertEqual(e['date'], '2026-11-20')
        self.assertEqual(e['notes'], 'notes')
        self.assertEqual([f['name'] for f in e['files']], names[:3])
        self.assertEqual(e['files'][0], {'name': 'Rekarina.exe', 'size': 1, 'sha256': '1' * 64})
        self.assertEqual(e['files'][2], {'name': 'SHA256SUMS', 'size': 3, 'sha256': hashlib.sha256(b'xxx').hexdigest()})

    def test_written_history_and_page(self):
        with tempfile.TemporaryDirectory() as tmp:
            page = Path(tmp) / 'index.html'
            page.write_text('<main>\n        <!-- release:list -->\n        <!-- /release:list -->\n</main>\n',
                            encoding='utf-8')
            with mock.patch.object(pr, 'HISTORY', Path(tmp) / 'releases.json'), \
                    mock.patch.object(pr, 'RELEASES_PAGE', page):
                history = [entry('1.2.10', exe='f' * 64), entry('1.2.9')]
                pr.write_history(history)
                self.assertEqual(pr.load_history(Path(tmp) / 'releases.json'), history)
                pr.write_history(history)   # rendering again replaces the block, it does not add to it
            out = page.read_text(encoding='utf-8')
        self.assertEqual(out.count('<article'), 2)
        self.assertLess(out.index('id="v1.2.10"'), out.index('id="v1.2.9"'))   # versions sort as numbers
        self.assertIn('<details class="notes" open>', out.split('id="v1.2.9"')[0])
        self.assertNotIn(' open>', out.split('id="v1.2.9"')[1])
        self.assertIn('https://dl.rekarina.dev/releases/1.2.9/Rekarina-Setup-1.2.9-windows.zip', out)
        self.assertIn('https://dl.rekarina.dev/releases/1.2.9/SHA256SUMS', out)
        self.assertIn(f'<code>{"f" * 64}</code>', out)
        self.assertIn('19 November 2026', out)
        self.assertIn('155 MB', out)


class SiteTest(unittest.TestCase):
    def test_releases_is_the_page_not_a_redirect(self):
        redirects = (ROOT / 'website' / 'public' / '_redirects').read_text(encoding='utf-8')
        self.assertNotRegex(redirects, r'(?m)^/releases')
        self.assertIn('release:list', (ROOT / 'website' / 'public' / 'releases' / 'index.html').read_text(encoding='utf-8'))
        self.assertIn('href="/releases/"', (ROOT / 'website' / 'public' / 'index.html').read_text(encoding='utf-8'))


if __name__ == '__main__':
    unittest.main()
