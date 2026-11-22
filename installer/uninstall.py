"""Rekarina's uninstaller, the Python side (Windows): what the setup leaves in the game folder for it, the Windows
"Apps & features" entry, and the launcher's way in. The work itself is uninstall.ps1, the one implementation for
every way in (its header says what goes, what never goes, and how saves stay).

- install(game, version): the script into the game folder as rekarina-uninstall.ps1 (byte for byte: its byte order
  mark makes Windows PowerShell read it as UTF-8), "Uninstall Rekarina" beside it (PowerShell with the script; the
  game's icon), and the per-user entry HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\RekarinaFork (no
  administrator), whose uninstall command is the same. Called by every setup run that leaves a game in the folder
  (installer/oot.py launcher_shortcuts), so the entry follows the game folder and the version.
- plan(game): what would go, as uninstall.ps1 -List says (the launcher's confirmation sheet).
- start(game, keep_saves): the script in a PowerShell of its own, after this process has ended (-WaitPid), without
  asking again: the launcher asked. Its last message box says what was done.
"""
import json
import os
import shutil
import subprocess
from pathlib import Path

from . import fork

SCRIPT = Path(__file__).resolve().parent / 'uninstall.ps1'
GAME_SCRIPT = 'rekarina-uninstall.ps1'
LINK = 'Uninstall Rekarina.lnk'
REG_PATH = rf'Software\Microsoft\Windows\CurrentVersion\Uninstall\{fork.APP_ID}'
WIN = os.name == 'nt'


def powershell():
    """Windows PowerShell by its full path (a PATH that names another powershell.exe changes nothing)."""
    root = os.environ.get('SystemRoot') or r'C:\Windows'
    exe = Path(root) / 'System32' / 'WindowsPowerShell' / 'v1.0' / 'powershell.exe'
    return str(exe) if exe.is_file() else 'powershell.exe'


def script_args(game_script):
    return ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-WindowStyle', 'Hidden', '-File', str(game_script)]


def command_line(game_script):
    return subprocess.list2cmdline([powershell()] + script_args(game_script))


def write_link(link, target, args, workdir, icon, description):
    """A shortcut with arguments, written by the shell's own writer as installer/oot.py write_shortcut does (the
    values in the environment, not in the command). (ok, what PowerShell said)."""
    script = ('$s = (New-Object -ComObject WScript.Shell).CreateShortcut($env:REKARINA_LNK); '
              '$s.TargetPath = $env:REKARINA_LNK_TARGET; $s.Arguments = $env:REKARINA_LNK_ARGS; '
              '$s.WorkingDirectory = $env:REKARINA_LNK_DIR; $s.IconLocation = $env:REKARINA_LNK_ICON; '
              '$s.Description = $env:REKARINA_LNK_TEXT; $s.Save()')
    env = dict(os.environ, REKARINA_LNK=str(link), REKARINA_LNK_TARGET=str(target), REKARINA_LNK_ARGS=args,
               REKARINA_LNK_DIR=str(workdir), REKARINA_LNK_ICON=str(icon), REKARINA_LNK_TEXT=description)
    proc = subprocess.run([powershell(), '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-Command', script],
                          env=env, capture_output=True, text=True, timeout=60,
                          creationflags=subprocess.CREATE_NO_WINDOW if WIN else 0)
    return proc.returncode == 0 and Path(link).is_file(), (proc.stderr or proc.stdout or '').strip()


def folder_kib(*folders):
    """The size of folders in KiB, for the entry's EstimatedSize (links are not followed)."""
    total = 0
    for folder in folders:
        for dirpath, dirnames, filenames in os.walk(folder):
            dirnames[:] = [d for d in dirnames if not os.path.islink(os.path.join(dirpath, d))]
            for f in filenames:
                try:
                    total += os.path.getsize(os.path.join(dirpath, f))
                except OSError:
                    pass
    return total // 1024


