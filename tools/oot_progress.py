#!/usr/bin/env python3
"""Story progress for OoT test runs: edits the game-data values of a COPY of a save profile (not a player feature).

    oot_progress.py --nup <dump.nup> --profile <copy of a profile folder> [--preset child-all|adult|adult-early ...]
                    [--set <name>=<value> ...] [--quest <quest>=<step> ...] [--dump] [--find <text>]

The game keeps its progress as named game-data values (bools, ints, enums, strings ...), looked up by the 32-bit
MurmurHash3 of the name, and writes every value that belongs in the save into slot_NN/progress.sav: a header, a table
of (name hash, value) pairs split into typed sections by entries whose hash is 0 and whose value is the type number,
then a data region with the arrays, strings and 64-bit values the table points at. The list of every value the game
has, with its type, its default and which file saves it, is the game's own GameDataList in the RomFS; it also holds
"structs" that group values under readable member names, which is how the hero's belongings are kept: per item a
count (1 = owned, or the ammunition), a "was got" bool, and a player status with the hearts, magic, rupees and what
is worn. This tool reads that list from the player's dump, so it carries no table of the game's values itself; the
game's own names its presets need are kept with their letters moved 13 places, as runtime/oot_warp.c does.

Every value is changed in place (the file keeps its size), in every slot of the profile, so whichever slot the game
continues from has the change. Refuses to touch a folder that is not a copy (the setup's or the test folder's own
profile).

<name> is a game-data name, a struct path with '/' between the parts (a member may be given as 0x<hash>, and a struct
with one member is entered by itself), or 0x<hash>. <value>: a number, true/false, an enum's value name, or text.
"""
import argparse
import os
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from oot_romfs import RomFS, NUP  # noqa: E402

TYPES = ['Bool', 'BoolArray', 'Int', 'IntArray', 'Float', 'FloatArray', 'Enum', 'EnumArray', 'Vector2', 'Vector2Array',
         'Vector3', 'Vector3Array', 'String16', 'String16Array', 'String32', 'String32Array', 'String64',
         'String64Array', 'Binary', 'BinaryArray', 'UInt', 'UIntArray', 'Int64', 'Int64Array', 'UInt64',
         'UInt64Array', 'WString16', 'WString16Array', 'WString32', 'WString32Array', 'WString64', 'WString64Array',
         'Bool64bitKey']
INLINE = {'Bool', 'Int', 'Float', 'Enum', 'UInt'}
STRLEN = {'String16': 16, 'String32': 32, 'String64': 64}


def rot13(s):
    return s.translate(str.maketrans('ABCDEFGHIJKLMabcdefghijklmNOPQRSTUVWXYZnopqrstuvwxyz',
                                     'NOPQRSTUVWXYZnopqrstuvwxyzABCDEFGHIJKLMabcdefghijklm'))


def mm3(data, seed=0):
    """MurmurHash3 x86 32: the game's hash of a game-data name."""
    if isinstance(data, str):
        data = data.encode()
    c1, c2, h, n = 0xcc9e2d51, 0x1b873593, seed, len(data)
    rotl = lambda x, r: ((x << r) | (x >> (32 - r))) & 0xffffffff  # noqa: E731
    for i in range(0, n & ~3, 4):
        k = struct.unpack_from('<I', data, i)[0]
        k = rotl((k * c1) & 0xffffffff, 15) * c2 & 0xffffffff
        h = (rotl(h ^ k, 13) * 5 + 0xe6546b64) & 0xffffffff
    t, k = data[n & ~3:], 0
    for j in range(len(t) - 1, -1, -1):
        k ^= t[j] << (8 * j)
    if t:
        h ^= rotl((k * c1) & 0xffffffff, 15) * c2 & 0xffffffff
    h ^= n
    h ^= h >> 16
    h = (h * 0x85ebca6b) & 0xffffffff
    h ^= h >> 13
    h = (h * 0xc2b2ae35) & 0xffffffff
    return h ^ (h >> 16)


