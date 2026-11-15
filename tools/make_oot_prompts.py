"""RomFS overlays for OoT (Switch 2): romfs-patches, romfs-patches-xbox-label and romfs-patches-xbox.

Stand-ins for damaged files: a dump can hold files whose first sectors read as zeros (the player's 1.1.1 dump has
three .bntx.zs textures whose first 12 KiB are zero, so they are no zstd frame and cannot be recovered). The game
retries such a load without end and then reads a resource slot that stayed empty: the camera turning towards the
fairy-fountain floor in the hero's house crashed in main+0xc938d0. Each damaged texture is replaced by a sibling of the
same kind whose texture name has the same length, renamed inside (STANDINS); every overlay folder carries them,
romfs-patches (Nintendo prompts) only them. tools/make_oot_prompts.py --scan lists .zs files without a zstd header.

Xbox button prompts:

Every button icon in the game's text is a glyph of Font2/Scalable/External.Nin_Ounce_NVN2.bfotf, the same button font
TotK uses (U+E040 A ... U+E047 ZR, U+E04B Plus, U+E04C Minus, the d-pad at U+E049 and U+E060..E065). The shapes are
TotK's (tools/button_icons.py xbox_icons: LB RB LT RT, Menu and View, the d-pad as a cross), lettered with the game's
own Latin text font; the face buttons keep the game's glyphs, by label, or trade places for a pad mapped by position
(style xbox: the button labelled A on an Xbox pad is the Switch's B, so the prompt for B shows A). OoT keeps the font
as a file of its own, so the overlay replaces that one file; it is compressed again with the dictionary of the
original (Pack/ZsDic.pack.zs) and listed in manifest.tsv as runtime/romfs.c reads it (path, size, SHA-256, SHA-256
of the original as the game reads it). The redrawn font is larger, so the resource size table's entry for it grows
by as much (button_icons.grow_restbl), as TotK's prompts do: with the old entry the game's loader got a buffer too
small for the font and read null right after loading it.

The action hint (the button and its word at the bottom of the screen, layouts DoCommand and ActionGuide) is no
text: it draws a white disc and on it one of four letter images, KeyTexFontA_00, B, X and Y of
UI/Layout2Archive/Common.Product.100.Nin_Ounce_NVN2.bntx.zs (BC4, 64x64, the letter 27 pixels high in the middle).
The font alone left that hint on the Switch's letters in every set. KEY_TEXTURES gives each set's folder that file
too: the sets by position trade the images A with B and X with Y (the game's own letters, as the font's glyphs trade),
the PlayStation sets draw the shapes (button_icons.playstation_symbols), the keyboard set draws the default key or
mouse button (button_icons.pc_icons); the sets by label keep the file as it is. The images are encoded again as BC4
in the Switch 2's block-linear layout (button_icons.bntx_redraw with ns2), so the file keeps its size. The game reads
that file once, at start, so a set changed in the running game reaches the hint through prompts-hint.bin (write_hint):
every set's four images as the game holds them, which runtime/romfs.c hint_apply writes over the shown set's in memory.
A hint shown after the change has the new images; one already on screen keeps its look until it changes, as the game
draws it once. The keyboard set's hint shows the default keys, not the bindings of the player's rekarina.ini.

Cutscene skip: the game's own skip screen (UIEventSkipScreen) offers to skip any event whose setting list does not hold
the component Event/EventSetting/EventSettingComponent/SkipSetting/DisableSkip.engine__event__SkipSetting.bgyml, a BYML of
one entry {IsDisableSkip: true}. Three events list it: the opening in the forest, the staff credits and the title scene,
so those alone cannot be skipped. Every overlay folder carries that file with the value false (skip_setting: the same
56 bytes, so the resource size table keeps its entry), and those three skip like every other cutscene.

Faster dialogue: the game's messages (Mals/<language>.Product.<version>.sarc.zs, one SARC of MSBT files per language)
hold pause tags between phrases: group 5, types 0, 1 and 2 without parameters (about 4,000, 7,500 and 200 in each
language), the only waits in the text; the game has no message speed setting. romfs-patches-fasttext holds every
archive with those tags left out (fast_text: the TXT2 strings rebuilt, every other section and the archive's order and
names as they were), compressed again with the original's dictionary. The files only shrink, so the resource size
table keeps its entries. runtime/romfs.c serves the folder when fast_text is on (a restart).

Scene resolution: Pack/Bootup.Nin_Ounce_NVN2.pack.zs holds System/GameGraphics/ModelSceneExtensionParam, whose
ResolutionParam renders the docked 2560x1440 picture from a scene of ResolutionReduceScale times that and whose
DynamicResolutionParam palettes step the scale down while the GPU looks loaded: 0.75 at best (1920x1080), down to
0.33 (853x480). On PC the load the game measures includes the replay's latency, so the scene dropped to half
resolution and the picture went soft now and then. Every level is set to --scene-scale (default 1.0, the 2560x1440
scene); the values change in place, so the pack keeps its size and the resource size table its entry. Every overlay
folder carries the pack.

    python tools/make_oot_prompts.py --out <dir next to rekarina.exe> [--nup <container>] [--scene-scale S]
    python tools/make_oot_prompts.py --scan [--nup <container>]
"""
import argparse
import codecs
import hashlib
import json
import struct
import sys
import zlib
from pathlib import Path

