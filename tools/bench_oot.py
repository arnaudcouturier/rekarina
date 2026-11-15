"""Frame-rate runs of Rekarina for the README's charts and docs/performance.md.

    python tools/bench_oot.py --game <game folder> --plan <plan.json> --out <results.json> [--only <location>]

The plan names the places and the modes; saves are the player's own and are not in the repository:

    {"resolution": "2560x1440",
     "locations": [{"name": "Forest", "profile": "<a copy of a profile folder>", "script": "<input script>"},
                   {"name": "Market", "profile": "...", "script": "...", "warp": "market"}],
     "modes": [{"name": "Native", "env": {"REKARINA_DLAA": "0", "REKARINA_TAA": "off", "REKARINA_FRAMEGEN": "off"}},
               {"name": "DLSS + frame generation", "generated": true,
                "env": {"REKARINA_DLAA": "1", "REKARINA_DLSS": "quality", "REKARINA_TAA": "dlss", "REKARINA_FRAMEGEN": "dlss"}}]}

A location with "warp" is reached from its save by a map change 25 s into the run (REKARINA_WARP_AT, runtime/oot_warp.c:
forest, field, market, village), so one save serves several places.

Each run starts the game in its own folder on a fresh copy of the profile (the player's save is never opened), with
the frame limit at 240 and the overlay off, for 125 s: the game logs a frame-time summary per minute of play, the
first of which holds the loading, so the second is the result (mean fps, 1 % low, frames over 25 ms). A mode marked
"generated" shows two frames for each the game renders; the result keeps the rendered rate and doubles it for
"shown". A run is refused while another program keeps the GPU busy, and thrown away when the log has a FATAL line.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import tempfile
import time
from pathlib import Path

SECONDS = 125
SUMMARY = re.compile(r'frametime: (\S+): (\d+) frames in ([\d.]+) s, mean ([\d.]+) fps, 1% low ([\d.]+) fps, over 25 ms (\d+)')


def gpu_busy():
    """The GPU's load in percent from nvidia-smi, or None without it."""
    try:
        out = subprocess.run(['nvidia-smi', '--query-gpu=utilization.gpu', '--format=csv,noheader,nounits'],
                             capture_output=True, text=True, timeout=10).stdout.strip().splitlines()
        return int(out[0]) if out else None
    except (OSError, ValueError, subprocess.TimeoutExpired):
        return None


def machine():
    """What the README's hardware table lists; every field best effort."""
    def ps(expr):
        try:
            return subprocess.run(['powershell', '-NoProfile', '-Command', expr], capture_output=True, text=True,
                                  timeout=30).stdout.strip()
        except (OSError, subprocess.TimeoutExpired):
            return ''
    gpu = ''
    try:
        gpu = subprocess.run(['nvidia-smi', '--query-gpu=name,memory.total,driver_version', '--format=csv,noheader'],
                             capture_output=True, text=True, timeout=10).stdout.strip().splitlines()[0]
    except (OSError, IndexError, subprocess.TimeoutExpired):
        pass
    return {'cpu': ps('(Get-CimInstance Win32_Processor).Name'),
            'gpu': gpu or ps('(Get-CimInstance Win32_VideoController | Select-Object -First 1).Name'),
            'memory_gb': ps('[math]::Round((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB)'),
            'os': ps('(Get-CimInstance Win32_OperatingSystem).Caption')}


def run(game, exe, location, mode, resolution, keep_log):
    busy = gpu_busy()
    if busy is not None and busy > 30:
        return {'error': f'GPU busy ({busy} %): not started'}
    with tempfile.TemporaryDirectory(prefix='rekarina-bench-') as tmp:
        profile = Path(tmp) / 'profile'
        shutil.copytree(location['profile'], profile)
        env = dict(os.environ)
        env.update({'REKARINA_PROFILE_DIR': str(profile), 'REKARINA_INPUT_SCRIPT': location['script'], 'REKARINA_FPS_LIMIT': '240',
                    'REKARINA_REALTIME': '1', 'REKARINA_PERF_OVERLAY': 'off', 'REKARINA_FULLSCREEN': '1', 'REKARINA_AUDIO': 'none',
                    'REKARINA_OUTPUT_RES': resolution, 'REKARINA_WINDOW_SIZE': resolution, 'REKARINA_UPDATE_CHECK': '0'})
        if location.get('warp'):
            env['REKARINA_WARP_AT'] = f'25:{location["warp"]}'
        env.update(mode.get('env', {}))
        log = Path(tmp) / 'run.log'
        with open(log, 'wb') as f:
            try:
                subprocess.run([str(game / exe), '--seconds', str(SECONDS)], cwd=game, env=env, stdout=f,
                               stderr=subprocess.STDOUT, timeout=SECONDS + 240)
            except subprocess.TimeoutExpired:
                return {'error': 'the game did not end in time'}
        text = log.read_text(encoding='utf-8', errors='replace')
        if keep_log:
            shutil.copy(log, keep_log)
    if 'FATAL' in text:
        return {'error': 'FATAL in the log'}
    rows = [m.groups() for m in SUMMARY.finditer(text)]
    full = [r for r in rows if r[0] != 'exit']
    if len(full) < 2:
        return {'error': f'{len(full)} whole minutes in the log, 2 needed'}
    _tag, frames, secs, mean, low, over25 = full[1]
    out = {'fps': float(mean), 'low1': float(low), 'frames': int(frames), 'seconds': float(secs), 'over_25ms': int(over25)}
    if mode.get('generated'):
        out['shown_fps'], out['shown_low1'] = 2 * out['fps'], 2 * out['low1']
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--game', type=Path, required=True, help='the installed game folder')
    ap.add_argument('--exe', default='Rekarina.exe')
    ap.add_argument('--plan', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--only', help='run this location only')
    ap.add_argument('--logs', type=Path, help='keep each run\'s log in this folder')
    a = ap.parse_args()
    plan = json.loads(a.plan.read_text(encoding='utf-8'))
    results = json.loads(a.out.read_text(encoding='utf-8')) if a.out.is_file() else {}
    results.update({'resolution': plan['resolution'], 'machine': machine(), 'date': time.strftime('%Y-%m-%d'),
                    'version': (a.game / 'VERSION').read_text().strip() if (a.game / 'VERSION').is_file() else ''})
    runs = results.setdefault('runs', {})
    if a.logs:
        a.logs.mkdir(parents=True, exist_ok=True)
    for loc in plan['locations']:
        if a.only and loc['name'] != a.only:
            continue
        for mode in plan['modes']:
            keep = a.logs / f"{loc['name']}-{mode['name']}.log".replace(' ', '_') if a.logs else None
            r = run(a.game, a.exe, loc, mode, plan['resolution'], keep)
            runs.setdefault(loc['name'], {})[mode['name']] = r
            print(f"{loc['name']} / {mode['name']}: {r}", flush=True)
            a.out.write_bytes((json.dumps(results, indent=1, ensure_ascii=False) + '\n').encode('utf-8'))
            time.sleep(5)


if __name__ == '__main__':
    main()
