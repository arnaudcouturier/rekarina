"""Read OoT's RomFS straight from the player's NUP: files by path, and the game's .zs files decompressed
with its zstd dictionaries (Pack/ZsDic.pack.zs, a SARC of dictionaries, as in TotK).

    from oot_romfs import RomFS
    fs = RomFS(nup); data = fs.read('Font2/Scalable/External.Nin_Ounce_NVN2.bfotf.zs', unzs=True)

The dump is named by the caller (--nup of the tools that read it); REKARINA_NUP in the environment is the default.
"""
import os
import struct
import sys
from pathlib import Path

try:
    import zstandard
except ImportError:   # the release's Python 3.14: its own compression.zstd behind the package's name (tools/py314)
    sys.path.insert(0, str(Path(__file__).resolve().parent / 'py314'))
    import zstandard

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools' / 'vendor' / 'nup'))   # the NUP reference reader (THIRD_PARTY_NOTICES.md)
from nup.read import NupReader  # noqa: E402

NUP = os.environ.get('REKARINA_NUP', '')   # the default of the tools' --nup: the player's dump, a .nup file


def sarc_members(b):
    """{name: (start, end)} of a SARC archive (little-endian, as Switch games write it)."""
    assert b[:4] == b'SARC', b[:4]
    hdr = struct.unpack_from('<H', b, 4)[0]
    data_off = struct.unpack_from('<I', b, 0xC)[0]
    sfat = hdr
    assert b[sfat:sfat + 4] == b'SFAT'
    count = struct.unpack_from('<H', b, sfat + 6)[0]
    nodes = [struct.unpack_from('<IIII', b, sfat + 0xC + 0x10 * i) for i in range(count)]
    sfnt = sfat + 0xC + 0x10 * count
    assert b[sfnt:sfnt + 4] == b'SFNT'
    names_off = sfnt + 8
    out = {}
    for _hash, attr, start, end in nodes:
        name_off = names_off + (attr & 0xFFFF) * 4
        name = b[name_off:b.index(b'\0', name_off)].decode('utf-8')
        out[name] = (data_off + start, data_off + end)
    return out


class RomFS:
    def __init__(self, nup=NUP):
        if not nup:
            sys.exit('oot_romfs: no dump named: pass --nup <file.nup> or set REKARINA_NUP')
        self.r = NupReader(nup)
        nd = len(self.r.dirs)
        self.index = {}

        def walk(d, prefix):
            _name, first, count = self.r.dirs[d]
            for node in self.r.children[first:first + count]:
                if node < nd:
                    walk(node, prefix + self.r.dirs[node][0].decode() + '/')
                else:
                    self.index[prefix + self.r.files[node - nd][0].decode()] = node - nd
        for package in self.r.packages:   # base first, then updates: a later package's file replaces the earlier one
            walk(package.romfs_dir, '')
        self._dicts = None

    def read_raw(self, path):
        return self.r.read_file(self.index[path])

    def dicts(self):
        if self._dicts is None:
            pack = zstandard.ZstdDecompressor().decompress(self.read_raw('Pack/ZsDic.pack.zs'),
                                                            max_output_size=1 << 28)
            self._dicts = {}
            for name, (lo, hi) in sarc_members(pack).items():
                d = zstandard.ZstdCompressionDict(pack[lo:hi])
                self._dicts[d.dict_id()] = d
        return self._dicts

    def unzs(self, raw):
        did = zstandard.get_frame_parameters(raw).dict_id
        dctx = zstandard.ZstdDecompressor(dict_data=self.dicts()[did]) if did else zstandard.ZstdDecompressor()
        return dctx.decompress(raw, max_output_size=1 << 30)

    def read(self, path, unzs=None):
        raw = self.read_raw(path)
        return self.unzs(raw) if (unzs or (unzs is None and path.endswith('.zs'))) else raw