try:
    import zstandard
except ImportError:   # the release's Python 3.14: its own compression.zstd behind the package's name (tools/py314)
    sys.path.insert(0, str(Path(__file__).resolve().parent / 'py314'))
    import zstandard

import button_icons as icons
from oot_romfs import NUP, RomFS, sarc_members

FONT = 'Font2/Scalable/External.Nin_Ounce_NVN2.bfotf.zs'
RESTBL = 'System/Resource/ResourceSizeTable.Product.111.Nin_Ounce_NVN2.rsizetable.zs'
# The letters on LB, RB, LT and RT: the game's Latin UI font, DemiBold (the weight of its menus)
LETTER_FONTS = ('Font2/Scalable/ClarimoUDPE-DemiBold.Nin_Ounce_NVN2.bfotf.zs',
                'Font2/Scalable/CetraDisplayPro-Medium.Nin_Ounce_NVN2.bfotf.zs')
KEY_GLYPHS = ((0xE040, 0xE041), (0xE042, 0xE043))   # A B, X Y: traded for a pad mapped by position
STYLES = {'xbox-label': (), 'xbox': KEY_GLYPHS}
# Damaged texture -> the sibling that stands in for it (texture names of equal length; the name is replaced inside)
STANDINS = {
    'Tex/CmnTex_Map_Soil_FairyFountain_Floor_D_Nrm.bntx.zs': 'Tex/CmnTex_Map_Rock_FairyFountain_Floor_B_Nrm.bntx.zs',
    'Tex/Rock_WaterTemple_Gate_M_01_Nrm.bntx.zs': 'Tex/Rock_WaterTemple_Gate_P_01_Nrm.bntx.zs',
    # (a pair whose names hold a name of the game's story: written with every letter moved 13 places)
    codecs.decode('Grk/EOnxr_Fprar_TnabaFgntr_T_2_1_q0_g0.oagk.mf', 'rot13'):
        codecs.decode('Grk/EOnxr_Fprar_TnabaFgntr_T_2_2_q0_g0.oagk.mf', 'rot13'),
}
ZSTD_MAGIC = bytes.fromhex('28b52ffd')


def tex_name(path):
    return path.split('/')[-1].split('.')[0]


def standins(fs):
    """[(path, overlay bytes, original raw bytes, decompressed size)] for the damaged files this dump has."""
    out = []
    for path, sub in STANDINS.items():
        if path not in fs.index or path in fs.index and fs.read_raw(path)[:4] == ZSTD_MAGIC:
            continue   # not in this dump, or intact
        old, new = tex_name(sub).encode(), tex_name(path).encode()
        assert len(old) == len(new), (sub, path)
        raw_sub = fs.read_raw(sub)
        data = fs.unzs(raw_sub)
        assert data.count(old) >= 1, sub
        data = data.replace(old, new)
        out.append((path, compress_like(fs, raw_sub, data), fs.read_raw(path), len(data)))
    return out


def scan(fs):
    """.zs files of the RomFS that do not start with a zstd frame header."""
    import io
    r, bad = fs.r, []
    for path, i in fs.index.items():
        if not path.endswith('.zs'):
            continue
        _, size, _, eoff, esz, codec = r.files[i]
        if not size:
            continue
        if int(codec) == 0:
            head = bytes(r._mm[eoff:eoff + 4])
        else:
            with zstandard.ZstdDecompressor().stream_reader(io.BytesIO(r._mm[eoff:eoff + esz])) as rd:
                head = rd.read(4)
        if head != ZSTD_MAGIC:
            bad.append(path)
    return bad


