"""The in-game menu's grain texture (runtime/ui/tex/grain.a8): 256x256 bytes, a tileable fine grain with a slow
mottling under it, as the wood and leather surfaces of the game's own menus have. ui/menu.cpp packs it into the font
atlas as the alpha of a white square and draws it in a dark tint over flat fills.

    python tools/make_menu_textures.py
"""
from pathlib import Path

import numpy as np

SIZE = 256
OUT = Path(__file__).resolve().parents[1] / 'runtime' / 'ui' / 'tex' / 'grain.a8'


def blur_wrap(a, r):
    """A box blur of radius r that wraps at the edges, so the result still tiles."""
    out = np.zeros_like(a)
    for dy in range(-r, r + 1):
        for dx in range(-r, r + 1):
            out += np.roll(np.roll(a, dy, 0), dx, 1)
    return out / (2 * r + 1) ** 2


def main():
    rng = np.random.default_rng(1998)
    fine = blur_wrap(rng.normal(size=(SIZE, SIZE)), 1)
    fibre = blur_wrap(rng.normal(size=(SIZE, SIZE)), 1)
    fibre = sum(np.roll(fibre, k, 1) for k in range(-6, 7)) / 13          # short horizontal strokes
    slow = np.kron(rng.normal(size=(SIZE // 32, SIZE // 32)), np.ones((32, 32)))
    slow = blur_wrap(blur_wrap(slow, 12), 12)
    g = fine / fine.std() * 0.55 + fibre / fibre.std() * 0.30 + slow / slow.std() * 0.45
    g = (g - g.min()) / (g.max() - g.min())
    g = np.clip((g - 0.5) * 1.5 + 0.5, 0, 1)
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_bytes((g * 255 + 0.5).astype(np.uint8).tobytes())
    print(f'{OUT}: {SIZE}x{SIZE}, mean {g.mean():.3f}')


if __name__ == '__main__':
    main()