def byml_load(b):
    """BYML of either byte order -> Python objects (the parts the GameDataList uses)."""
    e = '<' if b[:2] == b'YB' else '>'
    if b[:2] not in (b'YB', b'BY'):
        raise ValueError('not BYML')
    _ver, kt, st, root = struct.unpack_from(e + 'HIII', b, 2)

    def count(off):
        w = struct.unpack_from(e + 'I', b, off)[0]
        return (w >> 8) if e == '<' else (w & 0xffffff)

    def strtab(off):
        if not off:
            return []
        n = count(off)
        offs = struct.unpack_from(e + '%dI' % (n + 1), b, off + 4)
        return [b[off + offs[i]:b.index(b'\0', off + offs[i])].decode('utf-8', 'replace') for i in range(n)]
    keys, strs = strtab(kt), strtab(st)

    def value(vt, at):
        raw = struct.unpack_from(e + 'I', b, at)[0]
        if vt in (0xC0, 0xC1, 0x20, 0x21):
            return node(raw)
        if vt == 0xA0:
            return strs[raw]
        if vt == 0xD0:
            return bool(raw)
        if vt == 0xD1:
            return struct.unpack_from(e + 'i', b, at)[0]
        if vt == 0xD2:
            return struct.unpack_from(e + 'f', b, at)[0]
        if vt == 0xD3:
            return raw
        if vt in (0xD4, 0xD5, 0xD6):
            return struct.unpack_from(e + {0xD4: 'q', 0xD5: 'Q', 0xD6: 'd'}[vt], b, raw)[0]
        return None

    def node(off):
        t, n = b[off], count(off)
        if t == 0xC1:
            d = {}
            for i in range(n):
                at = off + 4 + 8 * i
                w = struct.unpack_from(e + 'I', b, at)[0]
                d[keys[(w & 0xffffff) if e == '<' else (w >> 8)]] = value(b[at + 3], at + 4)
            return d
        if t == 0xC0:
            base = off + 4 + ((n + 3) & ~3)
            return [value(b[off + 4 + i], base + 4 * i) for i in range(n)]
        return None
    return node(root)


class GameData:
    """The GameDataList: every value's type and default, the structs and the enums' value names."""

    def __init__(self, fs):
        names = sorted(p for p in fs.index if p.startswith('GameData/GameDataList.Product.') and p.endswith('.byml.zs'))
        data = byml_load(fs.read(names[-1]))['Data']   # the newest list (the update's)
        self.entry, self.struct = {}, {}
        for typ, lst in data.items():
            for e in lst:
                h = e.get('Hash')
                if typ == 'Struct':
                    self.struct[h] = {m['Hash']: m['Value'] for m in e['DefaultValue']}
                elif isinstance(h, int) and h < 1 << 32:
                    self.entry[h] = (typ, e)
        # member names of the belongings structs are the item actors' names: name them from the dump's actor packs
        self.member_name = {mm3(n): n for n in {q.rsplit('/', 1)[-1].split('.')[0] for q in fs.index
                                                if q.startswith('Pack/Actor/')}}
        self.enum_name = {}
        for typ in ('Enum', 'EnumArray'):
            for e in data.get(typ, []):
                for v, r in zip(e['Values'], e['RawValues']):
                    self.enum_name[v] = r

    def resolve(self, name):
        """name -> hash of a value: 0x<hash>, a plain name, or a struct path (a/b/c)."""
        if name.startswith('0x'):
            return int(name, 16)
        parts = name.split('/')
        h = mm3(parts[0])
        for p in parts[1:]:
            s = self.struct.get(h)
            if s is None:
                raise KeyError(f'{name}: {p!r} is under something that is not a struct')
            m = int(p, 16) if p.startswith('0x') else mm3(p)
            if m not in s:
                raise KeyError(f'{name}: no member {p!r}')
            h = s[m]
        while h in self.struct and len(self.struct[h]) == 1:   # a struct of one value stands for that value
            h = next(iter(self.struct[h].values()))
        if h in self.struct:
            raise KeyError(f'{name}: names a struct of {len(self.struct[h])} members, not a value')
        return h