def compress_like(fs, raw, contents, level=None):
    """contents compressed with the dictionary the original raw used, checked by decompressing. Level 19, or 9 for a
    file over 16 MiB: the common layout images (245 MB) take 25 s at 19 and 1.4 s at 9 for 6 % more bytes, once per
    prompt set at every install."""
    did = zstandard.get_frame_parameters(raw).dict_id
    d = fs.dicts()[did] if did else None
    level = level or (19 if len(contents) <= 16 << 20 else 9)
    cctx = zstandard.ZstdCompressor(level=level, dict_data=d) if d else zstandard.ZstdCompressor(level=level)
    data = cctx.compress(contents)
    check = zstandard.ZstdDecompressor(dict_data=d) if d else zstandard.ZstdDecompressor()
    assert check.decompress(data, max_output_size=len(contents) * 2) == contents
    return data


COMMON_BNTX = 'UI/Layout2Archive/Common.Product.100.Nin_Ounce_NVN2.bntx.zs'
# The action hint's letter images (A, B, X and Y as the Switch names them) and the pairs the sets by position trade
KEY_TEXTURES = {'KeyTexFontA_00': 0xE040, 'KeyTexFontB_00': 0xE041, 'KeyTexFontX_00': 0xE042, 'KeyTexFontY_00': 0xE043}
KEY_SWAPS = (('KeyTexFontA_00', 'KeyTexFontB_00'), ('KeyTexFontX_00', 'KeyTexFontY_00'))
# The size of a drawn symbol in the 64-pixel image: the game's letters are 27 pixels high; a key or the mouse needs a
# little more room for the label it carries
KEY_FIT = {'playstation': 28, 'pc': 34}


def key_textures(fs, style, glyphs, adv, hint_images=None):
    """(path, overlay bytes, original raw bytes) of the common layout images with the action hint's four buttons as
    style shows them, None for a set that keeps the game's letters (the Switch prompts and the sets by label).
    hint_images: a dict that gets style -> the four images' bytes (KEY_TEXTURES' order) as the game holds them in
    memory, for every style, for prompts-hint.bin."""
    raw = fs.read_raw(COMMON_BNTX)
    data = bytearray(fs.unzs(raw))
    changed = True
    if style in ('xbox', 'rekarina'):
        icons.bntx_swap(data, 0, KEY_SWAPS)
    elif style in ('playstation', 'playstation-label'):
        icons.bntx_redraw(data, 0, KEY_TEXTURES, icons.playstation_symbols(style == 'playstation'), ns2=True,
                          fit=KEY_FIT['playstation'])
    elif style == 'pc':
        icons.bntx_redraw(data, 0, KEY_TEXTURES, icons.pc_icons(glyphs, adv), ns2=True, fit=KEY_FIT['pc'])
    else:
        changed = False
    if hint_images is not None:
        images = icons.bntx_images(data, 0)
        hint_images[style] = [bytes(data[images[n][1]:images[n][1] + images[n][2]]) for n in KEY_TEXTURES]
    return (COMMON_BNTX, compress_like(fs, raw, bytes(data)), raw) if changed else None


def write_hint(out, hint_images):
    """prompts-hint.bin: the action hint's four button images of every set, for changing them in the running game as the
    font's cmap changes (runtime/romfs.c hint_apply). "RKPH", u32 version 1, u32 sets, u32 image bytes, u32 images per
    set (4: A, B, X, Y), then per set a 32-byte name (the names of prompts-live.bin) and its images, each exactly the
    bytes the game copies out of the common layout images (BC4, the Switch 2's block-linear layout)."""
    sizes = {len(img) for imgs in hint_images.values() for img in imgs}
    assert len(sizes) == 1, f'the hint images differ in size: {sizes}'
    size = sizes.pop()
    data = b'RKPH' + struct.pack('<IIII', 1, len(hint_images), size, len(KEY_TEXTURES))
    for style, imgs in hint_images.items():
        data += style.encode().ljust(32, b'\0') + b''.join(imgs)
    (Path(out) / 'prompts-hint.bin').write_bytes(data)
    return dict(sets=len(hint_images), image_bytes=size)


