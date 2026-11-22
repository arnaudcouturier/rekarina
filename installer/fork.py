"""Identity of the launcher and update state for the GitHub fork."""
import os
from pathlib import Path

REPOSITORY = 'arnaudcouturier/rekarina'
APP_ID = 'RekarinaFork'
INSTALL_MARKER = 'rekarina-fork.txt'
START_SHORTCUT = 'Rekarina (fork).lnk'
DESKTOP_SHORTCUT = 'Rekarina Launcher (fork).lnk'


def appdata_dir():
    """Per-user launcher settings, update cache and setup timings for this fork."""
    base = os.environ.get('APPDATA') if os.name == 'nt' else os.environ.get('XDG_CONFIG_HOME')
    return Path(base or Path.home() / '.config') / APP_ID


def owns_install(folder):
    """Whether this setup or game folder carries the fork's release marker."""
    try:
        return (Path(folder) / INSTALL_MARKER).read_bytes().strip() == REPOSITORY.encode('ascii')
    except OSError:
        return False


def mark_install(folder):
    """Record the fork's ownership before packaging files into a setup or game folder."""
    (Path(folder) / INSTALL_MARKER).write_text(REPOSITORY + '\n', encoding='ascii')