class Save:
    def __init__(self, path):
        self.path = path
        self.b = bytearray(Path(path).read_bytes())
        if struct.unpack_from('<I', self.b, 0)[0] != 0x01020304:
            raise ValueError(f'{path}: not a game-data save')
        end = struct.unpack_from('<I', self.b, 8)[0]
        self.at, t = {}, None   # hash -> (table position, type)
        for i in range(0x20, end, 8):
            h, v = struct.unpack_from('<II', self.b, i)
            if h == 0:
                t = TYPES[v] if v < len(TYPES) else str(v)
            else:
                self.at[h] = (i, t)
        self.changed = 0

    def get(self, h):
        pos, t = self.at[h]
        v = struct.unpack_from('<I', self.b, pos + 4)[0]
        if t in STRLEN:
            return self.b[v:v + STRLEN[t]].split(b'\0')[0].decode('utf-8', 'replace')
        if t == 'Float':
            return struct.unpack('<f', struct.pack('<I', v))[0]
        if t == 'Int':
            return v - (1 << 32) if v >> 31 else v
        return v

    def put(self, h, value):
        pos, t = self.at[h]
        if t in STRLEN:
            off = struct.unpack_from('<I', self.b, pos + 4)[0]
            raw = value.encode()
            if len(raw) >= STRLEN[t]:
                raise ValueError(f'{value!r} is too long for a {t}')
            new = raw + bytes(STRLEN[t] - len(raw))
            if self.b[off:off + STRLEN[t]] != new:
                self.b[off:off + STRLEN[t]] = new
                self.changed += 1
            return
        if t not in INLINE:
            raise ValueError(f'{t} values are not written by this tool')
        raw = struct.pack('<f', value) if t == 'Float' else struct.pack('<I', int(value) & 0xffffffff)
        if self.b[pos + 4:pos + 8] != raw:
            self.b[pos + 4:pos + 8] = raw
            self.changed += 1

    def write(self):
        Path(self.path).write_bytes(bytes(self.b))


# The presets, in the game's own names (letters moved 13 places). A pouch entry set above 0 also sets the item's
# "was got" flag. Hearts are counted in quarters.
Q = rot13
SWORD1, SWORD2, SWORD3 = Q('Jrncba_Fjbeq_Xbxvev'), Q('Jrncba_Fjbeq_Znfgre'), Q('Jrncba_Fjbeq_Ovttbeba')
SHIELD1, SHIELD2, SHIELD3 = Q('Jrncba_Fuvryq_Qrxh'), Q('Jrncba_Fuvryq_Ulyvna'), Q('Jrncba_Fuvryq_Zveebe')
CHILD_POUCH = [SWORD1, SHIELD1, SHIELD2, Q('Jrncba_Enatr_FyvatFubg'), Q('Vgrz_Obbzrenat'), Q('Vgrz_Bpnevan_Snvel'),
               Q('Vgrz_Bpnevan_Gvzr'), Q('Vgrz_Fpnyr_Tbyq'), Q('Vgrz_Jnyyrg_Ynetr'), Q('Vgrz_OhyyrgOnt_Ynetr'),
               Q('Vgrz_AhgOnt_Ynetr'), Q('Vgrz_FgvpxOnt_Ynetr'), Q('Vgrz_ObzoOnt_Ynetr'),
               Q('Nezbe_Puvyq_Tnhagyrg_Tbeba'), Q('Vgrz_QvaSver'), Q('Vgrz_SneberJvaq'), Q('Vgrz_AnlehYbir'),
               Q('Vgrz_NtbalFgbar'), Q('Nezbe_TynffGehgu'), Q('Vgrz_SnvelOyrff_ZC'), Q('Vgrz_SnvelOyrff_ZCQbhoyr'),
               Q('Vgrz_SnvelOyrff_QrsrafrQbhoyr'), Q('Vgrz_Fgbar_Sberfg'), Q('Vgrz_Fgbar_Sver'), Q('Vgrz_Fgbar_Jngre'),
               'BottleA', 'BottleB', 'BottleC', 'BottleD']