BOOTUP = 'Pack/Bootup.Nin_Ounce_NVN2.pack.zs'
SCENE_PARAM = 'System/GameGraphics/ModelSceneExtensionParam/Default.game__gfx__ModelSceneExtensionParam.bgyml'


def byml_floats(b, key_names):
    """Offsets of the float values (BYML type 0xD2) stored under any of key_names, anywhere in the document (hash nodes,
    arrays and the 0x20 hash-keyed dictionaries of BYML v7)."""
    ver, kt, st, root = struct.unpack_from('<HIII', b, 2)
    n = struct.unpack_from('<I', b, kt)[0] >> 8
    offs = struct.unpack_from('<%dI' % (n + 1), b, kt + 4)
    keys = [b[kt + offs[i]:b.index(b'\0', kt + offs[i])].decode() for i in range(n)]
    found, seen = set(), set()

    def node(off):
        if off in seen:
            return
        seen.add(off)
        t, n = b[off], struct.unpack_from('<I', b, off)[0] >> 8
        if t == 0xC1:
            for i in range(n):
                e = off + 4 + 8 * i
                k, vt = struct.unpack_from('<I', b, e)[0] & 0xffffff, b[e + 3]
                value(vt, e + 4, keys[k])
        elif t == 0xC0:
            base = off + 4 + ((n + 3) & ~3)
            for i in range(n):
                value(b[off + 4 + i], base + 4 * i, None)
        elif t == 0x20:
            for i in range(n):
                value(b[off + 4 + 8 * n + i], off + 4 + 8 * i + 4, None)

    def value(vt, at, key):
        if vt in (0xC0, 0xC1, 0x20):
            node(struct.unpack_from('<I', b, at)[0])
        elif vt == 0xD2 and key in key_names:
            found.add(at)
    node(root)
    return sorted(found)


def scene_scale_pack(fs, scale):
    """(Bootup pack path, the pack with every ResolutionReduceScale set to scale, its original raw)."""
    raw = fs.read_raw(BOOTUP)
    pack = bytearray(fs.unzs(raw))
    lo, hi = sarc_members(bytes(pack))[SCENE_PARAM]
    at = byml_floats(bytes(pack[lo:hi]), {'ResolutionReduceScale'})
    assert at, 'no ResolutionReduceScale in ' + SCENE_PARAM
    for a in at:
        struct.pack_into('<f', pack, lo + a, scale)
    return BOOTUP, compress_like(fs, raw, bytes(pack)), raw


SKIP_SETTING = 'Event/EventSetting/EventSettingComponent/SkipSetting/DisableSkip.engine__event__SkipSetting.bgyml'


def skip_setting(fs):
    """(SKIP_SETTING, the setting with IsDisableSkip false, its original raw); None when the dump lacks the file or its
    layout is not the one expected (a BYML whose root dictionary holds the bool IsDisableSkip)."""
    if SKIP_SETTING not in fs.index:
        return None
    raw = fs.read_raw(SKIP_SETTING)
    data = bytearray(fs.unzs(raw) if raw[:4] == ZSTD_MAGIC else raw)
    if data[:2] != b'YB':
        return None
    keys_at, root_at = struct.unpack_from('<I', data, 4)[0], struct.unpack_from('<I', data, 12)[0]
    if data[keys_at] != 0xC2 or data[root_at] != 0xC1:
        return None
    nkeys = int.from_bytes(data[keys_at + 1:keys_at + 4], 'little')
    names = []
    for i in range(nkeys):
        s = keys_at + struct.unpack_from('<I', data, keys_at + 4 + 4 * i)[0]
        names.append(bytes(data[s:data.index(0, s)]).decode())
    count = int.from_bytes(data[root_at + 1:root_at + 4], 'little')
    for i in range(count):
        e = root_at + 4 + 8 * i
        key, kind = int.from_bytes(data[e:e + 3], 'little'), data[e + 3]
        if key < len(names) and names[key] == 'IsDisableSkip' and kind == 0xD0:
            if struct.unpack_from('<I', data, e + 4)[0] == 0:
                return None   # already skippable
            struct.pack_into('<I', data, e + 4, 0)
            out = compress_like(fs, raw, bytes(data)) if raw[:4] == ZSTD_MAGIC else bytes(data)
            return SKIP_SETTING, out, raw
    return None


PAUSE_GROUP, PAUSE_TYPES = 5, (0, 1, 2)


