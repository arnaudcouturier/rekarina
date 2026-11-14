"""Write an ExeFS whose NSO modules are stored uncompressed, for the runtime's loader (runtime/loader.c reads LZ4 only).

Switch 2 modules from system 22.0.0 on may store their segments as ZBIC (NSO flag bit 7, the platform's zstd variant;
OoT's main). recomp/nso.py decodes it with the zbic package; the C loader does not, so the setup writes
each module once with its segments uncompressed: flags bits 0-2 and 7 cleared, every segment at a file offset of its
own with its stored size equal to its size. The segment hashes (flags bits 3-5, header 0xA0) are of the decompressed
bytes and stay valid; the lifted code and the module layout depend only on those bytes, so nothing else changes.
Files other than NSO modules are copied.

    python tools/nso_plain.py <exefs> <out>
"""
import shutil
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'recomp'))
from nso import NSO_ZBIC, zbic_segment  # noqa: E402

import lz4.block  # noqa: E402


def plain(raw, name):
    flags = struct.unpack_from('<I', raw, 0xC)[0]
    first = min(struct.unpack_from('<I', raw, 0x10 + i * 0x10)[0] for i in range(3))
    segs, hdr = [], bytearray(raw[:first])   # the header and the module name after it (+0x1C / +0x2C)
    for i in range(3):
        file_off, mem_off, size = struct.unpack_from('<III', raw, 0x10 + i * 0x10)
        stored = struct.unpack_from('<I', raw, 0x60 + i * 4)[0]
        data = raw[file_off:file_off + stored]
        if flags & (1 << i):
            data = zbic_segment(data, size, name, i) if flags & NSO_ZBIC else lz4.block.decompress(data, uncompressed_size=size)
        if len(data) != size:
            raise SystemExit(f'{name}: segment {i} is {len(data):#x} bytes, header says {size:#x}')
        segs.append((mem_off, bytes(data)))
    out = bytearray(hdr)
    for i, (mem_off, data) in enumerate(segs):
        off = len(out)
        struct.pack_into('<III', out, 0x10 + i * 0x10, off, mem_off, len(data))
        struct.pack_into('<I', out, 0x60 + i * 4, len(data))
        out += data
    struct.pack_into('<I', out, 0xC, flags & ~(0x7 | NSO_ZBIC))
    return bytes(out)


def main():
    src, dst = Path(sys.argv[1]), Path(sys.argv[2])
    dst.mkdir(parents=True, exist_ok=True)
    for f in sorted(src.iterdir()):
        raw = f.read_bytes()
        if raw[:4] == b'NSO0':
            (dst / f.name).write_bytes(plain(raw, f.name))
            print(f'{f.name}: {len(raw):,} -> {(dst / f.name).stat().st_size:,} bytes (uncompressed)')
        else:
            shutil.copyfile(f, dst / f.name)
            print(f'{f.name}: copied')


if __name__ == '__main__':
    main()