CHILD_MASKS = [Q('Nezbe_Puvyq_Znfx_Xrngba'), Q('Nezbe_Puvyq_Znfx_Fxhyy'), Q('Nezbe_Puvyq_Znfx_Fcbbxl'), Q('Nezbe_Puvyq_Znfx_Ohaal'), Q('Nezbe_Puvyq_Znfx_Tbeba'), Q('Nezbe_Puvyq_Znfx_Mben'), Q('Nezbe_Puvyq_Znfx_Trehqb'), Q('Nezbe_Puvyq_Znfx_Gehgu')]
ADULT_POUCH = [SWORD2, SWORD3, SHIELD3, Q('Jrncba_Enatr_Obj'), Q('Vgrz_Ubbxfubg'), Q('Vgrz_Ybatfubg'),
               Q('Vgrz_Unzzre'), Q('Vgrz_Neebj_Sver'), Q('Vgrz_Neebj_Vpr'), Q('Vgrz_Neebj_Yvtug'),
               Q('Cbhpu_Enatr_ObjNaqNeebj_Sver'), Q('Cbhpu_Enatr_ObjNaqNeebj_Vpr'),
               Q('Cbhpu_Enatr_ObjNaqNeebj_Yvtug'), Q('Vgrz_Dhvire_Ynetr'), Q('Nezbe_Nqhyg_Tbeba'),
               Q('Nezbe_Nqhyg_Mben'), Q('Nezbe_Nqhyg_UrnilObbg'), Q('Nezbe_Nqhyg_UbireObbg'),
               Q('Nezbe_Nqhyg_Tnhagyrg_Fvyire'), Q('Nezbe_Nqhyg_Tnhagyrg_Tbyq')] + \
              [Q('Vgrz_Zrqny_Sberfg'), Q('Vgrz_Zrqny_Sver'), Q('Vgrz_Zrqny_Jngre'), Q('Vgrz_Zrqny_Funqbj'), Q('Vgrz_Zrqny_Fcvevg'), Q('Vgrz_Zrqny_Yvtug')]
AMMO = {Q('Vgrz_QrxhFgvpx'): 30, Q('Vgrz_QrxhAhg'): 40, Q('Vgrz_QrxhFrrq'): 50, Q('Vgrz_Obzo'): 40,
        Q('Vgrz_Obzopuh'): 50, Q('Vgrz_ZntvpOrna'): 10}
ADULT_AMMO = {Q('Vgrz_Neebj_Abezny'): 50}
SONGS_EARLY = [Q('Vgrz_Zrybql_MryqnYhyynol'), Q('Vgrz_Zrybql_FnevnFbat'), Q('Vgrz_Zrybql_RcbanFbat'), Q('Vgrz_Zrybql_FhaFbat'), Q('Vgrz_Zrybql_FgbezFbat'), Q('Vgrz_Zrybql_GvzrFbat')]
SONGS_ALL = SONGS_EARLY + [Q('Vgrz_Zrybql_SberfgZvahrg'), Q('Vgrz_Zrybql_SverObyreb'), Q('Vgrz_Zrybql_JngreFreranqr'), Q('Vgrz_Zrybql_FcvevgErdhvrz'), Q('Vgrz_Zrybql_FunqbjAbpghear'), Q('Vgrz_Zrybql_YvtugCeryhqr'), Q('Vgrz_Zrybql_FpnerpebjFbat')]
QUESTS_CHILD = [Q('Zrrg_QrxhGerr'), Q('Trg_FgbarBsSberfg'), Q('Tbgb_UlehyrPnfgyr'), Q('Trg_FgbarBsSver'), Q('Trg_FgbarBsJngre'), Q('Zrrg_TerngSnvel')]
STATUS = Q('CynlreFgnghf')
POUCH, GOT = Q('CbhpuQngn'), Q('VfTrg')