def text_without_pauses(s):
    """(one UTF-8 MSBT string without its pause tags, how many were left out). A tag is 0x0E, group, type and parameter
    length (u16 each) and the parameters; an end tag 0x0F, group and type; UTF-8 text never holds those bytes."""
    out, j, removed = bytearray(), 0, 0
    while j < len(s):
        c = s[j]
        if c == 0x0E and j + 7 <= len(s):
            group, kind, size = struct.unpack_from('<HHH', s, j + 1)
            if group == PAUSE_GROUP and kind in PAUSE_TYPES and size == 0:
                removed += 1
            else:
                out += s[j:j + 7 + size]
            j += 7 + size
        elif c == 0x0F and j + 5 <= len(s):
            out += s[j:j + 5]
            j += 5
        else:
            out.append(c)
            j += 1
    return bytes(out), removed


def msbt_without_pauses(m):
    """(the MSBT with its TXT2 strings without pause tags, tags left out); unchanged for a UTF-16 file or a TXT2 whose
    strings are not packed right after its offset table."""
    if m[:8] != b'MsgStdBn' or m[0xc] != 0:
        return m, 0
    sections = struct.unpack_from('<H', m, 0xe)[0]
    out, p, removed = bytearray(m[:0x20]), 0x20, 0
    for _ in range(sections):
        magic, size = struct.unpack_from('<4sI', m, p)
        body = m[p + 16:p + 16 + size]
        if magic == b'TXT2':
            n = struct.unpack_from('<I', body, 0)[0]
            offs = list(struct.unpack_from(f'<{n}I', body, 4)) + [len(body)]
            if n and offs[0] == 4 + 4 * n and offs == sorted(offs):
                strings = []
                for i in range(n):
                    s, k = text_without_pauses(body[offs[i]:offs[i + 1]])
                    strings.append(s)
                    removed += k
                at, table = 4 + 4 * n, []
                for s in strings:
                    table.append(at)
                    at += len(s)
                body = struct.pack(f'<I{n}I', n, *table) + b''.join(strings)
        head = bytearray(m[p:p + 16])
        struct.pack_into('<I', head, 4, len(body))
        out += head + body
        out += b'\xab' * (-len(out) % 16)
        p = (p + 16 + size + 15) & ~15
    struct.pack_into('<I', out, 0x12, len(out))
    return bytes(out), removed


def sarc_without_pauses(data):
    """(the message SARC with every MSBT member without pause tags, tags left out). The members keep their order,
    names and hashes; each starts 16-aligned, as MSBT sizes keep them in the original (no gaps between members)."""
    data_at = struct.unpack_from('<I', data, 0xc)[0]
    count = struct.unpack_from('<H', data, 0x14 + 6)[0]
    out, blob, removed = bytearray(data[:data_at]), bytearray(), 0
    for i in range(count):
        node = 0x20 + 16 * i
        begin, end = struct.unpack_from('<II', data, node + 8)
        member = data[data_at + begin:data_at + end]
        if member[:8] == b'MsgStdBn':
            member, k = msbt_without_pauses(member)
            removed += k
        blob += bytes(-len(blob) % 16)
        struct.pack_into('<II', out, node + 8, len(blob), len(blob) + len(member))
        blob += member
    out += blob
    struct.pack_into('<I', out, 8, len(out))
    return bytes(out), removed


def fast_text(fs):
    """[(path, overlay bytes, original raw)] for every message archive, and how many pause tags went."""
    files, removed = [], 0
    for path in sorted(p for p in fs.index if p.startswith('Mals/') and p.endswith('.sarc.zs')):
        raw = fs.read_raw(path)
        data, k = sarc_without_pauses(fs.unzs(raw))
        assert set(sarc_members(data)) == set(sarc_members(fs.unzs(raw)))
        files.append((path, compress_like(fs, raw, data, level=9), raw))
        removed += k
    return files, removed


def write_overlay(folder, files):
    """files: [(RomFS path, overlay bytes, original raw bytes)] -> the folder and its manifest.tsv"""
    lines = []
    for path, data, raw in files:
        target = folder / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        lines.append('\t'.join([path, str(len(data)), hashlib.sha256(data).hexdigest(), hashlib.sha256(raw).hexdigest()]))
    (folder / 'manifest.tsv').write_text('# path\tsize\tsha256\toriginal_sha256 (tools/make_oot_prompts.py)\n'
                                         + ''.join(line + '\n' for line in lines), encoding='utf-8')


