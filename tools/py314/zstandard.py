"""The part of the zstandard package Rekarina's tools and the vendored NUP reader use, on Python 3.14's own
compression.zstd.

The release folder's Python is 3.14 with no zstandard package; tools/oot_romfs.py, tools/oot_exefs.py and
tools/make_oot_prompts.py put this folder on sys.path when `import zstandard` fails, and tools/vendor/nup (kept
byte-identical to upstream) then finds this module under the package's name. Only what is called here exists:
ZstdDecompressor (decompress, stream_reader), ZstdCompressor (compress), ZstdCompressionDict (dict_id) and
get_frame_parameters (dict_id, content_size).
"""
from collections import namedtuple

from compression import zstd as _z

CONTENTSIZE_UNKNOWN = -1
FrameParameters = namedtuple('FrameParameters', 'content_size dict_id')
ZstdError = _z.ZstdError


class ZstdCompressionDict:
    def __init__(self, data, dict_type=None):
        self._d = _z.ZstdDict(bytes(data))

    def dict_id(self):
        return self._d.dict_id


def get_frame_parameters(data):
    info = _z.get_frame_info(bytes(data[:64]))
    size = info.decompressed_size
    return FrameParameters(CONTENTSIZE_UNKNOWN if size is None else size, info.dictionary_id)


class _Reader:
    """stream_reader's result: read(n) gives the next n decompressed bytes."""

    def __init__(self, source, zstd_dict):
        self._data = source.read()
        self._d = _z.ZstdDecompressor(zstd_dict=zstd_dict)
        self._fed = False

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False

    def read(self, n=-1):
        data, self._fed = (b'' if self._fed else self._data), True
        return self._d.decompress(data, max_length=n)


class ZstdDecompressor:
    def __init__(self, dict_data=None):
        self._d = dict_data._d if dict_data else None

    def decompress(self, data, max_output_size=0):
        out = _z.decompress(bytes(data), zstd_dict=self._d)
        if max_output_size and len(out) > max_output_size:
            raise ZstdError(f'the frame decompresses to {len(out)} bytes, over the limit of {max_output_size}')
        return out

    def stream_reader(self, source):
        return _Reader(source, self._d)


class ZstdCompressor:
    def __init__(self, level=3, dict_data=None):
        self._level, self._d = level, dict_data._d if dict_data else None

    def compress(self, data):
        return _z.compress(bytes(data), level=self._level, zstd_dict=self._d)