def preset_ops(name):
    """-> list of (name, value) for a preset; 'age' is handled by the caller."""
    ops, age = [], None
    pouch = {}
    if name in ('child-all', 'adult', 'adult-early'):
        early = name == 'adult-early'
        items = CHILD_POUCH if not early else [SWORD1, SHIELD1, SHIELD2, Q('Jrncba_Enatr_FyvatFubg'),
                                               Q('Vgrz_Obbzrenat'), Q('Vgrz_Bpnevan_Snvel'), Q('Vgrz_Bpnevan_Gvzr'),
                                               Q('Vgrz_Fpnyr_Fvyire'), Q('Vgrz_Jnyyrg_Zrqvhz'),
                                               Q('Vgrz_OhyyrgOnt_Fznyy'), Q('Vgrz_ObzoOnt_Fznyy'),
                                               Q('Nezbe_Puvyq_Tnhagyrg_Tbeba'), Q('Vgrz_SnvelOyrff_ZC'),
                                               Q('Vgrz_Fgbar_Sberfg'), Q('Vgrz_Fgbar_Sver'), Q('Vgrz_Fgbar_Jngre'),
                                               'BottleA', SWORD2]
        for it in items:
            pouch[it] = 1
        if name == 'child-all':
            for it in CHILD_MASKS:
                pouch[it] = 1
        if name == 'adult':
            for it in ADULT_POUCH:
                pouch[it] = 1
            pouch.update(ADULT_AMMO)
        pouch.update({k: (v if not early else min(v, 20)) for k, v in AMMO.items()
                      if not early or k in (Q('Vgrz_QrxhFgvpx'), Q('Vgrz_QrxhAhg'), Q('Vgrz_QrxhFrrq'), Q('Vgrz_Obzo'))})
        for it, n in pouch.items():
            ops.append((f'{POUCH}/{it}', n))
        for s in (SONGS_ALL if name != 'adult-early' else SONGS_EARLY):
            ops.append((f'{GOT}/{s}', True))
        hearts = {'child-all': 20, 'adult': 20, 'adult-early': 6}[name]
        ops += [(f'{STATUS}/{Q("ZnkYvsr")}', hearts * 4), (f'{STATUS}/{Q("Yvsr")}', hearts * 4),
                (f'{STATUS}/{Q("ZCZnk")}', 48 if early else 96), (f'{STATUS}/{Q("ZC")}', 48 if early else 96),
                (f'{STATUS}/0xc995bbb8', 200),   # rupees (what the largest wallet the menu showed holds)
                (f'{STATUS}/{Q("RdhvcBpnevan")}', Q('Vgrz_Bpnevan_Gvzr'))]
        # a bottle is owned when its slot of the player status says so; the pouch count alone does not show it
        contents = ['Fairy', 'Empty', 'RedPotion', 'BlueFire']
        for i, b in enumerate(['BottleA'] if early else ['BottleA', 'BottleB', 'BottleC', 'BottleD']):
            ops += [(f'{STATUS}/AssignableItem/BottleInfo/{b}/IsValid', True),
                    (f'{STATUS}/AssignableItem/BottleInfo/{b}/BottleItem', contents[i] if not early else 'Empty')]
        for qn in QUESTS_CHILD:
            ops.append((Q('Fgrc_') + qn, 'Complete'))
        if name == 'child-all':
            age = 'child'
            ops += [(f'{STATUS}/{Q("RdhvcJrncba")}', SWORD1), (f'{STATUS}/{Q("RdhvcFuvryq")}', SHIELD1)]
        else:
            age = 'adult'
            ops += [(Q('Fgrc_QbbeBsGvzr'), 'Complete'), (f'{STATUS}/{Q("RdhvcJrncba")}', SWORD2),
                    (f'{STATUS}/{Q("RdhvcFuvryq")}', SHIELD2)]
    else:
        raise SystemExit(f'unknown preset {name!r} (child-all, adult, adult-early)')
    return ops, age