def restbl_entries(fs):
    """(RESTBL raw, decompressed table, CRC32 -> offset of the entry)"""
    table_raw = fs.read_raw(RESTBL)
    table = fs.unzs(table_raw)
    return table_raw, table, icons.restbl_offsets(table)


def restbl_entry(fs, path):
    _, table, at = restbl_entries(fs)
    return struct.unpack_from('<I', table, at[zlib.crc32(path.removesuffix('.zs').encode())])[0]


def restbl_file(fs, want):
    """(RESTBL path, the table with entries raised, its original raw), None when nothing needs raising. want: path ->
    the entry it needs. Entries are the game's buffer size for a file, which is more than the file (a type's header and
    alignment), so a needed entry is another entry plus what the file grew, never a file size."""
    table_raw, table, at = restbl_entries(fs)
    extra = {}
    for path, need in want.items():
        have = struct.unpack_from('<I', table, at[zlib.crc32(path.removesuffix('.zs').encode())])[0]
        if need > have:
            extra[path] = need - have
    if not extra:
        return None
    return RESTBL, compress_like(fs, table_raw, icons.grow_restbl(table, extra)), table_raw


def build(fs, out, scene_scale=1.0):
    raw = fs.read_raw(FONT)
    blob = fs.unzs(raw)
    otf, key = icons.bfotf_decode(blob)
    glyphs = icons.text_glyphs(fs.read(p) for p in LETTER_FONTS)
    drawn = icons.xbox_icons(glyphs)
    fixes = standins(fs)
    fix_need = {p: restbl_entry(fs, STANDINS[p]) for p, _, _, _ in fixes}   # a stand-in needs its sibling's entry
    report = {'standins': [p for p, _, _, _ in fixes], 'scene_scale': scene_scale}
    scene = scene_scale_pack(fs, scene_scale)
    fixes.append(scene + (None,))   # same size: no resource size table entry to raise
    skip = skip_setting(fs)         # every cutscene skippable; the same size too
    if skip:
        fixes.append(skip + (None,))
    report['skip_all_cutscenes'] = bool(skip)
    # Every folder's font holds the button glyphs of every set and differs only in its cmap (fonts_with_sets), so the
    # prompts can change while the game runs: prompts-live.bin lists each set's cmap table for runtime/prompts_live.c.
    # The Switch prompts ("nintendo") get the font too, in romfs-patches.
    sets = {'nintendo': ({}, ())}
    sets.update({style: (drawn, swaps) for style, swaps in STYLES.items()})
    sets['playstation'] = (icons.playstation_icons(glyphs, True), ())
    sets['playstation-label'] = (icons.playstation_icons(glyphs, False), ())
    sets['rekarina'] = (icons.rekarina_icons(glyphs, otf, True), ())     # Xbox names on the game's own outlines
    sets['rekarina-label'] = (icons.rekarina_icons(glyphs, otf, False), ())
    adv = icons.font_advances(blob)
    icons.pc_labels_fit(glyphs)
    sets['pc'] = (icons.pc_icons(glyphs, adv), ())                       # the keyboard and the mouse, default keys
    fonts, cmaps, keyinfo = icons.fonts_with_sets(otf, sets, icons.pc_key_icons(glyphs, adv, adv[0xE040]))
    hint_images = {}
    for style, (set_icons, swaps) in sets.items():
        new = icons.bfotf_encode(fonts[style], key, blob)
        files = [(FONT, compress_like(fs, raw, new), raw)] + [(p, d, r) for p, d, r, _ in fixes]
        hint = key_textures(fs, style, glyphs, adv, hint_images)   # same size as the original: no table entry
        if hint:
            files.append(hint)
        table = restbl_file(fs, dict(fix_need, **{FONT: restbl_entry(fs, FONT) + max(0, len(new) - len(blob))}))
        if table:
            files.append(table)
        write_overlay(Path(out) / ('romfs-patches' if style == 'nintendo' else f'romfs-patches-{style}'), files)
        report[style] = dict(icons=len(set_icons), swaps=len(swaps), font_bytes=len(new), grew=len(new) - len(blob),
                             files=len(files), hint=bool(hint))
    # prompts-live.bin: "RKPL", u32 version 1, u32 sets, u32 table length, then per set a 32-byte name and the table
    size = len(next(iter(cmaps.values())))
    live = b'RKPL' + struct.pack('<III', 1, len(cmaps), size)
    for style, tbl in cmaps.items():
        live += style.encode().ljust(32, b'\0') + tbl
    (Path(out) / 'prompts-live.bin').write_bytes(live)
    report['live'] = dict(sets=len(cmaps), cmap_bytes=size)
    fast, removed = fast_text(fs)
    write_overlay(Path(out) / 'romfs-patches-fasttext', fast)
    report['fast_text'] = dict(archives=len(fast), pauses_removed=removed)
    report['previews'] = write_previews(out, fonts)
    report['keys'] = write_keys(out, fonts['pc'], keyinfo)
    report['hint'] = write_hint(out, hint_images)
    return report