def register(game, version, setup=None, key=REG_PATH):
    """The per-user "Apps & features" entry for the game folder. Returns the values written."""
    import winreg
    game = Path(game)
    values = {
        'DisplayName': 'Rekarina',
        'DisplayVersion': str(version),
        'Publisher': 'Rekarina (GitHub fork)',
        'DisplayIcon': str(game / 'Rekarina.exe'),
        'InstallLocation': str(game),
        'UninstallString': command_line(game / GAME_SCRIPT),
        'URLInfoAbout': 'https://github.com/' + fork.REPOSITORY,
    }
    dwords = {'NoModify': 1, 'NoRepair': 1,
              'EstimatedSize': min(folder_kib(*[f for f in (game, setup) if f]), 0xFFFFFFFF)}
    with winreg.CreateKey(winreg.HKEY_CURRENT_USER, key) as k:
        for name, value in values.items():
            winreg.SetValueEx(k, name, 0, winreg.REG_SZ, value)
        for name, value in dwords.items():
            winreg.SetValueEx(k, name, 0, winreg.REG_DWORD, value)
    return dict(values, **dwords)


def install(game, version, setup=None, key=REG_PATH, log=None):
    """The uninstaller into the game folder, its shortcut and the registry entry. Each part that fails is logged and
    costs nothing else. Returns the names of the parts written."""
    say = log or (lambda _text: None)
    game = Path(game)
    done = []
    try:
        fork.mark_install(game)
        shutil.copyfile(SCRIPT, game / GAME_SCRIPT)
        done.append(GAME_SCRIPT)
    except OSError as e:
        say(f'uninstaller: {game / GAME_SCRIPT} not written ({e})')
        return done
    ok, said = write_link(game / LINK, powershell(), subprocess.list2cmdline(script_args(game / GAME_SCRIPT)), game,
                          str(game / 'Rekarina.exe') + ',0', 'Remove Rekarina from this PC (your saves stay unless you say otherwise)')
    if ok:
        done.append(LINK)
    say(f'uninstaller: shortcut {game / LINK}: ' + ('written' if ok else f'not written ({said})'))
    try:
        register(game, version, setup, key)
        done.append('registry')
        say(f'uninstaller: the "Apps & features" entry names {game}')
    except OSError as e:
        say(f'uninstaller: the "Apps & features" entry not written ({e})')
    return done


def run_script(game, *args, env=None):
    """uninstall.ps1 of the setup (the newest copy), for the game folder; (exit code, stdout, stderr)."""
    cmd = [powershell(), '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', str(SCRIPT),
           '-GameDir', str(game), *args]
    proc = subprocess.run(cmd, capture_output=True, text=True, encoding='utf-8', timeout=600, env=env,
                          creationflags=subprocess.CREATE_NO_WINDOW if WIN else 0)
    return proc.returncode, proc.stdout, proc.stderr


def as_list(value):
    if value is None or value == '':
        return []
    return value if isinstance(value, list) else [value]


def plan(game, env=None, extra=()):
    """What uninstall.ps1 would remove for the game folder: its -List, with every list a list."""
    code, out, err = run_script(game, '-List', *extra, env=env)
    if code != 0:
        raise OSError((err or out or f'exit {code}').strip())
    p = json.loads(out)
    for k in ('items', 'shortcuts', 'dumps_kept', 'refused'):
        p[k] = as_list(p.get(k))
    return p


def start(game, keep_saves, wait_pid=None, extra=(), env=None):
    """The removal in a PowerShell of its own, after the process wait_pid (this one by default) has ended: the
    launcher starts it and closes. Its working folder is the temp folder, so it holds none of ours open. (env and
    extra: a test's application data folders and its -Quiet, -StartMenu, -Desktop and -RegKey.)"""
    work = os.environ.get('TEMP') or os.environ.get('TMP') or str(Path.home())
    cmd = [powershell(), '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-WindowStyle', 'Hidden',
           '-File', str(SCRIPT), '-GameDir', str(game), '-Yes', '-KeepSaves', 'yes' if keep_saves else 'no',
           '-WaitPid', str(wait_pid or os.getpid()), *extra]
    flags = 0
    if WIN:   # a hidden console of its own: without any console (DETACHED_PROCESS) PowerShell ends at once, running nothing
        flags = subprocess.CREATE_NEW_PROCESS_GROUP | subprocess.CREATE_NO_WINDOW
    return subprocess.Popen(cmd, cwd=work, creationflags=flags, close_fds=True, env=env)
