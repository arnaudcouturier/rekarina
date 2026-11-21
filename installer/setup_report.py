"""The setup's problem report: one zip with what a report of a setup that failed, stopped or took far too long needs,
made by the launcher whether the game exists yet or not.

The game's own report (Rekarina.exe --make-report, runtime/report.c) holds the game's logs and needs the game; a player
stuck in the setup had none to send. make() writes reports/rekarina-setup-report-<time>.zip in the game folder (or in
the setup's folder when there is no game folder yet) with:

    system.txt          the computer (Windows, processor, memory, graphics card), the setup's version, the disks' room
    window.log          the setup window's log as the player sees it, with the run's times
    setup/logs/...      the setup's logs of its working folder (install-*.log, each tool's log), the newest first
    rekarina-install.json, background.json, launcher-background.log, timings.json
    game-report.zip     the game's own report, when the game exists and makes one

Text only, nothing of the game; the user's home folder and name in any member are replaced by <home> and <user>, as
the game's report does. Nothing is sent anywhere.
"""
import ctypes
import os
import platform
import re
import shutil
import subprocess
import sys
import time
import zipfile
from pathlib import Path

from . import components, eta, state

LOG_TAIL = 8 << 20          # bytes of a log that go in, its end
LOGS_MAX = 40 << 20         # bytes of logs in all (the newest first)
LOGS_DAYS = 14              # logs older than this stay out


def scrubber():
    """A function that replaces the user's home folder (both slash forms) and name in a text."""
    pairs = []
    home = os.environ.get('USERPROFILE') or os.environ.get('HOME') or ''
    if len(home) >= 3:
        pairs += [(home, '<home>'), (home.replace('\\', '/'), '<home>')]
    user = os.environ.get('USERNAME') or os.environ.get('USER') or ''
    if len(user) >= 3:
        pairs.append((user, '<user>'))
    patterns = [(re.compile(re.escape(a), re.IGNORECASE), b) for a, b in pairs]

    def scrub(text):
        for p, b in patterns:
            text = p.sub(b, text)
        return text
    return scrub


def memory():
    """(total, available) bytes of physical memory; (0, 0) when not known."""
    if os.name != 'nt':
        try:
            info = {k: int(v.split()[0]) * 1024 for k, v in (line.split(':', 1) for line in open('/proc/meminfo'))}
            return info.get('MemTotal', 0), info.get('MemAvailable', 0)
        except (OSError, ValueError):
            return 0, 0

    class MemStatus(ctypes.Structure):
        _fields_ = [('dwLength', ctypes.c_uint32), ('dwMemoryLoad', ctypes.c_uint32)] + \
                   [(n, ctypes.c_ulonglong) for n in ('total', 'avail', 'tpf', 'apf', 'tv', 'av', 'aev')]
    s = MemStatus()
    s.dwLength = ctypes.sizeof(s)
    return (s.total, s.avail) if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(s)) else (0, 0)


def processor():
    name = ''
    if os.name == 'nt':
        try:
            import winreg
            with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r'HARDWARE\DESCRIPTION\System\CentralProcessor\0') as k:
                name = winreg.QueryValueEx(k, 'ProcessorNameString')[0].strip()
        except OSError:
            pass
    return name or platform.processor() or os.environ.get('PROCESSOR_IDENTIFIER', '?')


def free_text(path):
    p = Path(path)
    while not p.exists() and p != p.parent:
        p = p.parent
    try:
        u = shutil.disk_usage(p)
        return f'{p}: {u.free / 2**30:.1f} GiB free of {u.total / 2**30:.0f} GiB'
    except OSError as e:
        return f'{p}: not known ({e})'


def system_text(layout, dest, work):
    gb = 2 ** 30
    total, avail = memory()
    try:
        gpu = components.describe(components.detect()) or 'not detected'
    except Exception as e:   # a detection that fails must not cost the report
        gpu = f'not detected ({type(e).__name__}: {e})'
    lines = [f'Rekarina setup problem report, made {time.strftime("%Y-%m-%d %H:%M:%S")}',
             f'Setup: {getattr(layout, "release", "?")} in {getattr(layout, "root", "?")}',
             f'Windows: {platform.platform()} ({platform.machine()})',
             f'Processor: {processor()}, {os.cpu_count()} logical processors',
             f'Memory: {total / gb:.1f} GiB, {avail / gb:.1f} GiB available now',
             f'Graphics card: {gpu}',
             f'Python: {sys.version.split()[0]}',
             f'Game folder: {dest or "(none chosen yet)"}',
             f'Working folder: {work or "(not known)"}',
             'Room on the disks:']
    for p in {str(x) for x in (dest, work) if x}:
        lines.append('  ' + free_text(p))
    if dest and Path(dest).is_dir():
        lines.append('Game folder holds:')
        for name in ('Rekarina.exe', 'shader-prepared.bin', 'shader-warm-done.bin', 'pipeline-cache.bin', 'shader-cache',
                     'tex-cache', 'nv-cache'):
            p = Path(dest) / name
            if p.is_file():
                lines.append(f'  {name}: {p.stat().st_size:,} bytes')
            elif p.is_dir():
                lines.append(f'  {name}/: present')
            else:
                lines.append(f'  {name}: none')
    return '\n'.join(lines) + '\n'