PREVIEW_CELL = 72


def write_previews(out, fonts):
    """prompt-previews.bin: what each prompt set's buttons look like, for the settings menu (runtime/romfs.c
    romfs_prompt_preview). "RKPV", u32 version 1, u32 sets, u32 cell, u32 buttons, then per set a 32-byte name and
    buttons masks of cell x cell coverage bytes, drawn from the set's own button font (button_icons.preview_strip).
    The Switch set's glyphs are the game's, so the file belongs to the game folder like the overlays."""
    strips = {style: icons.preview_strip(otf, PREVIEW_CELL) for style, otf in fonts.items()}
    count = next(iter(strips.values()))[0]
    data = b'RKPV' + struct.pack('<IIII', 1, len(strips), PREVIEW_CELL, count)
    for style, (_, masks) in strips.items():
        data += style.encode().ljust(32, b'\0') + masks
    (Path(out) / 'prompt-previews.bin').write_bytes(data)
    return dict(sets=len(strips), buttons=count, cell=PREVIEW_CELL)


def write_keys(out, pc_font, live):
    """prompts-keys.bin: what the keyboard prompts need to follow the bindings while the game runs (runtime/romfs.c).
    "RKPK", u32 version 1, u32 buttons, u32 keys, u32 cell; per button a 16-byte action name (a bind_<action> line of
    rekarina.ini), u32 code point and u32 offset of its 16-bit big-endian glyph delta in the font's cmap table (the pc
    table of prompts-live.bin; glyph = code point + delta); per key a 16-byte name and u32 glyph id; then per key a
    cell x cell coverage mask, the key as the menu shows it."""
    buttons = [(action, cp, live['deltas'][cp]) for cp, action in icons.PC_KEY_BUTTONS.items()]
    names = list(live['keys'])
    # the scale of the set's own strip, so a key in the menu is as large as the buttons beside it
    font = icons.TTFont(icons.io.BytesIO(pc_font))
    cmap, glyphs = font.getBestCmap(), font.getGlyphSet()
    extent = 0
    for cp in icons.PREVIEW_CODES:
        box = icons.BoundsPen(glyphs); glyphs[cmap[cp]].draw(box)
        extent = max(extent, box.bounds[2] - box.bounds[0], box.bounds[3] - box.bounds[1])
    _, masks = icons.preview_strip(pc_font, PREVIEW_CELL, names=[f'rk.key.{k}' for k in names], extent=extent)
    data = b'RKPK' + struct.pack('<IIII', 1, len(buttons), len(names), PREVIEW_CELL)
    for action, cp, at in buttons:
        data += action.encode().ljust(16, bytes(1)) + struct.pack('<II', cp, at)
    for key in names:
        data += key.encode().ljust(16, bytes(1)) + struct.pack('<I', live['keys'][key])
    (Path(out) / 'prompts-keys.bin').write_bytes(data + masks)
    return dict(buttons=len(buttons), keys=len(names))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--out', type=Path)
    ap.add_argument('--nup', default=NUP)
    ap.add_argument('--scan', action='store_true', help='list .zs files without a zstd header (damaged dumps)')
    ap.add_argument('--scene-scale', type=float, default=1.0,
                    help='every dynamic resolution level (1.0: the 2560x1440 scene; the game ships 0.75 down to 0.33)')
    args = ap.parse_args()
    if args.scan:
        print('\n'.join(scan(RomFS(args.nup))) or 'no damaged .zs files')
        return
    if not args.out:
        ap.error('--out is required')
    print(json.dumps(build(RomFS(args.nup), args.out, args.scene_scale), indent=1))


if __name__ == '__main__':
    main()
