"""The README's frame-rate chart and the table of docs/performance.md from a results file of tools/bench_oot.py.

    python tools/make_perf_charts.py <results.json> [--out docs/media] [--doc docs/performance.md]

perf-locations.svg: one group per location, one bar per mode, the mean frame rate written at the bar's end and the
1 % low as a tick on the bar. A mode with generated frames is drawn at the rate shown on screen, with the rendered
rate in its label. The colours are the port's own (amber and cream on dark wood); the picture needs no legend to be
read, each bar carries its mode's name.
"""
import argparse
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WOOD, PANEL, LINE = '#2a1d12', '#1e1a12', '#5c4a2e'
CREAM, DIM, AMBER, KHAKI = '#f6efd6', '#d9cfae', '#d9951f', '#8c7648'
FONT = 'Segoe UI, Roboto, Helvetica, Arial, sans-serif'


def esc(s):
    return str(s).replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;')


def chart(results):
    runs = results['runs']
    locations = list(runs)
    modes = []
    for loc in locations:
        for m in runs[loc]:
            if m not in modes:
                modes.append(m)
    colours = [AMBER, CREAM, KHAKI]

    def shown(r):
        return r.get('shown_fps', r.get('fps', 0)), r.get('shown_low1', r.get('low1', 0))

    top = max((shown(r)[0] for loc in locations for r in runs[loc].values() if 'fps' in r), default=100)
    scale_max = max(120, int((top * 1.12) // 20 + 1) * 20)
    width, left, right, bar_h, gap, group_gap, head = 1200, 300, 150, 34, 8, 30, 86
    plot_w = width - left - right
    height = head + sum(len([m for m in modes if m in runs[loc]]) * (bar_h + gap) + group_gap for loc in locations) + 46
    o = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} {height}" width="{width}" height="{height}" '
         f'font-family="{FONT}" role="img" aria-label="Frame rate by location">',
         f'<rect width="{width}" height="{height}" fill="{WOOD}"/>',
         f'<text x="{left}" y="36" fill="{CREAM}" font-size="22" font-weight="600">Frames per second, '
         f'{esc(results.get("resolution", ""))}</text>',
         f'<text x="{left}" y="60" fill="{DIM}" font-size="15">mean of one minute; the tick marks the slowest 1 % of frames'
         f'{" · Rekarina " + esc(results["version"]) if results.get("version") else ""}</text>']
    for v in range(0, scale_max + 1, 20):   # the scale: a hairline every 20 fps, a stronger one at the console's 60
        x = left + plot_w * v / scale_max
        o.append(f'<line x1="{x:.1f}" y1="{head - 10}" x2="{x:.1f}" y2="{height - 40}" stroke="{LINE}" '
                 f'stroke-width="{2 if v == 60 else 1}" opacity="{1 if v == 60 else 0.55}"/>')
        o.append(f'<text x="{x:.1f}" y="{height - 18}" fill="{DIM}" font-size="13" text-anchor="middle">{v}</text>')
    x60 = left + plot_w * 60 / scale_max
    o.append(f'<text x="{x60 + 6:.1f}" y="{head - 14}" fill="{DIM}" font-size="13">60: the console</text>')
    y = head
    for loc in locations:
        rows = [m for m in modes if m in runs[loc]]
        mid = y + (len(rows) * (bar_h + gap) - gap) / 2
        o.append(f'<text x="{left - 18}" y="{mid + 7:.1f}" fill="{CREAM}" font-size="20" font-weight="600" '
                 f'text-anchor="end">{esc(loc)}</text>')
        for m in rows:
            r = runs[loc][m]
            colour = colours[modes.index(m) % len(colours)]
            o.append(f'<rect x="{left}" y="{y}" width="{plot_w}" height="{bar_h}" fill="{PANEL}"/>')
            if 'fps' not in r:
                o.append(f'<text x="{left + 12}" y="{y + bar_h * 0.68:.1f}" fill="{DIM}" font-size="15">{esc(m)}: '
                         f'not measured ({esc(r.get("error", ""))})</text>')
            else:
                fps, low = shown(r)
                w = plot_w * min(fps, scale_max) / scale_max
                o.append(f'<rect x="{left}" y="{y}" width="{w:.1f}" height="{bar_h}" fill="{colour}"/>')
                lx = left + plot_w * min(low, scale_max) / scale_max
                o.append(f'<rect x="{lx - 1.5:.1f}" y="{y - 3}" width="3" height="{bar_h + 6}" fill="{WOOD}"/>')
                ink = WOOD
                o.append(f'<text x="{left + 12}" y="{y + bar_h * 0.68:.1f}" fill="{ink}" font-size="15" font-weight="600">'
                         f'{esc(m)}</text>')
                label = f'{fps:.0f}' + (f' ({r["fps"]:.0f} rendered)' if 'shown_fps' in r else '')
                o.append(f'<text x="{left + w + 10:.1f}" y="{y + bar_h * 0.7:.1f}" fill="{CREAM}" font-size="18" '
                         f'font-weight="600">{label}</text>')
            y += bar_h + gap
        y += group_gap - gap
    o.append('</svg>')
    return '\n'.join(o) + '\n'


def table(results):
    m = results.get('machine', {})
    lines = ['# Performance', '',
             f'Measured {results.get("date", "")} with Rekarina {results.get("version", "")} at {results.get("resolution", "")}, '
             'by `tools/bench_oot.py` (its header says how a run is made).', '',
             '| | |', '|---|---|',
             f'| GPU | {m.get("gpu", "")} |', f'| CPU | {m.get("cpu", "")} |', f'| Memory | {m.get("memory_gb", "")} GB |',
             f'| System | {m.get("os", "")} |', '',
             '| Location | Mode | Mean fps | 1 % low | Shown on screen | Frames over 25 ms |', '|---|---|---|---|---|---|']
    for loc, modes in results['runs'].items():
        for name, r in modes.items():
            if 'fps' not in r:
                lines.append(f'| {loc} | {name} | not measured: {r.get("error", "")} | | | |')
            else:
                shown = f'{r["shown_fps"]:.0f} (1 % low {r["shown_low1"]:.0f})' if 'shown_fps' in r else 'the same'
                lines.append(f'| {loc} | {name} | {r["fps"]:.1f} | {r["low1"]:.1f} | {shown} | {r["over_25ms"]} of {r["frames"]} |')
    lines += ['', '## Measuring on your machine', '',
              'Make a plan file as the header of `tools/bench_oot.py` shows (a copy of your profile folder per location and '
              'an input script that loads the save and turns the camera), then:', '',
              '```', 'python tools/bench_oot.py --game <game folder> --plan plan.json --out results.json',
              'python tools/make_perf_charts.py results.json', '```', '',
              'Close other programs that use the GPU first: the tool refuses to start while the GPU is busy.', '']
    return '\n'.join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('results', type=Path)
    ap.add_argument('--out', type=Path, default=ROOT / 'docs' / 'media')
    ap.add_argument('--doc', type=Path, default=ROOT / 'docs' / 'performance.md')
    a = ap.parse_args()
    results = json.loads(a.results.read_text(encoding='utf-8'))
    a.out.mkdir(parents=True, exist_ok=True)
    (a.out / 'perf-locations.svg').write_bytes(chart(results).encode('utf-8'))
    a.doc.write_bytes(table(results).encode('utf-8'))
    print(f'wrote {a.out / "perf-locations.svg"} and {a.doc}')


if __name__ == '__main__':
    main()
