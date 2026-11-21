"""What an update from one setup folder to another makes a player's setup run again, by the inputs each step's key reads
(installer/oot.py), stamped the way that key stamps them (code_stamp for the lift and the prompts, the bytes for the
rest). Run it on a release candidate before publishing: a changed shader tool or translator makes every player
translate every shader again (an hour and more on some PCs), a changed lifter or patch file translates the game code
again and compiles every chunk that came out different.

    python tools/update_cost.py <old setup folder> <new setup folder> [--allow lift,shaders]

Exit code 1 when a long step (LONG) would run again and --allow does not name it. tools/make_rekarina_release.py runs
it with --previous. The steps' inputs are the ones installer/oot.py's keys read; tests/update_cost_test.py checks the
two lists against each other where they can be compared.
"""
import argparse
import hashlib
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
from installer.oot import code_stamp, tree_stamp  # noqa: E402

# name, what it costs a player, inputs (files or trees under the setup folder), how they are stamped
STEPS = [
    ('extract', 'seconds', ['tools/nso_plain.py'], tree_stamp),
    ('lift', 'LONG: the game code translated again (2-4 min) and every chunk that came out different compiled again (up '
             'to the whole compile: 12 min on 16 threads, about an hour on a 16 GB laptop)',
     ['recomp/patches-oot-v111.txt', 'recomp', 'tools/cheats.py', 'tools/exefs_patches.py'], code_stamp),
    ('compile', 'the runtime compiled and linked again (30 s to 5 min when the lift stands)',
     ['runtime', 'tools/build_game.py', 'data/pgo-oot.profdata'], tree_stamp),
    ('prompts', 'about a minute: the button pictures are drawn again',
     ['tools/make_oot_prompts.py', 'tools/oot_hint_layout.py', 'tools/button_icons.py', 'tools/make_ext_font.py'],
     code_stamp),
    ('shaders', 'LONG: every shader translated again (20 min to over an hour)',
     ['tools/oot_prepare_shaders.py', 'tools/prepare_shaders.py', 'tools/oot_romfs.py', 'tools/shader_census.py',
      'tools/data/shader-answers-oot.bin.gz', 'bin/rekarina_xlate.id', 'bin/totk_xlate.id'], tree_stamp),
    ('package', 'the copy into the game folder (seconds)',
     ['tools/package_oot.py', 'tools/pack_shader_cache.py', 'tools/oot_prepare_textures.py'], tree_stamp),
]
LONG = {'lift', 'shaders'}
# Each step's key names its form ('oot-lift-4'); a new form (a key stamped another way) runs the step again whatever its
# inputs say. The forms are read from each setup folder's installer/oot.py.
FORMS = {'extract': 'oot-extract', 'lift': 'oot-lift', 'compile': 'oot-compile', 'prompts': 'oot-prompts',
         'shaders': 'oot-prepare', 'package': 'oot-package'}


def key_forms(root, step):
    """The key forms (fingerprint names) installer/oot.py under root gives the step, sorted; [] without the file. A
    form the step only adopts records of (Step.adopts: the old form, recomputed) counts with the others."""
    try:
        text = (root / 'installer' / 'oot.py').read_text(encoding='utf-8', errors='replace')
    except OSError:
        return []
    return sorted(set(re.findall(r"fingerprint\('(" + re.escape(FORMS[step]) + r"-\d+)'", text)))


def form_change(old, new, name, inputs):
    """'' when the step's key forms are the same, or when every old form is one the new setup still adopts (it
    computes it, Step.adopts) and the step's inputs are byte for byte the old ones; else what changed."""
    fo, fn = key_forms(old, name), key_forms(new, name)
    if fo == fn:
        return ''
    line = f'installer/oot.py: the key\'s form {", ".join(fo) or "-"} -> {", ".join(fn) or "-"}'
    if fo and set(fo) <= set(fn) and all(stamp(old, rel, tree_stamp)[0] == stamp(new, rel, tree_stamp)[0] for rel in inputs):
        return ''   # adopted: the record of the old form stands
    return line + (' (records of the old form are adopted only while the inputs keep their bytes)'
                   if fo and set(fo) <= set(fn) else '')


def stamp(root, rel, how):
    """(the input's stamp, {file: its stamp}) under root; ('missing', {}) when it is not there."""
    p = root / rel
    if not p.exists():
        return 'missing', {}
    files = sorted(f for f in p.rglob('*') if f.is_file() and '__pycache__' not in f.parts) if p.is_dir() else [p]
    each = {f.relative_to(root).as_posix(): how(f) for f in files}
    return hashlib.sha256(repr(sorted(each.items())).encode()).hexdigest(), each


def compare(old, new):
    """[(step, cost, [changed input lines])] for every step, changed or not."""
    out = []
    for name, cost, inputs, how in STEPS:
        changed = []
        form = form_change(old, new, name, inputs)
        if form:
            changed.append(form)
        for rel in inputs:
            so, fo = stamp(old, rel, how)
            sn, fn = stamp(new, rel, how)
            if so != sn:
                files = sorted(k for k in set(fo) | set(fn) if fo.get(k) != fn.get(k))
                changed.append(f'{rel}: ' + (', '.join(files[:6]) + (f' (+{len(files) - 6} more)' if len(files) > 6 else '')
                                            if files else 'missing in one of them'))
        out.append((name, cost, changed))
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('old', type=Path)
    ap.add_argument('new', type=Path)
    ap.add_argument('--allow', default='', help='long steps that may run again, comma separated (lift, shaders)')
    a = ap.parse_args(argv)
    allow = {x for x in a.allow.split(',') if x}
    bad = []
    for name, cost, changed in compare(a.old, a.new):
        if changed:
            print(f'[RUNS AGAIN] {name}: {cost}')
            for c in changed:
                print(f'      {c}')
            if name in LONG and name not in allow:
                bad.append(name)
        else:
            print(f'[unchanged ] {name}')
    if bad:
        print(f'\nLONG steps would run again for every player: {", ".join(bad)}. Publish only if that is meant '
              f'(--allow={",".join(bad)}).')
        return 1
    print('\nNo long step runs again on update.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