def parse_value(gd, h, text):
    typ, e = gd.entry[h]
    if isinstance(text, bool) or typ == 'Bool':
        return text if isinstance(text, bool) else text.lower() in ('1', 'true', 'yes', 'on')
    if typ == 'Enum':
        for v, r in zip(e['Values'], e['RawValues']):
            if r == text:
                return v
        raise ValueError(f'{text!r} is not one of {" ".join(e["RawValues"])}')
    if typ == 'Float':
        return float(text)
    if typ in ('Int', 'UInt'):
        return int(text, 0) if isinstance(text, str) else int(text)
    return str(text)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--nup', default=NUP)
    ap.add_argument('--profile', required=True, help='a COPY of a profile folder (holds save/<user>/slot_NN)')
    ap.add_argument('--preset', action='append', default=[])
    ap.add_argument('--set', action='append', default=[], metavar='NAME=VALUE')
    ap.add_argument('--quest', action='append', default=[], metavar='QUEST=STEP', help='Step_<QUEST> to that step')
    ap.add_argument('--dump', action='store_true', help='print the values the presets and --set name, from the save')
    ap.add_argument('--dump-all', action='store_true',
                    help='print every inline value of the first slot that differs from its default, with its struct '
                         'path when the belongings or the player status hold it')
    args = ap.parse_args()
    prof = Path(args.profile).resolve()
    if prof.name.lower() == 'profile-try' or any(p.name == 'OoT' for p in prof.parents) or prof.name == 'OoT':
        sys.exit(f'{prof}: not a copy (the test folder\'s or the package\'s own profile); copy it first')
    saves = sorted(prof.glob('save/*/slot_*/progress.sav'))
    if not saves:
        sys.exit(f'{prof}: no save/<user>/slot_NN/progress.sav')
    gd = GameData(RomFS(args.nup))
    ops, age = [], None
    for p in args.preset:
        o, a = preset_ops(p)
        ops += o
        age = a or age
    for s in args.set:
        k, _, v = s.partition('=')
        ops.append((k, v))
    for s in args.quest:
        k, _, v = s.partition('=')
        ops.append((Q('Fgrc_') + k, v))
    resolved = []
    for k, v in ops:
        h = gd.resolve(k)
        if h not in gd.entry:
            sys.exit(f'{k}: not in the game data list')
        resolved.append((k, h, parse_value(gd, h, v)))
        parts = k.split('/')
        if parts[0] == POUCH and len(parts) == 2 and not isinstance(v, str) and v:
            try:
                resolved.append((f'{GOT}/{parts[1]}', gd.resolve(f'{GOT}/{parts[1]}'), True))
            except KeyError:
                pass
    child_h, banc_h = mm3(Q('Frdhrapr_VfPuvyqubbq')), mm3(Q('Frdhrapr_PheeragOnap'))
    for path in saves:
        sv = Save(path)
        if args.dump_all:
            paths = {}
            for root in (POUCH, GOT, STATUS):   # struct paths, members by hash (the dump knows the names it was given)
                stack = [(mm3(root), root)]
                while stack:
                    h, p = stack.pop()
                    for m, v in gd.struct.get(h, {}).items():
                        name = gd.member_name.get(m, f'0x{m:08x}')
                        (stack.append((v, f'{p}/{name}')) if v in gd.struct else paths.setdefault(v, f'{p}/{name}'))
            print(f'== {path}')
            for h, (pos, t) in sorted(sv.at.items(), key=lambda x: x[1][0]):
                if t not in INLINE and t not in STRLEN:
                    continue
                cur, e = sv.get(h), gd.entry.get(h, (t, {}))[1]
                if cur != e.get('DefaultValue', cur if t in STRLEN and cur == '' else None):
                    print(f'  0x{h:08x} {t:8s} {gd.enum_name.get(cur, cur)!s:24s} {paths.get(h, "")}')
            break
        if args.dump:
            print(f'== {path}')
            for k, h, v in resolved:
                cur = sv.get(h) if h in sv.at else '(not saved)'
                print(f'  {k} = {gd.enum_name.get(cur, cur)}')
            continue
        missing = [k for k, h, v in resolved if h not in sv.at]
        for k, h, v in resolved:
            if h in sv.at:
                sv.put(h, v)
        if age:
            sv.put(child_h, age == 'child')
            want, other = ('_Adult', '_Child') if age == 'adult' else ('_Child', '_Adult')
            for h, (pos, t) in sv.at.items():   # the map the save continues in, in the hero's age
                if t in STRLEN:
                    s = sv.get(h)
                    if s.endswith(other) and s.count('_') == 1:
                        sv.put(h, s[:-len(other)] + want)
        sv.write()
        print(f'{path}: {sv.changed} values changed' + (f'; not in this save: {", ".join(missing)}' if missing else ''))


if __name__ == '__main__':
    main()
