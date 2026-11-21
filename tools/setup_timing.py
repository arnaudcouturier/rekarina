"""One fresh install timed, step by step, for the release checks (tools/make_rekarina_release.py --timing).

    python tools/setup_timing.py <setup folder> --game <the dump> --dest <empty game folder> --json <out.json>
                                 [--temp <empty folder>] [--appdata <folder>]

Runs the setup folder's own Python on its command line (python -m installer.oot), as the setup window starts it, into
an empty game folder and an empty working folder, so every step runs from nothing: the lift, the compile, the
translation, the package. The step times the setup keeps for its estimates go to a scratch application data folder
(--appdata, default <json>.appdata), not the player's. Prints each step as it ends and writes
{"release", "result", "total", "steps": {step: seconds}, "phases": {"step/phase": seconds from the start}} to --json.
It takes every core for 30 to 60 minutes; the caller holds the machine for it.

compare(old, new) is the release check: the steps that took clearly longer than in the release before.
"""
import argparse
import json
import os
import subprocess
import sys
import time
from pathlib import Path

SLACK = 1.25     # a step may take this many times its time in the release before
GRACE = 60.0     # ... plus this many seconds (short steps vary by more than a quarter)


def run(setup, game, dest, temp=None, appdata=None, out=print):
    """The setup's CLI on dest; returns the timing record."""
    setup, dest = Path(setup), Path(dest)
    python = setup / 'python' / ('python.exe' if os.name == 'nt' else 'bin/python3')
    if not python.is_file():
        python = Path(sys.executable)
    env = dict(os.environ, PYTHONIOENCODING='utf-8', PYTHONUNBUFFERED='1')
    if appdata:
        Path(appdata).mkdir(parents=True, exist_ok=True)
        env['APPDATA'] = env['XDG_CONFIG_HOME'] = str(appdata)
    cmd = [str(python), '-m', 'installer.oot', '--dest', str(dest), '--game', str(game), '--lang', 'en-US',
           '--no-desktop-shortcut'] + (['--temp', str(temp)] if temp else [])
    t0 = time.monotonic()
    record = {'release': (setup / 'VERSION').read_text().strip() if (setup / 'VERSION').is_file() else '',
              'result': None, 'steps': {}, 'phases': {}}
    p = subprocess.Popen(cmd, cwd=str(setup), env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                         encoding='utf-8', errors='replace', creationflags=0x08000000 if os.name == 'nt' else 0)
    for line in p.stdout:
        e = event(line)
        if e:
            note(record, e, time.monotonic() - t0, out)
    code = p.wait()
    record['total'] = round(time.monotonic() - t0, 1)
    if record['result'] is None:
        record['result'] = f'exit code {code}'
    out(f'setup_timing: {record["result"]} after {record["total"]:.0f} s')
    return record


def event(line):
    line = line.strip()
    if not line.startswith('{'):
        return None
    try:
        e = json.loads(line)
    except ValueError:
        return None
    return e if isinstance(e, dict) else None


def note(record, e, now, out=print):
    """One event of the setup's output (installer/events.py) into the record."""
    kind = e.get('type')
    if kind == 'step' and e.get('state') == 'done' and e.get('step'):
        seconds = e.get('seconds')
        record['steps'][e['step']] = round(float(seconds if isinstance(seconds, (int, float)) else now), 1)
        out(f'{now:7.0f} s  {e["step"]} done in {record["steps"][e["step"]]:.0f} s')
    elif kind == 'progress' and e.get('step'):
        key = f'{e["step"]}/{e.get("phase") or ""}'
        if key not in record['phases']:
            record['phases'][key] = round(now, 1)
            out(f'{now:7.0f} s    {key}')
    elif kind == 'end':
        record['result'] = e.get('result')


def compare(old, new, allow=()):
    """[(step, old seconds, new seconds)] for the steps of new that took more than SLACK times and GRACE seconds over
    their time in old, except those in allow."""
    slow = []
    for step, t in new.get('steps', {}).items():
        before = old.get('steps', {}).get(step)
        if before is not None and step not in allow and t > before * SLACK + GRACE:
            slow.append((step, before, t))
    return slow


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('setup', type=Path)
    ap.add_argument('--game', type=Path, required=True)
    ap.add_argument('--dest', type=Path, required=True, help='an empty game folder')
    ap.add_argument('--json', type=Path, required=True)
    ap.add_argument('--temp', type=Path, help='an empty working folder (default: the setup folder\'s temp)')
    ap.add_argument('--appdata', type=Path, help='where the setup keeps its step times (default: <json>.appdata)')
    a = ap.parse_args(argv)
    if a.dest.exists() and any(a.dest.iterdir()):
        sys.exit(f'setup_timing: {a.dest} is not empty: a timed install starts from nothing')
    record = run(a.setup, a.game, a.dest, a.temp, a.appdata or a.json.with_suffix('.appdata'))
    a.json.write_text(json.dumps(record, indent=1) + '\n', encoding='utf-8')
    return 0 if record['result'] == 'ok' else 1


if __name__ == '__main__':
    sys.exit(main())
