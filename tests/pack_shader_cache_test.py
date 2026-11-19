"""tools/pack_shader_cache.py on a fake shader cache: the pack layout gfx_shader.c reads (sorted index, bounds, data),
loose files folded in and deleted, a second run merging new loose files, a loose file winning over its packed copy,
malformed files skipped and --keep. No game files; stdlib only.

Run: python tests/pack_shader_cache_test.py
"""
import hashlib
import struct
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import pack_shader_cache as psc  # noqa: E402

USE = struct.pack('<5I', 0x4d554b54, 1, 3, 0, 1)   # gfx_shader.c use_store


def name(seed):
    return hashlib.sha256(seed.encode()).hexdigest()


def read_pack(path):
    """{(kind, name hex): the file's bytes}, checking what gfx_shader.c pack_open checks; a compressed entry is
    given unpacked."""
    b = path.read_bytes()
    assert b[:8] == psc.MAGIC
    count = struct.unpack_from('<I', b, 8)[0]
    out, last = {}, None
    for i in range(count):
        nm, word, size, offset = struct.unpack_from('<32sIIQ', b, 16 + 48 * i)
        kind = word & 0xff
        assert word & ~(0xff | psc.ZSTD) == 0
        assert offset + size <= len(b) and offset >= 16 + 48 * count
        assert last is None or (last[0], last[1]) < (kind, nm), 'index not sorted by (kind, name)'
        last = (kind, nm)
        data = b[offset:offset + size]
        out[(kind, nm.hex())] = psc.zstandard.ZstdDecompressor().decompress(data) if word & psc.ZSTD else data
    return out


def stored_kind(path, kind, nm):
    """The kind word of one entry as the pack has it."""
    b = path.read_bytes()
    for i in range(struct.unpack_from('<I', b, 8)[0]):
        n, word, _size, _offset = struct.unpack_from('<32sIIQ', b, 16 + 48 * i)
        if n.hex() == nm and word & 0xff == kind:
            return word
    return None


class PackTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.cache = Path(self.tmp.name) / 'shader-cache'
        (self.cache / 'in').mkdir(parents=True)

    def tearDown(self):
        self.tmp.cleanup()

    def shader(self, seed, spv=b'\x03\x02\x23\x07spirv', res=b'{"x":1}'):
        k = name(seed)
        (self.cache / f'{k}.spv').write_bytes(spv + seed.encode())
        (self.cache / f'{k}.res.json').write_bytes(res)
        (self.cache / f'{name("use" + seed)}.use').write_bytes(USE)
        (self.cache / 'in').mkdir(exist_ok=True)   # in_store makes it again after a pack removed it
        (self.cache / 'in' / f'{name("in" + seed)}.in').write_bytes(b'TKSI' + seed.encode())
        return k

    def test_pack_holds_every_file_and_deletes_the_loose_ones(self):
        keys = [self.shader(str(i)) for i in range(50)]
        (self.cache / 'notes.txt').write_text('kept')
        (self.cache / f'{keys[0]}.code').write_bytes(b'kept too')
        n, new, skipped, _ = psc.pack(self.cache)
        self.assertEqual((n, new, skipped), (200, 200, 0))
        got = read_pack(self.cache / psc.PACK)
        self.assertEqual(got[(1, keys[3])], b'\x03\x02\x23\x07spirv3')
        self.assertEqual(got[(2, keys[3])], b'{"x":1}')
        self.assertEqual(got[(3, name('use3'))], USE)
        self.assertEqual(got[(4, name('in3'))], b'TKSI3')
        left = sorted(p.name for p in self.cache.iterdir())
        self.assertEqual(left, sorted([psc.PACK, 'notes.txt', f'{keys[0]}.code']))   # in/ removed when empty

    def test_second_run_merges_new_loose_files_and_loose_wins(self):
        a = self.shader('a')
        psc.pack(self.cache)
        b = self.shader('b')
        (self.cache / f'{a}.res.json').write_bytes(b'{"newer":1}')
        n, new, _, _ = psc.pack(self.cache)
        got = read_pack(self.cache / psc.PACK)
        self.assertEqual(n, 8)
        self.assertEqual(new, 5)
        self.assertEqual(got[(2, a)], b'{"newer":1}')
        self.assertEqual(got[(1, b)], b'\x03\x02\x23\x07spirvb')

    def test_malformed_files_are_skipped_or_replaced_by_the_packed_copy(self):
        a = self.shader('a')
        psc.pack(self.cache)
        (self.cache / f'{name("usea")}.use').write_bytes(b'short')   # bad loose copy: the packed one stays
        (self.cache / f'{name("bad")}.use').write_bytes(b'short')    # bad and nowhere else: skipped
        (self.cache / f'{name("empty")}.spv').write_bytes(b'')
        n, _, skipped, _ = psc.pack(self.cache)
        got = read_pack(self.cache / psc.PACK)
        self.assertEqual(got[(3, name('usea'))], USE)
        self.assertNotIn((3, name('bad')), got)
        self.assertEqual((n, skipped), (4, 2))
        self.assertTrue((self.cache / f'{name("bad")}.use').exists())   # never deleted unless packed

    def test_keep_leaves_the_loose_files(self):
        k = self.shader('a')
        psc.pack(self.cache, keep=True)
        self.assertTrue((self.cache / f'{k}.spv').exists())
        self.assertEqual(len(read_pack(self.cache / psc.PACK)), 4)

    def test_a_broken_pack_is_rebuilt_from_the_loose_files(self):
        self.shader('a')
        (self.cache / psc.PACK).write_bytes(b'garbage')
        n, _, _, _ = psc.pack(self.cache)
        self.assertEqual(n, 4)

    def test_a_large_shader_is_stored_compressed_and_a_small_file_as_it_is(self):
        big = b'#' + bytes(range(64)) * 400          # 25 KiB that compress well
        k = self.shader('big', spv=big)
        _, _, _, size = psc.pack(self.cache)
        got = read_pack(self.cache / psc.PACK)
        self.assertEqual(got[(1, k)], big + b'big')
        self.assertEqual(stored_kind(self.cache / psc.PACK, 1, k), 1 | psc.ZSTD)
        self.assertEqual(stored_kind(self.cache / psc.PACK, 2, k), 2)        # 7 bytes: as it is
        self.assertEqual(stored_kind(self.cache / psc.PACK, 3, name('usebig')), 3)
        self.assertLess(size, len(big) // 4)

    def test_a_pack_of_the_layout_before_is_written_anew_compressed(self):
        big = b'#' + bytes(range(64)) * 400
        files = {(1, name('old')): big, (2, name('old')): b'{"x":1}', (3, name('useold')): USE}
        keys = sorted((kind, bytes.fromhex(nm)) for kind, nm in files)
        offset, index, data = 16 + 48 * len(keys), [], b''
        for kind, nm in keys:
            d = files[(kind, nm.hex())]
            index.append(struct.pack('<32sIIQ', nm, kind, len(d), offset + len(data)))
            data += d
        (self.cache / psc.PACK).write_bytes(psc.MAGIC_PLAIN + struct.pack('<II', len(keys), 0) + b''.join(index) + data)
        n, new, skipped, size = psc.pack(self.cache)                           # no loose file: converted all the same
        self.assertEqual((n, new, skipped), (3, 0, 0))
        got = read_pack(self.cache / psc.PACK)
        self.assertEqual(got[(1, name('old'))], big)
        self.assertEqual(got[(3, name('useold'))], USE)
        self.assertLess(size, len(big) // 4)
        self.assertEqual(psc.pack(self.cache)[:3], (3, 0, 0))                  # and left alone the next time


if __name__ == '__main__':
    unittest.main()