def logs_of(work):
    """The setup's log files in its working folder, the newest first, of the last LOGS_DAYS days."""
    folder = Path(work) / 'logs' if work else None
    if not folder or not folder.is_dir():
        return []
    cut = time.time() - LOGS_DAYS * 86400
    found = [p for p in folder.glob('*.log') if p.is_file() and p.stat().st_mtime >= cut]
    return sorted(found, key=lambda p: p.stat().st_mtime, reverse=True)


def tail(path, n=LOG_TAIL):
    with open(path, 'rb') as f:
        f.seek(0, os.SEEK_END)
        size = f.tell()
        f.seek(max(0, size - n))
        data = f.read()
    return data.decode('utf-8', errors='replace')


def game_report(dest, timeout=120):
    """The game's own report zip (Rekarina.exe --make-report), or None when there is no game or it made none."""
    exe = Path(dest) / 'Rekarina.exe' if dest else None
    if not exe or not exe.is_file():
        return None
    try:
        r = subprocess.run([str(exe), '--make-report'], cwd=str(exe.parent), capture_output=True, timeout=timeout,
                           env=dict(os.environ, REKARINA_REPORT_NO_OPEN='1'),
                           creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
    except (OSError, subprocess.SubprocessError):
        return None
    out = r.stdout.decode('utf-8', 'replace').strip().splitlines()
    return Path(out[-1]) if not r.returncode and out and Path(out[-1]).is_file() else None


def make(layout, dest=None, work=None, window_lines=(), with_game=True):
    """The report's zip (see the module text); returns its path. dest: the game folder (may not exist yet); work: the
    setup's working folder of that game folder (installer/oot.py work_folder)."""
    scrub = scrubber()
    dest = Path(dest) if dest else None
    out_dir = (dest if dest and dest.is_dir() else Path(layout.root)) / 'reports'
    out_dir.mkdir(parents=True, exist_ok=True)
    path = out_dir / time.strftime('rekarina-setup-report-%Y%m%d-%H%M%S.zip')
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as z:
        z.writestr('system.txt', scrub(system_text(layout, dest, work)))
        if window_lines:
            z.writestr('window.log', scrub('\n'.join(window_lines) + '\n'))
        used = 0
        for log in logs_of(work):
            text = tail(log)
            if used + len(text) > LOGS_MAX:
                break
            used += len(text)
            z.writestr(f'setup/logs/{log.name}', scrub(text))
        extra = [(state.NAME, dest / state.NAME if dest else None),
                 ('background.json', dest / 'build-cache' / 'background.json' if dest else None),
                 ('launcher-background.log', dest / 'logs' / 'launcher-background.log' if dest else None),
                 ('timings.json', eta.timings_path())]
        for name, p in extra:
            if p and Path(p).is_file():
                z.writestr(name, scrub(tail(p)))
        if with_game and dest:
            game = game_report(dest)
            if game:
                z.write(game, 'game-report.zip')
        z.writestr('README.txt', 'Rekarina setup problem report. Made on this computer and sent nowhere: attach it to '
                   'your report.\nsystem.txt: the computer, the setup, the disks. window.log: what the setup window '
                   'showed, with times. setup/logs: the setup\'s own logs. game-report.zip: the game\'s report, when the '
                   'game exists.\nNo save data and no game files are in it. Your user name and home folder in any path '
                   'are replaced by <user> and <home>.\n')
    return path


def json_lines(entries, cat):
    """The window's log entries as the lines it shows (guilogic.log_text)."""
    from .guilogic import log_text
    return [t for t in (log_text(cat, e) for e in entries) if t]


if __name__ == '__main__':   # python -m installer.setup_report <game folder>: a report from the command line
    from .oot import find_layout, work_folder
    lay = find_layout()
    d = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else None
    w = work_folder(lay.temp, None, d)[0] if d else None
    print(make(lay, d, w))
