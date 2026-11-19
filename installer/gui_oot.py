"""The Rekarina launcher and setup window: python -m installer.gui_oot

The player's whole path in one window: choose the folder with the OoT dump, choose where the game goes,
press Start, watch the steps, press Play. Once the game is built the window opens on its home screen instead: Play, the
mods (a page of its own: the list of the mods folder with a switch per mod, their order, adding and taking out, and the
rebuild a mod's code patch asks for; mods.py), the saves (a page that lists the saves of this game found in the
emulators' and the other ports' folders and copies one into the game folder, keeping the one it replaces; saves.py), the
settings file, the game's folder, the way back to the form for a rebuild, and three lines about this machine: the
graphics card the setup found and the first settings it chose for it (components.py), the optional libraries that are
still missing with a Retry, and whether a newer Rekarina is out, with Update now and its release notes (update.py).
The folders are remembered between runs (launcher.json in the user's application data). A front end over
`python -m installer.oot` (oot.py), built on the TotK window (gui.py: the run of the setup, the
language switch and the event model are its); this file adds the home screen, the launcher's own jobs (the update, the
component download: they run in a thread and feed the same step list), the time column of the step list (eta.py) and
replaces the form, the finish screen and the look. Text: installer/text/<lang>.json, ids "ui.oot_*", "step.oot_*",
"oot.*", "update.*", "preset.*", "comp.*", "eta.*", "ui.mods_*", "mods.*", "ui.saves_*", "saves.*".

The look is the in-game menu's (runtime/ui/menu.cpp): its colours, its typeface, its rows with a dark value cell, its
white cursor. The keyboard works as in the game's menus: on the drawn screens the arrow keys and Tab move the cursor
over the plates and Enter or the space bar presses the one it is on; on the form and the finish screen Tab walks the
fields and buttons, Enter presses the button it is on (in a field: Start) and Esc goes back. On the Mods page Up and
Down walk the list, Enter or the space bar switches the mod, Left, Right and Tab reach the plates; the Saves page
works the same way, with Enter bringing the chosen save in.

    --lang <code>                window language, one of guilogic.UI_LANGUAGES (default: the Windows display language)
    --game DIR --dest DIR        fill the form (a shortcut or a test)
    --toolchain DIR              passed on to installer.oot (a checkout has no compiler of its own)
    --rebuild DIR                the game passes it at its start when the mods of game folder DIR hold code patches it
                                 was built without (build-cache/rebuild.txt, installer/oot.py): the build starts at once
    --after-update               the launcher exe passes it after swapping an update in: the leftovers are removed and
                                 the setup runs on the remembered game folder
    --selftest [--shots DIR]     draw every screen in English and Russian with sample events and close; on Windows
                                 the window is never seen and never takes the keyboard (shots.py)
"""
import argparse
import json
import os
import queue
import re
import shutil
import subprocess
import sys
import threading
import time
import tkinter as tk
import webbrowser
from dataclasses import dataclass
from pathlib import Path
from tkinter import filedialog, messagebox, ttk

from . import components, eta, ini, mods, net, oot, saves, shaderjob, update
from . import shots as screenshots
from . import state as statefile
from tkinter import font as tkfont

from .gui import App, GLYPH, POLL_MS, fix_tcl_paths, grab
from .guilogic import UI_LANGUAGES, Model, amount, describe_event, parse_line, windows_ui_language

# ---- theme ---------------------------------------------------------------------------------------------------------
# Everything about the look is in this block: the colours, the fonts, and how each kind of widget is drawn. The rest
# of the file only names a colour, a font ('RekText', 'RekTitle', ...) or a style ('Go.TButton', 'Row.TLabel', ...) of
# it, so a new look is a change to this block alone.
#
# The window takes its look from the game's own system menu, as the in-game settings menu does (runtime/ui/menu.cpp):
# a brown band across the top with the logo, dark wood behind everything, khaki plates for what the player presses,
# amber for the one thing to press next and for progress, a dark inset for values, a parchment sheet for what is read.
# The colours are the menu's own (menu.cpp layout_menu, option_rows.cpp): the same numbers under the same names.
SHEET_X, SHEET_TEXT, SHEET_BOTTOM = 374, 676, 622   # the text column of the home screen's sheet, in the stage's pixels
WOOD = '#120b06'                                       # the window behind the picture
PANEL, PANEL_EDGE = '#2a1d12', '#46341f'               # the menu's panel and its edge: the form and the finish screen
DESC = '#312318'                                       # the menu's description plate: a notice (the total line's picture)
BAND = '#5f4c33'                                       # the band, where the toolkit has to paint a piece of it
ROW, ROW_HOT, ROW_EDGE = '#6f5a35', '#7c663e', '#8c7648'   # kRow, kRowHot, kRowEdge: a plate, under the pointer, its edge
INSET, INSET_HOT = '#1e1a12', '#2c261b'                # kInset: the value cell of a row, entry fields; under the pointer
AMBER, AMBER_HOT, AMBER_EDGE = '#d08c1e', '#e0a030', '#f0b84a'   # between kAmberTop and kAmberBottom
CREAM, SOFT, DIM = '#f6efd6', '#d9cfae', '#beb496'     # kMenuCream; kMenuDim: small print; a row that waits
OFF_PLATE, OFF_TEXT = '#3a2f1e', '#8a7f66'             # a button that cannot be pressed now
PLATE_SHADE, AMBER_SHADE = '#3f3119', '#8a5a12'        # the soft shadow under a plate's text (the menu's row labels)
INK, INK_DIM, INK_WARN, SHEET_LINE = '#3a2a18', '#5a4424', '#7a2f0c', '#7d6234'   # on the parchment sheet
DONE_TEXT, RED, NOTE = '#cfe3a8', '#ff8a7a', '#ffd27a'   # NOTE is kMenuNote
FORM_W, FINISH_W, PANEL_PAD = 880, 720, 24             # the panel's width on the form and on the finish screen
WRAP = FORM_W - 2 * PANEL_PAD - 4                      # the width text is wrapped to on the form, in pixels
FINISH_WRAP = FINISH_W - 2 * PANEL_PAD - 4

# The typeface is the menu's too: Roboto Medium, loaded from the runtime's sources (a release carries them, the setup
# compiles them) for this process alone. It has one weight, so nothing is set in bold with it: Windows would smear a
# bold out of it. A language it has no letters for takes the Windows interface face of its script, which has a bold.
MENU_FONT = 'Roboto Medium'
MENU_FONT_FILE = ('ui', 'fonts', 'Roboto-Medium.ttf')   # under runtime/
SCRIPT_FONTS = {'ja': 'Yu Gothic UI', 'ko': 'Malgun Gothic', 'zh-Hans': 'Microsoft YaHei UI',
                'zh-Hant': 'Microsoft JhengHei UI'}
# name: (points, set in bold where the face has one, the heading face)
FONTS = {'RekText': (10, False, False), 'RekSmall': (9, False, False), 'RekBody': (11, False, False),
         'RekStrong': (10, True, False), 'RekPlate': (11, True, False), 'RekPlay': (19, True, False),
         'RekTitle': (19, True, True), 'RekTitleSmall': (14, True, True), 'RekHead': (15, True, True)}
HEAD_FONT = 'Georgia'           # screen titles: a bold serif beside the logo's
MARK_FONT = 'Segoe UI Symbol'   # the step marks (gui.py GLYPH): named, or Tk takes them from whatever face it finds first


def load_menu_font(runtime):
    """Make the menu's typeface known to this process (Windows: a private font, gone with the process). True when
    it can be used."""
    path = Path(runtime).joinpath(*MENU_FONT_FILE)
    if os.name != 'nt' or not path.is_file():
        return False
    try:
        import ctypes
        return ctypes.windll.gdi32.AddFontResourceExW(str(path), 0x10, 0) > 0   # FR_PRIVATE
    except (OSError, AttributeError):
        return False


def set_fonts(root, lang, menu_font, base):
    """The window's named fonts for a window language, made or changed in place: every widget and every line on the
    stage names one of them, so a language switch reaches all of them at once. Returns {name: font}; the caller
    keeps it (Tk drops a named font nobody holds)."""
    known = set(tkfont.families(root))
    script = SCRIPT_FONTS.get(lang)
    if script and script in known:
        family, heavy, head = script, True, script
    elif menu_font and not script:
        family, heavy, head = MENU_FONT, False, HEAD_FONT if HEAD_FONT in known else MENU_FONT
    else:
        family, heavy, head = base, True, HEAD_FONT if HEAD_FONT in known and not script else base
    fonts = {}
    marks = MARK_FONT if MARK_FONT in known else base
    for name, (size, strong, heading) in list(FONTS.items()) + [('RekMark', (11, False, None))]:
        face = marks if heading is None else head if heading else family
        options = {'family': face, 'size': size, 'weight': 'bold' if strong and (heavy or face != MENU_FONT) else 'normal'}
        try:
            fonts[name] = tkfont.nametofont(name, root=root)
            fonts[name].configure(**options)
        except tk.TclError:
            fonts[name] = tkfont.Font(root=root, name=name, **options)
    return fonts


def theme(root):
    """ttk's clam theme recoloured; the native Windows theme draws its own buttons and takes no colours."""
    root.configure(background=WOOD)
    s = ttk.Style(root)
    s.theme_use('clam')
    s.configure('.', background=PANEL, foreground=CREAM, fieldbackground=INSET, bordercolor=ROW_EDGE, lightcolor=ROW_EDGE,
                darkcolor=ROW_EDGE, troughcolor=INSET, focuscolor=CREAM, font='RekText')
    s.configure('TFrame', background=PANEL)
    s.configure('TLabel', background=PANEL, foreground=CREAM)
    s.configure('Band.TFrame', background=BAND)
    s.configure('Panel.TFrame', background=PANEL, bordercolor=PANEL_EDGE, lightcolor=PANEL_EDGE, darkcolor=PANEL_EDGE,
                relief='solid', borderwidth=1)
    s.configure('Title.TLabel', background=PANEL, font='RekHead', foreground=CREAM)
    s.configure('Dim.TLabel', foreground=SOFT)
    s.configure('Notice.TLabel', background=DESC, foreground=SOFT, padding=(14, 10))
    s.configure('Err.TLabel', foreground=RED)
    s.configure('Hint.TLabel', foreground=NOTE)                               # the form's "this folder is updated" line
    s.configure('StepDone.TLabel', foreground=DONE_TEXT)                      # the finish screen's list of steps
    s.configure('StepFailed.TLabel', foreground=RED)
    s.configure('StepWait.TLabel', foreground=DIM)
    # a row of the form, like a setting in the game: its name on a khaki strip, its value in a dark cell at the right
    s.configure('Row.TLabel', background=ROW, foreground=CREAM, font='RekBody')
    s.configure('TEntry', fieldbackground=INSET, foreground=CREAM, insertcolor=CREAM, bordercolor=INSET, lightcolor=INSET,
                darkcolor=INSET, selectbackground=AMBER, selectforeground=CREAM, padding=(8, 5))
    s.map('TEntry', bordercolor=[('focus', CREAM)], lightcolor=[('focus', INSET)], darkcolor=[('focus', INSET)])
    # a drop-down: the same dark cell, its arrow on the cell's own colour
    s.configure('TCombobox', fieldbackground=INSET, background=INSET, foreground=CREAM, arrowcolor=CREAM, arrowsize=13,
                bordercolor=INSET, lightcolor=INSET, darkcolor=INSET, selectbackground=INSET, selectforeground=CREAM,
                padding=(8, 5))
    s.map('TCombobox', fieldbackground=[('readonly', 'active', INSET_HOT), ('readonly', INSET)],
          background=[('active', INSET_HOT)], foreground=[('readonly', CREAM)],
          selectbackground=[('readonly', 'active', INSET_HOT), ('readonly', INSET)], selectforeground=[('readonly', CREAM)],
          bordercolor=[('focus', CREAM)], lightcolor=[('active', INSET_HOT)], darkcolor=[('active', INSET_HOT)])
    s.configure('Band.TCombobox', bordercolor=ROW_EDGE)                       # on the band the cell has its own edge
    s.map('Band.TCombobox', bordercolor=[('focus', CREAM)])
    # the plates: the game's "Save / Load / Settings" buttons
    s.configure('TButton', background=ROW, foreground=CREAM, bordercolor=ROW_EDGE, lightcolor=ROW, darkcolor=ROW,
                font='RekStrong', padding=(18, 8))
    s.map('TButton', background=[('disabled', OFF_PLATE), ('pressed', '#5c4a2c'), ('active', ROW_HOT)],
          lightcolor=[('disabled', OFF_PLATE), ('pressed', '#5c4a2c'), ('active', ROW_HOT)],
          darkcolor=[('disabled', OFF_PLATE), ('pressed', '#5c4a2c'), ('active', ROW_HOT)],
          bordercolor=[('disabled', '#5a4a30'), ('focus', CREAM), ('active', CREAM)], foreground=[('disabled', OFF_TEXT)])
    # "Browse...": a dark cell in the row, like the value beside it
    s.configure('Cell.TButton', background=INSET, bordercolor=INSET, lightcolor=INSET, darkcolor=INSET, font='RekText',
                padding=(12, 2))   # the cell gives it its height: a tall script's line still fits
    s.map('Cell.TButton', background=[('pressed', '#14110b'), ('active', INSET_HOT)],
          lightcolor=[('pressed', '#14110b'), ('active', INSET_HOT)], darkcolor=[('pressed', '#14110b'), ('active', INSET_HOT)],
          bordercolor=[('focus', CREAM), ('active', ROW_EDGE)])
    # the one thing to press next
    s.configure('Go.TButton', background=AMBER, foreground=CREAM, bordercolor=AMBER_EDGE, lightcolor=AMBER, darkcolor=AMBER,
                font='RekPlate', padding=(26, 8))
    s.map('Go.TButton', background=[('disabled', OFF_PLATE), ('pressed', '#b87a18'), ('active', AMBER_HOT)],
          lightcolor=[('disabled', OFF_PLATE), ('pressed', '#b87a18'), ('active', AMBER_HOT)],
          darkcolor=[('disabled', OFF_PLATE), ('pressed', '#b87a18'), ('active', AMBER_HOT)],
          bordercolor=[('disabled', '#5a4a30'), ('focus', CREAM), ('active', CREAM)], foreground=[('disabled', OFF_TEXT)])
    s.configure('Vertical.TScrollbar', background=ROW, troughcolor=INSET, bordercolor=INSET, lightcolor=ROW, darkcolor=ROW,
                arrowcolor=CREAM, gripcount=0)
    s.map('Vertical.TScrollbar', background=[('active', ROW_HOT)], lightcolor=[('active', ROW_HOT)],
          darkcolor=[('active', ROW_HOT)])
    # the drop-down list of a combobox is a plain Tk listbox
    for option, value in (('background', INSET), ('foreground', CREAM), ('selectBackground', AMBER),
                          ('selectForeground', CREAM), ('font', 'RekText'), ('borderWidth', 0), ('relief', 'flat'),
                          ('activeStyle', 'none'), ('highlightThickness', 1), ('highlightBackground', ROW_EDGE),
                          ('highlightColor', ROW_EDGE)):
        root.option_add('*TCombobox*Listbox.' + option, value)
    # Enter presses the button the keyboard is on, as it does in the game's menus (the toolkit's own key is the space)
    root.bind_class('TButton', '<Return>', lambda e: e.widget.invoke())
    root.bind_class('TButton', '<KP_Enter>', lambda e: e.widget.invoke())


# The prepared textures' size on disk, C's estimate (tools/oot_prepare_textures.py --estimate) until a full run measures it
TEXTURES_GIB = 12

STAGE_W, STAGE_H, BAND_H = 1120, 700, 62   # the window's picture (tools/make_launcher_art.py) and its band
ART = Path(__file__).resolve().parent / 'art'


def plate_image(src, width):
    """The strip src cut to width: its left part, then its last pixels as the right edge."""
    h, sw, cap = src.height(), src.width(), 6
    width = max(cap * 2, min(int(width), sw))
    img = tk.PhotoImage(width=width, height=h)
    img.tk.call(img, 'copy', src, '-from', 0, 0, width - cap, h, '-to', 0, 0)
    img.tk.call(img, 'copy', src, '-from', sw - cap, 0, sw, h, '-to', width - cap, 0)
    return img


CURSOR_MARGIN, CURSOR_CORNER = 14, 26   # of the cursor pictures: the glow outside the box; a corner piece's side


def cursor_image(src, width, height):
    """The game's cursor (a white frame with a pale glow) for a box of width x height, pieced from the picture of
    one made for another box: its four corners, and its straight sides repeated between them."""
    m, k = CURSOR_MARGIN, CURSOR_CORNER
    w, h, sw, sh = width + 2 * m, height + 2 * m, src.width(), src.height()
    img = tk.PhotoImage(width=w, height=h)

    def piece(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1):
        if dx1 > dx0 and dy1 > dy0:   # a source piece smaller than its place is repeated to fill it
            img.tk.call(img, 'copy', src, '-from', sx0, sy0, sx1, sy1, '-to', dx0, dy0, dx1, dy1, '-compositingrule', 'set')

    for sy, dy in (((0, k), (0, k)), ((sh - k, sh), (h - k, h))):
        piece(0, sy[0], k, sy[1], 0, dy[0], k, dy[1])
        piece(sw - k, sy[0], sw, sy[1], w - k, dy[0], w, dy[1])
        piece(k, sy[0], k + 8, sy[1], k, dy[0], w - k, dy[1])
    for sx, dx in (((0, k), (0, k)), ((sw - k, sw), (w - k, w))):
        piece(sx[0], k, sx[1], k + 8, dx[0], k, dx[1], h - k)
    return img


def tall_plate(src, height):
    """The plate src made `height` pixels tall: its upper half, its lower half, and a band of its middle repeated
    between them (a band, not a row: a row repeated would draw the grain out into stripes)."""
    w, h = src.width(), src.height()
    img = tk.PhotoImage(width=w, height=height)
    half, band = h // 2, h // 6
    img.tk.call(img, 'copy', src, '-from', 0, 0, w, half, '-to', 0, 0)
    img.tk.call(img, 'copy', src, '-from', 0, half - band, w, half + band, '-to', 0, half, w, height - (h - half))
    img.tk.call(img, 'copy', src, '-from', 0, half, w, h, '-to', 0, height - (h - half))
    return img


class StageText:
    """A line of text drawn on the stage, with the two calls the window's text binding makes on a label."""

    def __init__(self, canvas, item):
        self.canvas, self.item = canvas, item

    def configure(self, text=None, **_):
        if text is not None:
            self.canvas.itemconfigure(self.item, text=text)

    def cget(self, _option):
        return self.canvas.itemcget(self.item, 'text')


class Plate:
    """A button drawn on the stage: a plate cut from a strip (kind: 'plate46', 'plate40', 'play76', 'go40') with its
    text over it, lighter under the pointer, one pixel lower while the mouse button is down on it; it acts when the
    button is let go over it, as a button does. The keyboard reaches it through the window's cursor
    (RekarinaApp.move_cursor). It answers what the window asks of a ttk button: configure(text=), cget('text') and
    state(['disabled'] or ['!disabled'])."""

    def __init__(self, app, kind, width, command, font, tags=()):
        self.app, self.canvas, self.kind, self.command, self.font = app, app.stage, kind, command, font
        self.enabled, self.x, self.y, self.width, self.shown = True, 0, 0, 0, True
        self.inside = self.down = False
        self.shade_fill = AMBER_SHADE if kind in ('play76', 'go40') else PLATE_SHADE
        self.image = self.canvas.create_image(0, 0, anchor='nw', tags=tags)
        self.shade = self.canvas.create_text(0, 0, fill=self.shade_fill, font=font, tags=tags)
        self.label = self.canvas.create_text(0, 0, fill=CREAM, font=font, tags=tags)
        for item in (self.image, self.shade, self.label):
            self.canvas.tag_bind(item, '<Enter>', lambda _e: self.hover(True))
            self.canvas.tag_bind(item, '<Leave>', lambda _e: self.hover(False))
            self.canvas.tag_bind(item, '<Button-1>', lambda e: self.press(True, e))
            self.canvas.tag_bind(item, '<ButtonRelease-1>', lambda e: self.press(False, e))
        app.plates_made.append(self)
        self.resize(width)

    @property
    def height(self):
        return self.pictures[0].height()

    def resize(self, width):
        if width != self.width:
            self.width = width
            self.pictures = (self.app.plate(self.kind, width), self.app.plate(self.kind + '-hot', width))
            self.paint()
            self.place(self.x, self.y)

    def paint(self):
        """The plate's picture for what it is now: lighter under the pointer; Play as a dull khaki plate while there
        is no game to start (an amber one would still ask to be pressed)."""
        if not self.enabled and self.kind == 'play76':
            picture = self.app.plate('play76-off', self.width)
        else:
            picture = self.pictures[1 if self.inside and self.enabled else 0]
        self.canvas.itemconfigure(self.image, image=picture)

    def fit(self, pad=26, least=96):
        """As wide as its text needs."""
        text = self.canvas.itemcget(self.label, 'text')
        self.resize(max(least, self.app.fonts[self.font].measure(text) + pad * 2))

    def place(self, x, y):
        self.x, self.y = x, y
        cx, cy = x + self.width // 2, y + self.height // 2 + (1 if self.down else 0)
        self.canvas.coords(self.image, x, y)
        self.canvas.coords(self.shade, cx + 1, cy + 1)
        self.canvas.coords(self.label, cx, cy)
        if self.app.cursor_on is self:
            self.app.place_cursor()

    def show(self, on):
        self.shown = on
        for item in (self.image, self.shade, self.label):
            self.canvas.itemconfigure(item, state='normal' if on else 'hidden')

    def configure(self, text=None, **_):
        if text is not None:
            for item in (self.shade, self.label):
                self.canvas.itemconfigure(item, text=text)

    def cget(self, _option):
        return self.canvas.itemcget(self.label, 'text')

    def state(self, flags):
        for flag in flags:
            if flag in ('disabled', '!disabled'):
                self.enabled = flag == '!disabled'
        self.canvas.itemconfigure(self.label, fill=CREAM if self.enabled else '#b4a47c')
        self.canvas.itemconfigure(self.shade, fill=self.shade_fill if self.enabled else '')   # '' draws nothing
        self.paint()
        self.app.cursor_changed(self)

    def hover(self, on):
        self.inside = on
        self.paint()
        self.canvas.configure(cursor='hand2' if on and self.enabled else '')
        if on and self.enabled:
            self.app.pointer_on(self)

    def press(self, down, event):
        """The mouse button went down or came up on the plate (the canvas keeps sending the plate its events while
        the button is held, wherever the pointer goes: so where it came up is asked of the event)."""
        over = self.x <= event.x < self.x + self.width and self.y <= event.y < self.y + self.height
        acts = not down and self.down and over
        self.down = down and self.enabled
        self.place(self.x, self.y)
        if acts:
            self.invoke()

    def invoke(self):
        if self.enabled and self.shown and self.command:
            self.command()


def theme_text(widget):
    """A plain Tk text box (the log): ttk has none, so it takes its colours here."""
    widget.configure(background=INSET, foreground=SOFT, insertbackground=CREAM, relief='flat', selectbackground=AMBER,
                     selectforeground=CREAM, inactiveselectbackground=ROW)


STEPS_TOP, STEP_PITCH = 124, 54   # the first step row's top on the stage; from one row's top to the next
FORM_VALUE_W = 430                # the value cell of a form row, Browse included


def step_share(row):
    """How much of the running step's cell its amber bar fills, 0..1, from the step's row of the tracker's view: the
    share its counter gives, or without a counter the time it has run against its estimate, never quite full."""
    if not row:
        return 0.0
    if row.get('fraction') is not None:
        return min(1.0, max(0.0, row['fraction']))
    estimate = row.get('seconds_estimated') or 0
    return min(0.95, (row.get('seconds_taken') or 0) / estimate) if estimate > 0 else 0.0


# ---- end of theme --------------------------------------------------------------------------------------------------

# auto: the set of the controller or the keyboard the player used last, switched while the game runs (runtime/input.c)
PROMPTS = ('auto', 'xbox', 'rekarina', 'playstation', 'nintendo')
PROGRESS_EVERY = 0.2   # seconds between two progress events a job's thread sends to the window
FACTS = ('ui.oot_fact_gpu', 'ui.oot_fact_preset', 'ui.oot_fact_components', 'ui.oot_fact_mods')   # the home sheet's table
MODS_TOP, MODS_ROWS = 126, 7      # the Mods page: the first row's top on the stage; the rows it shows at once
SAVES_ROWS = 6                    # the Saves page: the rows it shows at once
SAVES_MARK = {saves.PORT: '\u25c6', saves.EMULATOR: '\u25c7', saves.FOLDER: '\u25cb', saves.ARCHIVE: '\u25cb'}
MODS_MARK = {'on': '\u2714', 'off': '\u25cb', 'no': '\u2013'}   # as the step marks: done, waiting, left out


def notes_text(body):
    """A release's notes as plain lines for the home sheet. GitHub writes them in Markdown: heading marks, emphasis
    marks and code ticks go, a list mark becomes a bullet, a link keeps its words, several empty lines become one."""
    lines = []
    for line in body.splitlines():
        line = re.sub(r'^#{1,6}\s*', '', line.strip())
        line = re.sub(r'^[-*+]\s+', '• ', line)
        line = re.sub(r'\[([^\]]+)\]\([^)]*\)', r'\1', line)
        line = re.sub(r'(\*\*|__)(.+?)\1', r'\2', line).replace('`', '')
        if line or (lines and lines[-1]):
            lines.append(line)
    return '\n'.join(lines).strip()


def settings_path():
    """launcher.json: the folders of the last run, so the window opens on Play."""
    base = os.environ.get('APPDATA') if os.name == 'nt' else os.environ.get('XDG_CONFIG_HOME')
    return Path(base or Path.home() / '.config') / 'Rekarina' / 'launcher.json'


def load_settings():
    try:
        data = json.loads(settings_path().read_text(encoding='utf-8'))
        return data if isinstance(data, dict) else {}
    except (OSError, ValueError):
        return {}


def save_settings(**values):
    data = load_settings()
    data.update({k: v for k, v in values.items() if v})
    try:
        settings_path().parent.mkdir(parents=True, exist_ok=True)
        settings_path().write_text(json.dumps(data, indent=1), encoding='utf-8')
    except OSError:
        pass   # a read-only profile: the window still works, it only forgets


@dataclass
class OotForm:
    game: str = ''
    dest: str = ''
    lang: str = ''
    prompts: str = 'auto'
    dlls: str = ''
    toolchain: str = ''
    textures: bool = True   # the setup's texture step (installer/oot.py --no-textures when off)
    all_shaders: bool = False   # the launcher's background job: every variant of every shader (installer/oot.py --all-shaders)

    def installed_state(self):
        if not self.dest.strip():
            return None
        try:
            state = statefile.load(Path(self.dest.strip()))
        except statefile.StateError:
            return None
        return state if state and state.get('oot') else None

    def problems(self):
        found = []
        update_mode = self.installed_state() is not None
        if not self.game.strip():
            if not update_mode:
                found.append('ui.oot_need_game')
        elif not Path(self.game.strip()).exists():
            found.append('ui.oot_missing_game')
        if not self.dest.strip():
            found.append('ui.need_dest')
        if not update_mode and self.lang not in ini.LANGUAGES:
            found.append('ui.need_lang')
        if self.dlls.strip() and not Path(self.dlls.strip()).is_dir():
            found.append('ui.oot_missing_dlls')
        return found

    def command(self, python, root=None, temp=None, verify_hashes=False):
        cmd = [str(python), '-m', 'installer.oot']
        for flag, value in (('--game', self.game), ('--dest', self.dest), ('--lang', self.lang), ('--prompts', self.prompts),
                            ('--dlls', self.dlls), ('--toolchain', self.toolchain)):
            if value and value.strip():
                cmd += [flag, value.strip()]
        if not self.textures:
            cmd.append('--no-textures')
        # a run on the run screen never prepares the rest of the shaders (the background job does: shaderjob.py)
        cmd.append('--all-shaders' if self.all_shaders else '--shaders-later')
        if root:
            cmd += ['--root', str(root)]
        if temp:
            cmd += ['--temp', str(temp)]
        return cmd


class JobFailed(Exception):
    """A job's failure with a message id of the text catalog and its parameters."""

    def __init__(self, id, **params):
        super().__init__(id)
        self.id, self.params = id, params


class Job:
    """Work of the launcher itself (the update, the component download) in a thread, reporting like the setup's
    command line does: events (events.py) into a queue the window drains. work(emit, cancelled) returns a value or
    raises JobFailed / update.UpdateError / net.Cancelled; outcome is then ('ok', value), ('failed', (id, params)) or
    ('cancelled', None)."""

    def __init__(self, kind, work):
        self.kind = kind
        self.events = queue.Queue()
        self.cancel = threading.Event()
        self.outcome = None
        self.last_progress = 0.0
        self.thread = threading.Thread(target=self.run, args=(work,), daemon=True)
        self.thread.start()

    def emit(self, event):
        if event.get('type') == 'progress' and event.get('done') != event.get('total'):
            now = time.monotonic()
            if now - self.last_progress < PROGRESS_EVERY:
                return
            self.last_progress = now
        self.events.put(event)

    def run(self, work):
        try:
            self.outcome = ('ok', work(self.emit, self.cancel.is_set))
        except net.Cancelled:
            self.outcome = ('cancelled', None)
        except (JobFailed, update.UpdateError) as e:
            self.outcome = ('failed', (e.id, e.params))
        except Exception as e:   # a thread's traceback would go nowhere: the finish screen says what broke
            self.outcome = ('failed', ('internal', {'exception': type(e).__name__, 'detail': str(e)}))


class RekarinaApp(App):
    def __init__(self, root, ui_lang, layout, args=None):
        self.args = args
        self.extra = {n: tk.StringVar() for n in ('game', 'dlls')}
        self.prompts_var = tk.StringVar()
        self.textures_var = tk.StringVar()
        self.tracker = eta.Tracker()     # the times of the step list
        self.step_times = {}
        self.job = None                  # the launcher's own running work, or None
        self.job_title = None            # (text id, params) shown above the step list instead of "Installing"
        self.finish_kind = None          # 'update' or 'components' when the finish screen reports a job
        self.tasks = queue.Queue()       # results of background threads, run on the window's thread by pump
        self.gpu, self.gpu_known = None, False
        self.forced_missing = None       # the self test's stand-in for the files of a game folder
        self.update_info, self.checking = None, False
        self.plates_made = []            # every plate of the stage, in the order the keyboard walks them
        self.cursor_on = None            # the plate the game's cursor is on
        self.cursor_keys = False         # the keyboard moved the cursor on this screen
        self.forced_mods = None          # the self test's stand-in for the mods folder
        self.mod_list, self.mod_sel, self.mod_first = [], None, 0   # the Mods page: the list, the chosen name, the top row
        self.mod_at = 0                  # the chosen mod's place in the list
        self.mod_note, self.mod_busy = None, False                  # (colour, text) under the list; a mod is being copied
        self.forced_saves = None         # the self test's stand-in for the saves found: (list, the game folder's own)
        self.save_list, self.save_at, self.save_first = [], 0, 0    # the Saves page: the list, the chosen row, the top row
        self.save_own, self.save_backups = None, []                 # the game folder's save; what imports replaced
        self.save_note, self.save_busy = None, False                # (colour, text) under the list; a copy is running
        self.save_asked = None           # the source whose import waits for Replace to be pressed
        self.menu_font = load_menu_font(layout.runtime)
        self.base_font = 'Segoe UI' if os.name == 'nt' else 'DejaVu Sans'
        self.fonts = set_fonts(root, ui_lang if ui_lang in UI_LANGUAGES else 'en', self.menu_font, self.base_font)
        super().__init__(root, ui_lang, layout)
        theme(root)
        self.bind_text(self.title, 'text', 'ui.oot_title')
        root.title(self.cat('ui.oot_title'))
        ico = layout.runtime / 'rekarina.ico'
        if os.name == 'nt' and ico.is_file():
            root.iconbitmap(default=str(ico))
        self.build_stage()
        theme_text(self.log)
        self.finish_text.configure(wraplength=FINISH_WRAP)
        self.finish_title.configure(wraplength=FINISH_WRAP)
        self.finish_notice.configure(wraplength=FINISH_WRAP - 28)
        self.frames['home'] = self.build_home()
        for key in ('<Return>', '<KP_Enter>'):   # Enter on the form starts the setup, on the finish screen it plays
            root.bind(key, self.panel_enter)
        root.bind('<Escape>', self.panel_escape)
        saved = load_settings() if args else {}   # the self test draws the same screens on every machine
        rebuild = getattr(args, 'rebuild', None)   # the game folder whose mods need a rebuild (the game starts this)
        game = str(args.game) if args and args.game else saved.get('game', '')
        dest = str(rebuild) if rebuild else str(args.dest) if args and args.dest else saved.get('dest', '')
        if game:
            self.extra['game'].set(game)
        if dest:
            self.vars['dest'].set(dest)
        self.recorded_gpu()
        if self.game_exe().is_file():
            self.refresh_home()
            self.show('home')
        else:
            self.show('form')
            self.panel_focus()
        if args:   # a real run: ask the network and the hardware off the window's thread
            root.after(POLL_MS, self.pump)
            root.after(2000, self.shader_tick)
            self.background(components.detect, self.got_gpu)
            self.check_updates()
            if getattr(args, 'after_update', False):
                self.background(lambda: update.cleanup(self.layout.root), lambda _value: None)
                if self.form().installed_state() is not None:   # rebuild what the new release changed
                    self.pending_title = ('ui.oot_after_update', {'version': net.local_version(self.layout.root)})
                    root.after(POLL_MS, self.start)
            elif rebuild and self.form().installed_state() is not None and not self.form().problems():
                # the game found code patches of its mods that it was built without: built in now, shaders and saves stay
                self.pending_title = ('ui.oot_rebuild_mods', {})
                root.after(POLL_MS, self.start)

    pending_title = None
    forced_shaders = None     # the self test's stand-in for the shader row's view (shader_view)

    # ---- the stage -------------------------------------------------------------------------------------------------
    # The window is one picture (tools/make_launcher_art.py): the painting with the band. The home screen and the
    # run screen are drawn on it; the form and the finish screen are the toolkit's own frames, shown as the menu's
    # dark panel in the middle (gui.py's body). gui.py's run frame is never shown: the drawn run screen reads its
    # texts from that frame's widgets, and the log is a text box of the stage.

    def build_stage(self):
        root, top = self.root, self.title.master
        self.art = {p.stem: tk.PhotoImage(file=str(p)) for p in sorted(ART.glob('*.png'))}   # kept: Tk drops a picture nobody holds
        self.plates, self.cursors = {}, {}
        top.pack_forget()
        self.body.pack_forget()
        self.stage = tk.Canvas(root, width=STAGE_W, height=STAGE_H, highlightthickness=0, borderwidth=0, background=WOOD,
                               takefocus=1)
        self.stage.pack(fill='both', expand=True)
        root.geometry(f'{STAGE_W}x{STAGE_H}')
        root.minsize(STAGE_W, STAGE_H)
        root.resizable(False, False)
        c = self.stage
        c.create_image(0, 0, image=self.art['bg'], anchor='nw')
        c.create_image(26, 16, image=self.art['logo'], anchor='nw')
        version = StageText(c, c.create_text(26 + self.art['logo'].width() + 16, 34, anchor='w', fill=SOFT, font='RekText'))
        self.bind_text(version, 'text', 'ui.version', version=self.layout.release)
        # the language box: the title row with nothing else left in it, at the band's right end
        for w in top.winfo_children():
            if isinstance(w, ttk.Label):
                w.pack_forget()
            elif isinstance(w, ttk.Frame):
                w.configure(style='Band.TFrame')
        top.configure(style='Band.TFrame', padding=0)
        top.lift(c)
        self.ui_box.configure(style='Band.TCombobox', height=len(UI_LANGUAGES))   # every language without a scroll bar
        c.create_window(STAGE_W - 22, BAND_H // 2, window=top, anchor='e')
        # a newer release, where the in-game menu names it: in the band, at the right
        root.update_idletasks()
        self.band_note = c.create_text(STAGE_W - 22 - top.winfo_reqwidth() - 18, BAND_H // 2, anchor='e', fill=NOTE,
                                       font='RekText')
        self.body.configure(style='Panel.TFrame', padding=PANEL_PAD)
        self.body.lift(c)
        self.body_item = c.create_window(STAGE_W // 2, BAND_H + (STAGE_H - BAND_H) // 2, window=self.body, anchor='center')
        self.cursor_item = c.create_image(0, 0, anchor='nw', state='hidden')
        self.build_run_stage()
        self.build_mods_stage()
        self.build_saves_stage()
        self.build_stage_log()
        for key, step in (('<Down>', 1), ('<Right>', 1), ('<Up>', -1), ('<Left>', -1)):
            c.bind(key, lambda _e, step=step, rows=key in ('<Down>', '<Up>'): self.arrow(step, rows))
        c.bind('<Tab>', lambda _e: self.move_cursor(1, leave=True))
        c.bind('<Shift-Tab>', lambda _e: self.move_cursor(-1, leave=True))
        for key in ('<Return>', '<KP_Enter>', '<space>'):
            c.bind(key, lambda _e: self.press_cursor())

    def plate(self, kind, width):
        key = (kind, int(width))
        if key not in self.plates:
            if kind == 'play76-off':   # no strip of its own: a khaki plate at Play's height
                self.plates[key] = tall_plate(self.plate('plate46', width), self.art['play76'].height())
            else:
                self.plates[key] = plate_image(self.art[kind], width)
        return self.plates[key]

    def show(self, name):
        if not hasattr(self, 'stage'):   # gui.py's own first screen, before the stage is there
            return super().show(name)
        changed, self.screen = name != self.screen, name
        drawn = name in ('home', 'run', 'mods', 'saves')
        for tag in ('scr:home', 'scr:run', 'scr:mods', 'scr:saves'):
            self.stage.itemconfigure(tag, state='normal' if tag == 'scr:' + name else 'hidden')
        if changed:
            self.cursor_keys = False
            self.set_cursor(None)
        if drawn:
            self.stage.itemconfigure(self.body_item, state='hidden')
            if name == 'home':
                self.layout_home()
            elif name == 'mods':
                self.refresh_mods()
            elif name == 'saves':
                self.refresh_saves()
            else:
                self.refresh_run()
            if changed:
                self.stage.focus_set()
                if name == 'home':   # the cursor starts on Play, as the game's menus start on their first entry
                    self.set_cursor(next((p for p in self.cursor_order() if p.enabled), None))
        else:
            for f in self.frames.values():
                f.pack_forget()
            self.frames[name].pack(fill='both', expand=True)
            self.stage.itemconfigure(self.body_item, state='normal')
            self.size_panel()
            if changed:
                self.panel_focus()
        self.stage.itemconfigure(self.log_item, state='normal' if name == 'run' and self.log_open else 'hidden')
        # the band's note steps back while the update it names is being installed
        self.stage.itemconfigure(self.band_note, state='hidden' if name == 'run' and self.finish_kind == 'update' else 'normal')
        self.root.update_idletasks()

    def size_panel(self):
        """The panel: as wide as its screen is made for, as tall as that screen asks at this width, in the middle of
        the room under the band."""
        if self.screen not in ('form', 'finish'):
            return
        if self.screen == 'form':
            self.place_form_lines()
        room = STAGE_H - BAND_H - 24
        self.stage.itemconfigure(self.body_item, width=FORM_W if self.screen == 'form' else FINISH_W, height=room)
        self.root.update_idletasks()
        self.stage.itemconfigure(self.body_item, height=min(self.body.winfo_reqheight(), room))

    def fit(self):
        if not hasattr(self, 'stage'):
            return super().fit()
        self.size_panel()   # the window keeps the picture's size; the panel follows what its screen now says
        self.root.update_idletasks()

    def panel_focus(self):
        """The keyboard starts on what the screen is there for: Start on the form, Play (or Back) when a run ended."""
        if self.screen == 'form':
            self.start_button.focus_set()
        elif self.screen == 'finish':
            (self.play_button if self.play_button.instate(['!disabled']) else self.back_button).focus_set()

    def panel_enter(self, event):
        """Enter in a field of the form starts the setup; a button the keyboard is on takes the key itself."""
        if self.screen == 'form' and event.widget.winfo_class() == 'TEntry':
            self.start()

    def panel_escape(self, _event):
        """Esc is Back, as in the game's menus: from the form to the home screen, from the finish screen to where it
        came from."""
        if self.screen == 'form' and self.home_button.instate(['!disabled']):
            self.go_home()
        elif self.screen == 'finish':
            self.leave_finish()
        elif self.screen in ('mods', 'saves'):
            self.go_home()

    # ---- the cursor ------------------------------------------------------------------------------------------------
    # The game's cursor, a white frame with a pale glow, marks the plate that Enter or the space bar presses. On the
    # home screen it is there from the start, on Play, and goes where the pointer or the arrow keys and Tab take it.
    # On the run screen the running step's value cell carries the cursor (build_run_stage), so the plates show
    # theirs only once the keyboard has moved to them.

    def cursor_order(self):
        """The plates of the screen that is shown, in the order they were made: top to bottom, left to right."""
        tag = 'scr:' + str(self.screen)
        return [p for p in self.plates_made if p.shown and tag in self.stage.gettags(p.image)]

    def set_cursor(self, plate):
        self.cursor_on = plate
        self.place_cursor()

    def place_cursor(self):
        p = self.cursor_on
        if p is None or not p.shown or not p.enabled or (self.screen in ('run', 'mods', 'saves') and not self.cursor_keys):
            self.stage.itemconfigure(self.cursor_item, state='hidden')
            return
        key = (p.width, p.height)
        if key not in self.cursors:
            self.cursors[key] = cursor_image(self.art['cursor-cell'], *key)
        self.stage.coords(self.cursor_item, p.x - CURSOR_MARGIN, p.y - CURSOR_MARGIN)
        # 'disabled': drawn, but not what the pointer is over. The picture lies over its plate, and a canvas takes
        # a picture's whole rectangle for the picture: shown as 'normal' it would take the plate's clicks.
        self.stage.itemconfigure(self.cursor_item, image=self.cursors[key], state='disabled')
        self.stage.tag_raise(self.cursor_item)

    def cursor_changed(self, plate):
        """A plate was switched on or off: the cursor does not stay on one that cannot be pressed."""
        if plate is self.cursor_on:
            if not plate.enabled and self.screen == 'home':
                self.cursor_on = next((p for p in self.cursor_order() if p.enabled), None)
            self.place_cursor()

    def pointer_on(self, plate):
        if self.screen == 'home' and plate is not self.cursor_on:
            self.set_cursor(plate)

    def move_cursor(self, step, leave=False):
        """The cursor one plate on (step 1) or back (-1). The arrow keys go round; with leave (Tab) the step past
        the last plate is not taken here, and the toolkit moves the keyboard on to the language box."""
        order = [p for p in self.cursor_order() if p.enabled]
        if not order:
            return None
        at = order.index(self.cursor_on) if self.cursor_on in order else (-1 if step > 0 else 0)
        if leave and self.cursor_keys and not 0 <= at + step < len(order):
            return None
        self.cursor_keys = True
        self.set_cursor(order[(at + step) % len(order)])
        return 'break'

    def press_cursor(self):
        p = self.cursor_on
        if p is not None and p in self.cursor_order() and (self.screen == 'home' or self.cursor_keys):
            p.invoke()
        elif self.screen == 'mods':   # the cursor is in the list: the chosen mod's switch
            self.toggle_mod()
        elif self.screen == 'saves':   # the chosen save comes in
            self.import_chosen_save()
        return 'break'

    def arrow(self, step, rows):
        """An arrow key. On the Mods page Up and Down (rows) walk the list and take the cursor off the plates."""
        if self.screen == 'mods' and rows:
            return self.step_mod(step)
        if self.screen == 'saves' and rows:
            return self.step_save(step)
        return self.move_cursor(step)

    # ---- the log ---------------------------------------------------------------------------------------------------

    def build_stage_log(self):
        """The log as a part of the drawn run screen: a dark box in the place of the step rows while it is open.
        gui.py writes its lines to self.log, which is this box from here on (its own box sits in the frame that is
        never shown)."""
        c = self.stage
        self.log_box = tk.Frame(c, background=INSET, highlightbackground=ROW_EDGE, highlightthickness=1)
        self.log = tk.Text(self.log_box, wrap='word', state='disabled', font=(self.mono_font, 9), borderwidth=0, padx=10,
                           pady=8, highlightthickness=0)
        scroll = ttk.Scrollbar(self.log_box, command=self.log.yview)
        self.log.configure(yscrollcommand=scroll.set)
        scroll.pack(side='right', fill='y')
        self.log.pack(side='left', fill='both', expand=True)
        self.log_item = c.create_window(34, 124, window=self.log_box, anchor='nw', width=STAGE_W - 68, height=7 * 54 - 8,
                                        state='hidden')

    def toggle_log(self):
        self.log_open = not self.log_open
        self.bound = [b for b in self.bound if b[0] is not self.log_toggle]
        self.bind_text(self.log_toggle, 'text', 'ui.hide_log' if self.log_open else 'ui.show_log')
        if self.log_open:
            self.log.see('end')
        self.refresh_run()

    # ---- background work -------------------------------------------------------------------------------------------

    def background(self, work, then):
        """Run work() in a thread and then(its value, or the exception it raised) on the window's thread."""
        def run():
            try:
                value = work()
            except Exception as e:
                value = e
            self.tasks.put(lambda: then(value))
        threading.Thread(target=run, daemon=True).start()

    def pump(self):
        try:
            while True:
                self.tasks.get_nowait()()
        except queue.Empty:
            pass
        self.root.after(POLL_MS * 2, self.pump)

    # ---- the graphics card -----------------------------------------------------------------------------------------

    def recorded_gpu(self):
        """The card the last setup run on the game folder recorded: shown at once, until the detection answers."""
        state = self.form().installed_state()
        record = (state or {}).get('steps', {}).get('oot_components') or {}
        if isinstance(record.get('gpu'), dict):
            gpu = components.Gpu.from_json(record['gpu'])
            if gpu:
                self.gpu, self.gpu_known = gpu, True

    def got_gpu(self, value):
        self.gpu, self.gpu_known = (None if isinstance(value, Exception) else value), True
        self.refresh_home()
        self.refresh_form()

    def hardware_text(self):
        """'Detected: <card>, <N> GB -> preset <name>', '' while the card is not known yet."""
        if not self.gpu_known:
            return ''
        dest = self.game_exe().parent
        have = components.usable(dest) if self.game_exe().is_file() else set(components.GROUPS)
        chosen = components.preset(self.gpu, have)
        name = self.cat('preset.' + chosen['id'])
        if chosen['tex'] != 'auto':
            name += ', ' + self.cat('preset.tex_' + chosen['tex'])
        if not self.gpu:
            return self.cat('ui.oot_hw_unknown', preset=name)
        return self.cat('ui.oot_hw_line', gpu=components.describe(self.gpu), preset=name)

    def hardware_facts(self, lacking):
        """The values of the home sheet's table (FACTS): the card, the first settings chosen for it, the libraries
        that are in the game folder. Dots for the first two while the card is not known yet."""
        dest, built = self.game_exe().parent, self.game_exe().is_file()
        have = components.usable(dest) if built else set()
        if self.forced_missing is not None:   # the self test's stand-in for the files of a game folder
            have = set(components.groups_for(self.gpu)) - set(lacking)
        libraries = ' \u00b7 '.join(self.cat('comp.' + g) for g in components.GROUPS if g in have) or '\u2013'
        if not self.gpu_known:
            return '\u2026', '\u2026', libraries
        chosen = components.preset(self.gpu, have if built or self.forced_missing is not None else set(components.GROUPS))
        name = self.cat('preset.' + chosen['id'])
        if chosen['tex'] != 'auto':
            name += ', ' + self.cat('preset.tex_' + chosen['tex'])
        return (components.describe(self.gpu) if self.gpu else self.cat('ui.oot_fact_unknown')), name, libraries

    def missing_groups(self):
        """The component groups this card can use that the game folder lacks a file of."""
        if self.forced_missing is not None:
            return self.forced_missing
        if not self.gpu_known or not self.game_exe().is_file():
            return []
        lacking = set(components.missing(self.game_exe().parent, self.gpu))
        return [g for g in components.groups_for(self.gpu)
                if any(f.name in lacking for f in components.FILES if f.group == g)]

    # ---- home -----------------------------------------------------------------------------------------------------

    def game_exe(self):
        return Path(self.vars['dest'].get().strip() or '.') / oot.GAME_EXE

    def build_home(self):
        """The home screen, drawn on the stage: the plates at the left (Play, lit by the game's cursor, and what else
        there is to do), the parchment sheet at the right with what there is to read. Returns a stand-in frame: the
        window keeps a frame per screen."""
        c, tags = self.stage, ('scr:home',)
        self.home_play = Plate(self, 'play76', 266, self.start_game, 'RekPlay', tags)
        self.home_play.place(34, 100)
        self.bind_text(self.home_play, 'text', 'ui.oot_play')
        y = 202
        for id, command in (('ui.oot_home_mods', self.open_mods), ('ui.oot_home_saves', self.open_saves),
                            ('ui.oot_home_settings', self.open_settings),
                            ('ui.open_folder', self.open_game_folder), ('ui.oot_home_update', lambda: self.show('form')),
                            ('ui.oot_home_report', self.make_report)):
            p = Plate(self, 'plate46', 266, command, 'RekPlate', tags)
            p.place(34, y)
            self.bind_text(p, 'text', id)
            y += 58
        c.create_image(338 - 24, 100 - 24, image=self.art['sheet'], anchor='nw', tags=tags)
        c.create_image(372, 126, image=self.art['logo-ink'], anchor='nw', tags=tags)
        line = lambda fill, font, extra=(), width=SHEET_TEXT: StageText(   # noqa: E731
            c, c.create_text(SHEET_X, 0, anchor='nw', width=width, fill=fill, font=font, tags=tags + extra))
        self.home_text = line(INK, 'RekBody')
        self.home_rules = [c.create_line(0, 0, 0, 0, fill=SHEET_LINE, tags=tags) for _ in range(3)]
        # this machine, as a small table: the card, the first settings chosen for it, the libraries in the game folder
        self.facts = [(line(INK_DIM, 'RekText'), line(INK, 'RekText')) for _ in FACTS]
        for (name, _value), id in zip(self.facts, FACTS):
            self.bind_text(name, 'text', id)
        # the libraries that could not be downloaded
        self.comp_label = line(INK_WARN, 'RekText', ('home:comp',), SHEET_TEXT - 190)
        self.comp_retry = Plate(self, 'plate40', 150, self.retry_components, 'RekStrong', tags + ('home:comp',))
        self.bind_text(self.comp_retry, 'text', 'ui.oot_components_retry')
        # the shaders: a first install prepares the variants play met; the launcher prepares the rest in the background
        self.shaders_name = line(INK_DIM, 'RekText', ('home:shaders',), 90)
        self.shaders_status = line(INK, 'RekText', ('home:shaders',), SHEET_TEXT - 100)
        self.shaders_detail = line(INK, 'RekText', ('home:shaders',), SHEET_TEXT - 100)
        self.shaders_note = line(INK_DIM, 'RekSmall', ('home:shaders',), SHEET_TEXT - 100)
        self.shaders_bar = c.create_rectangle(0, 0, 0, 0, fill='#1c130a', outline=SHEET_LINE, tags=tags + ('home:shaders',))
        self.shaders_fill = c.create_rectangle(0, 0, 0, 0, fill=AMBER, outline='', tags=tags + ('home:shaders',))
        self.shaders_pause = Plate(self, 'plate40', 110, self.pause_shaders, 'RekStrong', tags + ('home:shaders',))
        self.bind_text(self.shaders_name, 'text', 'ui.oot_shaders_name')
        self.shaders_view = None
        # the launcher's own version: up to date, or the newer one with its buttons and what is new in it
        self.update_label = line(INK_DIM, 'RekText')
        make = lambda kind, id, command: (lambda p: (self.bind_text(p, 'text', id), p)[1])(   # noqa: E731
            Plate(self, kind, 150, command, 'RekStrong', tags + ('home:update',)))
        self.update_now = make('go40', 'ui.oot_update_now', self.start_update)
        self.update_restart = make('go40', 'ui.oot_update_restart', self.restart_for_update)
        self.update_page = make('plate40', 'ui.oot_update_page', self.open_release_page)
        self.update_check = make('plate40', 'ui.oot_update_check', lambda: self.check_updates(force=True))
        self.update_shown, self.comp_shown, self.notes_lines = [self.update_check], False, []
        self.notes_head = line(INK, 'RekHead', ('home:notes',))
        self.bind_text(self.notes_head, 'text', 'ui.oot_update_notes')
        self.notes_body = line(INK, 'RekText', ('home:notes',))
        return ttk.Frame(self.body)

    def layout_home(self):
        """The sheet's lines from the top down, each under the one before, as tall as its text came out; the pieces
        that are not there now are hidden. Only while the home screen is shown: showing a screen shows all its items."""
        if getattr(self, 'screen', None) != 'home':
            return
        c, x = self.stage, SHEET_X
        y = 190

        def below(item, gap):
            nonlocal y
            c.coords(item, x, y)
            box = c.bbox(item)
            y = (box[3] if box else y) + gap

        def rule(item):
            nonlocal y
            c.coords(item, x, y, x + SHEET_TEXT, y)
            y += 14

        below(self.home_text.item, 14)
        rule(self.home_rules[0])
        column = max(c.bbox(name.item)[2] - c.bbox(name.item)[0] for name, _value in self.facts) + 24
        for name, value in self.facts:
            c.coords(name.item, x, y)
            c.coords(value.item, x + column, y)
            c.itemconfigure(value.item, width=SHEET_TEXT - column)
            y = max(c.bbox(name.item)[3], c.bbox(value.item)[3]) + 7
        c.itemconfigure('home:comp', state='normal' if self.comp_shown else 'hidden')
        if self.comp_shown:
            y += 5
            self.comp_retry.fit()
            self.comp_retry.place(x + SHEET_TEXT - self.comp_retry.width, y)
            c.itemconfigure(self.comp_label.item, width=SHEET_TEXT - self.comp_retry.width - 24)
            box = c.bbox(self.comp_label.item)
            tall = box[3] - box[1] if box else 0
            c.coords(self.comp_label.item, x, y + max(0, (40 - tall) // 2))   # beside the plate's middle
            y += max(tall, 40) + 7
        y += 7
        rule(self.home_rules[1])
        below(self.update_label.item, 12)
        px = x
        for p in (self.update_now, self.update_restart, self.update_page, self.update_check):
            p.show(p in self.update_shown)
            if p in self.update_shown:
                p.fit()
                if px + p.width > x + SHEET_TEXT:   # the row is full: the rest goes under it
                    px, y = x, y + 48
                p.place(px, y)
                px += p.width + 10
        y += 40 + 8
        view = self.shader_view()
        self.shaders_view = view
        c.itemconfigure('home:shaders', state='normal' if view else 'hidden')
        c.itemconfigure(self.home_rules[2], state='normal' if view else 'hidden')
        if view:
            rule(self.home_rules[2])
            col = x + 100
            c.coords(self.shaders_name.item, x, y)
            c.coords(self.shaders_status.item, col, y)
            c.itemconfigure(self.shaders_status.item, text=view['status'])
            y = c.bbox(self.shaders_status.item)[3] + 8
            if view['detail']:
                c.coords(self.shaders_detail.item, col, y)
                c.itemconfigure(self.shaders_detail.item, text=view['detail'], state='normal')
                y = c.bbox(self.shaders_detail.item)[3] + 8
            else:
                c.itemconfigure(self.shaders_detail.item, state='hidden')
            bar = view['fraction'] is not None
            for item in (self.shaders_bar, self.shaders_fill):
                c.itemconfigure(item, state='normal' if bar else 'hidden')
            self.shaders_pause.show(view['button'] is not None)
            if bar:
                width = SHEET_TEXT - 100 - 130
                c.coords(self.shaders_bar, col, y, col + width, y + 32)
                c.coords(self.shaders_fill, col + 3, y + 3, col + 3 + round((width - 6) * view['fraction']), y + 29)
            if view['button'] is not None:
                self.shaders_pause.configure(text=self.cat(view['button']))
                self.shaders_pause.place(x + SHEET_TEXT - self.shaders_pause.width, y - 4)
            if bar or view['button'] is not None:
                y += 40
            if view['note']:
                c.itemconfigure(self.shaders_note.item, text=view['note'], state='normal')
                c.coords(self.shaders_note.item, col, y)
                y = c.bbox(self.shaders_note.item)[3] + 8
            else:
                c.itemconfigure(self.shaders_note.item, state='hidden')
        y += 14
        # what is new in the release that is out, as much of it as the sheet has room for
        shown = bool(self.notes_lines) and SHEET_BOTTOM - y > 90
        c.itemconfigure('home:notes', state='normal' if shown else 'hidden')
        if shown:
            below(self.notes_head.item, 8)
            lines, item = list(self.notes_lines), self.notes_body.item
            c.coords(item, x, y)
            c.itemconfigure(item, text='\n'.join(lines))
            while len(lines) > 1 and c.bbox(item)[3] > SHEET_BOTTOM - 2:
                lines.pop()
                c.itemconfigure(item, text='\n'.join(lines).rstrip() + '\n\u2026')
        self.place_cursor()

    def refresh_home(self):
        ready = self.game_exe().is_file()
        dest = self.vars['dest'].get().strip()
        text = self.cat('ui.oot_home_text' if ready else 'ui.oot_home_missing', dest=dest)
        state = self.form().installed_state() if ready else None
        if state and state.get('release') and state['release'] != self.layout.release:
            text = self.cat('ui.oot_built_by_older', dest=dest, built=state['release'], version=self.layout.release)
        if ready and self.last_run_failed():   # the game stopped with an error last time: say what to send
            text += '\n\n' + self.cat('ui.oot_report_after_crash')
        self.home_text.configure(text=text)
        self.home_play.state(['!disabled' if ready else 'disabled'])
        lacking = self.missing_groups()
        for (_name, value), text in zip(self.facts, self.hardware_facts(lacking) + (self.mods_fact(),)):
            value.configure(text=text)
        self.comp_shown = bool(lacking)
        if lacking:
            self.comp_label.configure(text=self.cat('ui.oot_components_missing',
                                                    names=', '.join(self.cat('comp.' + g) for g in lacking)))
        self.refresh_update()

    # ---- the shaders' background preparation ----------------------------------------------------------------------
    # The setup's first run prepares the variants play met. The rest are prepared here, by themselves (shaderjob.py): a
    # child at low priority while the window is open and no game runs. Play stops it, starts the game, hides the window
    # and takes the job up again when the game has ended. Closing the window stops it; the next start goes on.

    shader_job = None
    shader_hidden = False       # the window is hidden while the game runs, for the job's sake
    shader_finished = None      # 'done' or 'failed' once the job ended in this window's life: the row says so
    shader_error = ''

    def shader_wanted(self):
        """True when the game folder prepared the variants play met only, and this window may still do the rest."""
        return (self.shader_finished is None and self.game_exe().is_file() and
                oot.shaders_only_met(self.form().installed_state()) and not self.form().problems())

    def shader_command(self):
        python = Path(sys.executable) if self.layout.dev else self.layout.python
        form = self.form()
        form.all_shaders = True
        return form.command(python, root=self.layout.root if self.layout.dev else None, temp=self.run_args.get('temp'))

    def shader_tick(self):
        """Once a second: take in the job's events, stop the job when a game runs, start it when none does, and show the
        window again when the game is over and there is nothing left to wait for."""
        try:
            self.shader_step()
        except Exception:   # a tick that fails must not end the ticks
            pass
        finally:
            self.root.after(1000, self.shader_tick)

    def shader_step(self):
        job, changed = self.shader_job, False
        if job is not None:
            changed = job.poll()
            if job.state in ('done', 'failed') and self.shader_finished is None:
                self.shader_finished, self.shader_error = job.state, job.error or ''
                changed = True
        if self.shader_finished is None and (job is not None and job.active or self.shader_hidden or
                                             (self.screen in ('home', 'mods', 'saves') and not self.process and not self.job)):
            game = shaderjob.games_running(oot.GAME_EXE, job.pid if job is not None and job.active else None)
            if game:
                if job is not None and job.active:
                    job.stop()
                    changed = True
            elif self.shader_wanted():
                if job is None:
                    job = self.shader_job = shaderjob.ShaderJob(self.shader_command(), self.layout.root)
                if job.start():
                    changed = True
        if self.shader_hidden and not (self.shader_job is not None and self.shader_job.active) and \
                not shaderjob.games_running(oot.GAME_EXE):
            self.shader_show_window()   # the job ended (or cannot go on) and no game runs: nothing left to wait for
            changed = True
        if changed and self.screen == 'home':
            self.layout_home()

    def shader_show_window(self):
        self.shader_hidden = False
        self.root.deiconify()
        self.root.lift()

    def shader_stop(self):
        if self.shader_job is not None:
            self.shader_job.stop()

    def pause_shaders(self):
        job = self.shader_job
        if job is None:
            return
        if job.paused:
            job.resume()
        else:
            job.pause()
        self.layout_home()

    def shader_view(self):
        """What the home sheet's shader row says: {'status', 'detail', 'fraction' (None: no bar), 'button' (text id or
        None), 'note'}, or None for no row (nothing to prepare, nothing to say)."""
        if self.forced_shaders is not None:
            return self.forced_shaders or None
        job = self.shader_job
        if self.shader_finished == 'done':
            return {'status': self.cat('ui.oot_shaders_done'), 'detail': '', 'fraction': None, 'button': None, 'note': ''}
        if self.shader_finished == 'failed':
            return {'status': self.cat('ui.oot_shaders_ready'), 'fraction': None, 'button': None, 'note': '',
                    'detail': self.cat('ui.oot_shaders_failed', error=self.shader_error or '?')}
        if job is None or not self.shader_wanted():
            return None
        status, note, button, fraction = self.cat('ui.oot_shaders_ready'), '', None, None
        if job.paused:
            detail, button = self.cat('ui.oot_shaders_paused'), 'ui.oot_shaders_resume'
        elif job.active:
            left = job.remaining()
            minutes = max(1, round(left / 60)) if left is not None else '?'
            if job.done and job.total and job.phase in ('translate', 'libraries'):
                detail = self.cat('ui.oot_shaders_running', done=f'{job.done:,}'.replace(',', ' '),
                                  total=f'{job.total:,}'.replace(',', ' '), minutes=minutes)
            else:
                detail = self.cat('ui.oot_shaders_starting')
            fraction, button, note = job.fraction, 'ui.oot_shaders_pause', self.cat('ui.oot_shaders_note')
        else:
            detail, note = self.cat('ui.oot_shaders_waiting'), self.cat('ui.oot_shaders_note')
        return {'status': status, 'detail': detail, 'fraction': fraction, 'button': button, 'note': note}

    def open_game_folder(self):
        dest = self.vars['dest'].get().strip()
        if dest and Path(dest).is_dir() and os.name == 'nt':
            os.startfile(dest)

    def last_run_failed(self):
        """True when the game's last run ended with an error: its log (logs/rekarina.log beside the game, the newest run)
        ends with a crash report, a fatal error or a lost graphics device (runtime/report.c reads the same marks)."""
        log = self.game_exe().parent / 'logs' / 'rekarina.log'
        try:
            with open(log, 'rb') as f:
                f.seek(0, os.SEEK_END)
                f.seek(max(0, f.tell() - 256 * 1024))
                tail = f.read()
        except OSError:
            return False
        return b'FATAL' in tail or b'vk: device lost' in tail

    def make_report(self):
        """The game makes the problem report (rekarina --make-report: one zip in reports/ beside it, nothing sent);
        the launcher then shows the folder. The game runs without a window for it, a few seconds."""
        exe = self.game_exe()
        if not exe.is_file():
            return

        def work():
            env = dict(os.environ, REKARINA_REPORT_NO_OPEN='1')
            flags = subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0
            r = subprocess.run([str(exe), '--make-report'], cwd=str(exe.parent), env=env, capture_output=True,
                               timeout=120, creationflags=flags)
            path = r.stdout.decode('utf-8', 'replace').strip().splitlines()
            if r.returncode or not path or not Path(path[-1]).is_file():
                raise RuntimeError(r.stderr.decode('utf-8', 'replace').strip() or f'exit {r.returncode}')
            return Path(path[-1])

        def then(value):
            if isinstance(value, Exception):
                messagebox.showerror(self.cat('ui.oot_title'), self.cat('ui.oot_report_failed', detail=str(value)[:300]))
                return
            folder = value.parent
            if os.name == 'nt':
                os.startfile(str(folder))
            else:
                subprocess.Popen(['xdg-open', str(folder)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                 start_new_session=True)
        self.background(work, then)

    def open_settings(self):
        """rekarina.ini in the player's text editor: every setting has its explanation there. (totk.ini in a folder
        the game has not started in since it was renamed.)"""
        ini_file = self.game_exe().parent / oot.INI
        if not ini_file.is_file():
            ini_file = self.game_exe().parent / oot.OLD_INI
        if ini_file.is_file() and os.name == 'nt':
            os.startfile(str(ini_file))

    # ---- the launcher's update -------------------------------------------------------------------------------------

    # ---- mods -----------------------------------------------------------------------------------------------------
    # The Mods page, drawn on the stage: a row per mod of the game folder's mods folder (mods.py reads it as the game
    # does), the mark and the name on the strip, in the dark cell whether it is on and what it holds. The game's cursor
    # lies on the chosen row's cell: Up and Down move it, Enter, the space bar or a click on the cell switches the mod.
    # The plates under the list add a mod (a .zip or a folder), open the mods folder, move the chosen mod (the higher
    # mod wins a file two mods change), take it out (its folder goes to mods-removed) and, when a mod's code patch is
    # not in the build yet, rebuild the game.

    def build_mods_stage(self):
        c, tags = self.stage, ('scr:mods',)
        title = StageText(c, c.create_text(34, 78, anchor='nw', fill=CREAM, font='RekTitle', tags=tags))
        self.bind_text(title, 'text', 'ui.mods_title')
        self.mods_path = c.create_text(STAGE_W - 34, 100, anchor='e', fill=DIM, font='RekSmall', tags=tags)
        self.mods_empty = StageText(c, c.create_text(34, MODS_TOP + 6, anchor='nw', fill=SOFT, font='RekBody',
                                                     width=STAGE_W - 68, tags=tags))
        self.bind_text(self.mods_empty, 'text', 'ui.mods_empty')
        self.mod_rows = []
        for i in range(MODS_ROWS):
            y, row_tags = MODS_TOP + i * STEP_PITCH, tags + (f'mods:row{i}',)
            row = c.create_image(34, y, image=self.art['row'], anchor='nw', tags=row_tags)
            mark = c.create_text(34 + 28, y + 23, fill=CREAM, font='RekMark', tags=row_tags)
            shade = c.create_text(34 + 55, y + 24, anchor='w', fill=PLATE_SHADE, font='RekBody', tags=row_tags)
            name = c.create_text(34 + 54, y + 23, anchor='w', fill=CREAM, font='RekBody', tags=row_tags)
            note = c.create_text(0, y + 24, anchor='w', fill=NOTE, font='RekSmall', tags=row_tags)
            value = c.create_text(34 + 722 + 164, y + 23, fill=DIM, font='RekText', tags=row_tags)
            self.mod_rows.append((row, mark, shade, name, note, value))
            c.tag_bind(f'mods:row{i}', '<Button-1>', lambda e, i=i: self.click_mod(i, e.x >= 34 + 722))
            c.tag_bind(f'mods:row{i}', '<Double-Button-1>', lambda _e, i=i: self.click_mod(i, True))
            c.tag_bind(f'mods:row{i}', '<Enter>', lambda _e: c.configure(cursor='hand2'))
            c.tag_bind(f'mods:row{i}', '<Leave>', lambda _e: c.configure(cursor=''))
        # 'disabled': drawn, but the clicks go to the row under it (place_cursor)
        self.mods_cursor = c.create_image(0, 0, image=self.art['cursor-cell'], anchor='nw', state='disabled', tags=tags)
        self.mods_total = c.create_image(34, 0, image=self.art['total'], anchor='nw', tags=tags)
        self.mods_count = c.create_text(0, 0, anchor='w', fill=CREAM, font='RekBody', tags=tags)
        self.mods_hint = c.create_text(0, 0, anchor='e', fill=DIM, font='RekText', tags=tags)
        self.mods_line = c.create_text(34, 0, anchor='nw', fill=DIM, font='RekSmall', width=STAGE_W - 68, tags=tags)
        make = lambda kind, id, command: (lambda p: (self.bind_text(p, 'text', id), p)[1])(   # noqa: E731
            Plate(self, kind, 120, command, 'RekStrong', tags))
        self.mods_add_zip = make('plate40', 'ui.mods_add_zip', lambda: self.add_mod(False))
        self.mods_add_dir = make('plate40', 'ui.mods_add_folder', lambda: self.add_mod(True))
        self.mods_open = make('plate40', 'ui.open_folder', self.open_mods_folder)
        self.mods_up = make('plate40', 'ui.mods_up', lambda: self.move_mod(-1))
        self.mods_down = make('plate40', 'ui.mods_down', lambda: self.move_mod(1))
        self.mods_remove = make('plate40', 'ui.mods_remove', self.remove_mod)
        self.mods_rebuild = make('go40', 'ui.mods_rebuild', self.rebuild_mods)
        self.mods_back = make('plate40', 'ui.back', self.go_home)
        for key, step in (('<Control-Up>', -1), ('<Control-Down>', 1)):
            c.bind(key, lambda _e, step=step: self.screen == 'mods' and self.move_mod(step))
        c.bind('<MouseWheel>', lambda e: self.screen == 'mods' and self.step_mod(-1 if e.delta > 0 else 1))
        self.root.bind('<FocusIn>', self.mods_focus, add='+')
        c.itemconfigure('scr:mods', state='hidden')

    def mods_dest(self):
        return self.game_exe().parent

    def scan_mods(self):
        if self.forced_mods is not None:
            return list(self.forced_mods)
        if not self.vars['dest'].get().strip():
            return []
        try:
            return mods.scan(self.mods_dest())
        except OSError:
            return []

    def mods_fact(self):
        """The home sheet's line about the mods: how many are on, and how many wait for a rebuild."""
        total, on, waiting = mods.summary(self.scan_mods())
        if not total:
            return self.cat('ui.mods_fact_none')
        text = self.cat('ui.mods_fact', on=on, count=total)
        return text + ' \u00b7 ' + self.cat('ui.mods_fact_waits', count=waiting) if waiting else text

    def clip(self, text, font, room):
        """text, or as much of it as `room` pixels hold in that font, with three dots."""
        f = self.fonts[font]
        if f.measure(text) <= room:
            return text
        while text and f.measure(text + '\u2026') > room:
            text = text[:-1]
        return text.rstrip() + '\u2026'

    def chosen_mod(self):
        return next((m for m in self.mod_list if m.name == self.mod_sel), None)

    def mod_value(self, m):
        """A row's cell: on, off or waiting, then what the mod holds."""
        state = 'ui.mods_off' if m.problem or not m.enabled else 'ui.mods_waits' if m.waits else 'ui.mods_on'
        if m.problem in (mods.EMPTY, mods.PLUGIN):
            holds = [self.cat('ui.mods_kind_' + m.problem)]
        else:
            count = {mods.FILES: m.files, mods.PATCH: m.patches, mods.CHEATS: m.cheats}
            holds = [self.cat('ui.mods_kind_' + k, count=count[k]) for k in m.kinds]
        room, parts = self.art['cursor-cell'].width() - 2 * CURSOR_MARGIN - 24, [self.cat(state)] + holds
        while len(parts) > 2 and self.fonts['RekText'].measure(' \u00b7 '.join(parts)) > room:
            parts.pop()   # what does not fit is left out, the last kind first
        return self.clip(' \u00b7 '.join(parts), 'RekText', room)

    def refresh_mods(self, rescan=True):
        """The page from the mods folder as it is now. Only while it is shown: showing a screen shows all its items."""
        if getattr(self, 'screen', None) != 'mods':
            return
        c = self.stage
        if rescan:
            self.mod_list = self.scan_mods()
        found = self.mod_list
        names = [m.name for m in found]
        if self.mod_sel not in names:   # the chosen mod is gone: the one that took its place, or the first
            self.mod_sel = names[min(self.mod_at, len(names) - 1)] if names else None
        at = self.mod_at = names.index(self.mod_sel) if names else 0
        self.mod_first = max(0, min(self.mod_first, len(found) - MODS_ROWS, at), at - MODS_ROWS + 1)
        c.itemconfigure(self.mods_path, text=self.clip(str(mods.mods_dir(self.mods_dest())), 'RekSmall', 760))
        c.itemconfigure(self.mods_empty.item, state='hidden' if found else 'normal')
        for i, (row, mark, shade, name, note, value) in enumerate(self.mod_rows):
            m = found[self.mod_first + i] if self.mod_first + i < len(found) else None
            c.itemconfigure(f'mods:row{i}', state='normal' if m else 'hidden')
            if not m:
                continue
            live = m.enabled and not m.problem
            waits = live and m.waits
            hint = (self.cat('ui.mods_note_waits') if waits else self.cat('ui.mods_problem_' + m.problem) if m.problem else '')
            room = 722 - 54 - 16 - (self.fonts['RekSmall'].measure(hint) + 16 if hint else 0)
            text = self.clip(m.name, 'RekBody', room)
            c.itemconfigure(row, image=self.art['row' if live else 'row-dim'])
            c.itemconfigure(mark, text=MODS_MARK['no' if m.problem else 'on' if m.enabled else 'off'],
                            fill=DONE_TEXT if live else DIM)
            c.itemconfigure(name, text=text, fill=CREAM if live else DIM)
            c.itemconfigure(shade, text=text if live else '')
            c.itemconfigure(note, text=hint, fill=NOTE if waits else DIM)
            c.coords(note, 34 + 54 + self.fonts['RekBody'].measure(text) + 16, MODS_TOP + i * STEP_PITCH + 24)
            c.itemconfigure(value, text=self.mod_value(m), fill=NOTE if waits else CREAM if live else DIM)
        c.itemconfigure(self.mods_cursor, state='disabled' if found else 'hidden')
        c.coords(self.mods_cursor, 34 + 722 - CURSOR_MARGIN, MODS_TOP + (at - self.mod_first) * STEP_PITCH - CURSOR_MARGIN)
        # the total line, the line under it, the plates
        total, on, waiting = mods.summary(found)
        y = MODS_TOP + max(1, min(len(found), MODS_ROWS)) * STEP_PITCH + 6
        count = self.cat('ui.mods_total', count=total, on=on)
        if waiting:
            count += '  \u00b7  ' + self.cat('ui.mods_fact_waits', count=waiting)
        hint = self.cat('ui.mods_order_hint') if len(found) > 1 else ''
        if len(found) > MODS_ROWS:
            hint = self.cat('ui.mods_window', first=self.mod_first + 1, last=self.mod_first + MODS_ROWS, count=len(found))
        c.coords(self.mods_total, 34, y)
        for item in (self.mods_total, self.mods_count, self.mods_hint):   # nothing to count on an empty page
            c.itemconfigure(item, state='normal' if found else 'hidden')
        c.itemconfigure(self.mods_count, text=count)
        c.coords(self.mods_count, 34 + 18, y + 22)
        c.itemconfigure(self.mods_hint, text=hint)
        c.coords(self.mods_hint, STAGE_W - 34 - 18, y + 22)
        if hint and c.bbox(self.mods_hint)[0] < c.bbox(self.mods_count)[2] + 24:   # no room for both: the count stays
            c.itemconfigure(self.mods_hint, text='')
        chosen, idle = self.chosen_mod(), not self.mod_busy
        # a folder the setup did not build has no rebuild: nothing here offers one
        can_build = self.forced_mods is not None or self.form().installed_state() is not None
        fill, line = self.mod_note or (DIM, self.cat('ui.mods_hint_rebuild') if waiting and can_build else '')
        c.itemconfigure(self.mods_line, text=line, fill=fill)
        c.coords(self.mods_line, 34, y + 44 + 12)
        self.mods_rebuild.show(bool(waiting) and can_build)
        for p, on in ((self.mods_add_zip, idle), (self.mods_add_dir, idle), (self.mods_up, idle and chosen and at > 0),
                      (self.mods_down, idle and chosen and at < len(found) - 1), (self.mods_remove, idle and chosen),
                      (self.mods_rebuild, idle)):
            p.state(['!disabled' if on else 'disabled'])
        self.place_mod_plates()
        self.place_cursor()

    def place_mod_plates(self):
        """The plates in one row above the window's lower edge: what changes the list from the left, Back at the
        right end with Rebuild beside it. With less padding where a language's words are long."""
        left = [self.mods_add_zip, self.mods_add_dir, self.mods_open, self.mods_up, self.mods_down, self.mods_remove]
        right = [p for p in (self.mods_rebuild, self.mods_back) if p.shown]
        for pad, gap in ((22, 10), (14, 8), (9, 6)):
            for p in left + right:
                p.fit(pad=pad, least=64)
            if sum(p.width + gap for p in left + right) - gap + 16 <= STAGE_W - 68:
                break
        x, y = 34, STAGE_H - 58
        for p in left:
            p.place(x, y)
            x += p.width + gap
        x = STAGE_W - 34
        for p in reversed(right):
            x -= p.width
            p.place(x, y)
            x -= gap

    def open_mods(self):
        self.mod_note, self.mod_sel, self.mod_first, self.mod_at = None, None, 0, 0
        self.show('mods')

    def mods_focus(self, event):
        """The window came to the front again: the mods folder may have changed meanwhile (Open folder)."""
        if event.widget is self.root and self.screen == 'mods' and not self.mod_busy:
            self.refresh_mods()

    def step_mod(self, step):
        """The list's cursor one row on or back; the plates' cursor steps aside."""
        self.cursor_keys = False
        self.set_cursor(None)
        if self.mod_list:
            self.mod_sel = self.mod_list[max(0, min(len(self.mod_list) - 1, self.mod_at + step))].name
            self.refresh_mods(rescan=False)
        return 'break'

    def click_mod(self, row, switch):
        """A click on a row of the list chooses its mod; on the row's cell it also switches it."""
        if self.mod_first + row < len(self.mod_list):
            self.stage.focus_set()
            self.cursor_keys = False
            self.set_cursor(None)
            self.mod_sel = self.mod_list[self.mod_first + row].name
            if switch:
                self.toggle_mod()
            else:
                self.refresh_mods(rescan=False)

    def mod_action(self, work):
        """One change of the mods folder; under the list the line it returns (colour, text), or what went wrong."""
        self.mod_note = None
        try:
            note = work()
            self.mod_note = note if isinstance(note, tuple) else None
        except mods.ModError as e:
            self.mod_note = (RED, self.cat(e.id, **e.params))
        except OSError as e:
            self.mod_note = (RED, self.cat('mods.failed', detail=str(e)))
        self.refresh_mods()

    def toggle_mod(self):
        m = self.chosen_mod()
        if not m or self.mod_busy or self.forced_mods is not None:
            return
        if m.problem:   # it cannot be switched on: the line under the list says why
            self.mod_note = (NOTE, self.cat('ui.mods_cannot_' + m.problem, name=m.name))
            return self.refresh_mods(rescan=False)
        self.mod_action(lambda: mods.set_enabled(self.mods_dest(), m.name, not m.enabled))

    def move_mod(self, step):
        m = self.chosen_mod()
        if m and not self.mod_busy and self.forced_mods is None:
            self.mod_action(lambda: mods.move(self.mods_dest(), m.name, step))
        return 'break'

    def remove_mod(self):
        m = self.chosen_mod()
        if not m or self.mod_busy or self.forced_mods is not None:
            return 'break'

        self.mod_action(lambda: (SOFT, self.cat('ui.mods_removed', name=m.name,
                                                path=str(mods.remove(self.mods_dest(), m.name)))))
        return 'break'

    def add_mod(self, folder):
        """A mod from a .zip or from a folder, copied into the mods folder off the window's thread: a large mod
        takes a while."""
        if self.mod_busy:
            return
        if folder:
            path = filedialog.askdirectory(parent=self.root, title=self.cat('ui.mods_pick_folder'), mustexist=True)
        else:
            path = filedialog.askopenfilename(parent=self.root, title=self.cat('ui.mods_pick_zip'),
                                              filetypes=[(self.cat('ui.mods_zip_files'), '*.zip')])
        if not path:
            return
        dest, source = self.mods_dest(), Path(os.path.normpath(path))
        self.mod_busy, self.mod_note = True, (SOFT, self.cat('ui.mods_adding', name=source.name))
        self.refresh_mods(rescan=False)
        self.background(lambda: mods.add(dest, source), self.added_mod)

    def added_mod(self, value):
        self.mod_busy = False
        if isinstance(value, mods.ModError):
            self.mod_note = (RED, self.cat(value.id, **value.params))
        elif isinstance(value, Exception):
            self.mod_note = (RED, self.cat('mods.failed', detail=str(value)))
        else:
            self.mod_sel, self.mod_note = value.name, (SOFT, self.cat('ui.mods_added', name=value.name))
        self.refresh_mods()

    def open_mods_folder(self):
        folder = mods.mods_dir(self.mods_dest())
        try:
            folder.mkdir(parents=True, exist_ok=True)
        except OSError:
            return
        if os.name == 'nt':
            os.startfile(str(folder))
        else:
            subprocess.Popen(['xdg-open', str(folder)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                             start_new_session=True)

    def rebuild_mods(self):
        """The setup on this game folder, for the mods' code patches the build lacks: the translate, compile and
        package steps run, the prepared shaders, the saves and the settings stay (installer/oot.py)."""
        form = self.form()
        if self.forced_mods is not None or form.installed_state() is None:
            return
        if form.problems():   # the form says what it needs
            self.show('form')
        else:
            self.pending_title = ('ui.oot_rebuild_mods', {})
        self.start()

    # ---- saves ----------------------------------------------------------------------------------------------------
    # The Saves page: a row per save found on this machine (saves.py: the other ports' folders, the emulators'), where
    # it is from on the strip, its slots and the time it was last written in the dark cell. Up and Down move the
    # game's cursor over the list; Enter, a double click or Import copies the chosen save into the game folder. A save
    # from anywhere else comes in through Choose folder or Choose .zip. The save the game folder held is kept
    # (profile/save-before-import), and Restore previous brings the newest of those back. Replacing a save asks once:
    # Import turns into Replace and the line under the list says what happens.

    def build_saves_stage(self):
        c, tags = self.stage, ('scr:saves',)
        title = StageText(c, c.create_text(34, 78, anchor='nw', fill=CREAM, font='RekTitle', tags=tags))
        self.bind_text(title, 'text', 'ui.saves_title')
        self.saves_path = c.create_text(STAGE_W - 34, 100, anchor='e', fill=DIM, font='RekSmall', tags=tags)
        self.saves_empty = StageText(c, c.create_text(34, MODS_TOP + 6, anchor='nw', fill=SOFT, font='RekBody',
                                                      width=STAGE_W - 68, tags=tags))
        self.bind_text(self.saves_empty, 'text', 'ui.saves_empty')
        self.save_rows = []
        for i in range(SAVES_ROWS):
            y, row_tags = MODS_TOP + i * STEP_PITCH, tags + (f'saves:row{i}',)
            row = c.create_image(34, y, image=self.art['row'], anchor='nw', tags=row_tags)
            mark = c.create_text(34 + 28, y + 23, fill=DONE_TEXT, font='RekMark', tags=row_tags)
            shade = c.create_text(34 + 55, y + 24, anchor='w', fill=PLATE_SHADE, font='RekBody', tags=row_tags)
            name = c.create_text(34 + 54, y + 23, anchor='w', fill=CREAM, font='RekBody', tags=row_tags)
            note = c.create_text(0, y + 24, anchor='w', fill=DIM, font='RekSmall', tags=row_tags)
            value = c.create_text(34 + 722 + 164, y + 23, fill=CREAM, font='RekText', tags=row_tags)
            self.save_rows.append((row, mark, shade, name, note, value))
            c.tag_bind(f'saves:row{i}', '<Button-1>', lambda _e, i=i: self.click_save(i, False))
            c.tag_bind(f'saves:row{i}', '<Double-Button-1>', lambda _e, i=i: self.click_save(i, True))
            c.tag_bind(f'saves:row{i}', '<Enter>', lambda _e: c.configure(cursor='hand2'))
            c.tag_bind(f'saves:row{i}', '<Leave>', lambda _e: c.configure(cursor=''))
        self.saves_cursor = c.create_image(0, 0, image=self.art['cursor-cell'], anchor='nw', state='disabled', tags=tags)
        self.saves_total = c.create_image(34, 0, image=self.art['total'], anchor='nw', tags=tags)
        self.saves_now = c.create_text(0, 0, anchor='w', fill=CREAM, font='RekBody', tags=tags)
        self.saves_line = c.create_text(34, 0, anchor='nw', fill=DIM, font='RekSmall', width=STAGE_W - 68, tags=tags)
        make = lambda kind, id, command: (lambda p: (self.bind_text(p, 'text', id), p)[1])(   # noqa: E731
            Plate(self, kind, 120, command, 'RekStrong', tags))
        self.saves_import = make('go40', 'ui.saves_import', self.import_chosen_save)
        self.saves_replace = make('go40', 'ui.saves_replace', self.confirm_save)
        self.saves_pick_dir = make('plate40', 'ui.saves_choose_folder', lambda: self.pick_save(True))
        self.saves_pick_zip = make('plate40', 'ui.saves_choose_zip', lambda: self.pick_save(False))
        self.saves_restore = make('plate40', 'ui.saves_restore', self.restore_save)
        self.saves_open = make('plate40', 'ui.open_folder', self.open_saves_folder)
        self.saves_back = make('plate40', 'ui.back', self.go_home)
        c.bind('<MouseWheel>', lambda e: self.screen == 'saves' and self.step_save(-1 if e.delta > 0 else 1), add='+')
        c.itemconfigure('scr:saves', state='hidden')

    def scan_saves(self):
        """(the saves found on this machine, the game folder's own, the saves imports replaced)."""
        if self.forced_saves is not None:
            return list(self.forced_saves[0]), self.forced_saves[1], []
        if not self.vars['dest'].get().strip():
            return [], None, []
        dest = self.mods_dest()
        try:
            own_dir = saves.save_dir(dest)
            mine = os.path.normcase(os.path.realpath(own_dir)) if own_dir else None
            found = [s for s in saves.candidates() if os.path.normcase(os.path.realpath(s.path)) != mine]
            return found, saves.current(dest), saves.backups(dest)
        except (OSError, saves.SaveError):
            return [], None, []

    def save_when(self, s):
        return time.strftime('%Y-%m-%d %H:%M', time.localtime(s.modified)) if s.modified else ''

    def save_value(self, s):
        return self.cat('ui.saves_cell', slots=s.slots, when=self.save_when(s))

    def save_error(self, e):
        if isinstance(e, saves.SaveError):
            return RED, self.cat('saves.' + e.code, **e.detail)
        return RED, self.cat('saves.failed', detail=str(e))

    def refresh_saves(self, rescan=True):
        """The page from what is on the machine now. Only while it is shown: showing a screen shows all its items."""
        if getattr(self, 'screen', None) != 'saves':
            return
        c = self.stage
        if rescan:
            self.save_list, self.save_own, self.save_backups = self.scan_saves()
        found = self.save_list
        at = self.save_at = max(0, min(self.save_at, len(found) - 1))
        self.save_first = max(0, min(self.save_first, len(found) - SAVES_ROWS, at), at - SAVES_ROWS + 1)
        own_dir = None
        if self.forced_saves is None and self.vars['dest'].get().strip():
            own_dir = saves.profile_dir(self.mods_dest()) / saves.SAVE
        c.itemconfigure(self.saves_path, text=self.clip(str(own_dir), 'RekSmall', 760) if own_dir else '')
        c.itemconfigure(self.saves_empty.item, state='hidden' if found else 'normal')
        for i, (row, mark, shade, name, note, value) in enumerate(self.save_rows):
            s = found[self.save_first + i] if self.save_first + i < len(found) else None
            c.itemconfigure(f'saves:row{i}', state='normal' if s else 'hidden')
            if not s:
                continue
            label = s.name or s.path.name
            where = self.clip(str(s.path), 'RekSmall', 722 - 54 - 32 - self.fonts['RekBody'].measure(label))
            c.itemconfigure(mark, text=SAVES_MARK.get(s.source, '\u25cb'))
            c.itemconfigure(name, text=label)
            c.itemconfigure(shade, text=label)
            c.itemconfigure(note, text=where)
            c.coords(note, 34 + 54 + self.fonts['RekBody'].measure(label) + 16, MODS_TOP + i * STEP_PITCH + 24)
            c.itemconfigure(value, text=self.clip(self.save_value(s), 'RekText',
                                                  self.art['cursor-cell'].width() - 2 * CURSOR_MARGIN - 24))
        c.itemconfigure(self.saves_cursor, state='disabled' if found else 'hidden')
        c.coords(self.saves_cursor, 34 + 722 - CURSOR_MARGIN, MODS_TOP + (at - self.save_first) * STEP_PITCH - CURSOR_MARGIN)
        # the game folder's own save, the line under it, the plates
        y = MODS_TOP + max(2, min(len(found), SAVES_ROWS)) * STEP_PITCH + 6
        own = self.save_own
        now = (self.cat('ui.saves_now', slots=own.slots, when=self.save_when(own)) if own
               else self.cat('ui.saves_now_none'))
        c.coords(self.saves_total, 34, y)
        c.itemconfigure(self.saves_now, text=now)
        c.coords(self.saves_now, 34 + 18, y + 22)
        fill, line = self.save_note or (DIM, self.cat('ui.saves_hint'))
        c.itemconfigure(self.saves_line, text=line, fill=fill)
        c.coords(self.saves_line, 34, y + 44 + 12)
        idle, asked = not self.save_busy, self.save_asked is not None
        self.saves_import.show(not asked)
        self.saves_replace.show(asked)
        for p, on in ((self.saves_import, idle and bool(found)), (self.saves_replace, idle),
                      (self.saves_pick_dir, idle), (self.saves_pick_zip, idle),
                      (self.saves_restore, idle and bool(self.save_backups))):
            p.state(['!disabled' if on else 'disabled'])
        self.place_save_plates()
        self.place_cursor()

    def place_save_plates(self):
        """The plates in one row above the window's lower edge, as on the Mods page: what brings a save in from the
        left, Back at the right end."""
        left = [p for p in (self.saves_import, self.saves_replace) if p.shown]
        left += [self.saves_pick_dir, self.saves_pick_zip, self.saves_restore, self.saves_open]
        right = [self.saves_back]
        for pad, gap in ((22, 10), (14, 8), (9, 6)):
            for p in left + right:
                p.fit(pad=pad, least=64)
            if sum(p.width + gap for p in left + right) - gap + 16 <= STAGE_W - 68:
                break
        x, y = 34, STAGE_H - 58
        for p in left:
            p.place(x, y)
            x += p.width + gap
        self.saves_back.place(STAGE_W - 34 - self.saves_back.width, y)

    def open_saves(self):
        self.save_note, self.save_asked, self.save_at, self.save_first = None, None, 0, 0
        self.show('saves')

    def step_save(self, step):
        """The list's cursor one row on or back; the plates' cursor steps aside, and a waiting Replace is dropped."""
        self.cursor_keys = False
        self.set_cursor(None)
        if self.save_list and not self.save_busy:
            self.save_at = max(0, min(len(self.save_list) - 1, self.save_at + step))
            self.save_asked, self.save_note = None, None
            self.refresh_saves(rescan=False)
        return 'break'

    def click_save(self, row, bring):
        """A click on a row chooses its save; a double click also brings it in."""
        if self.save_first + row < len(self.save_list) and not self.save_busy:
            self.stage.focus_set()
            self.cursor_keys = False
            self.set_cursor(None)
            self.save_at = self.save_first + row
            self.save_asked, self.save_note = None, None
            if bring:
                self.import_chosen_save()
            else:
                self.refresh_saves(rescan=False)

    def import_chosen_save(self):
        if self.save_asked is not None:
            return self.confirm_save()
        if self.save_list and not self.save_busy:
            s = self.save_list[self.save_at]
            self.ask_save(s.path, False, s.name or s.path.name)
        return 'break'

    def pick_save(self, folder):
        """A save from a folder or a .zip the player names; a folder may hold the save some levels down."""
        if self.save_busy or self.forced_saves is not None:
            return
        if folder:
            path = filedialog.askdirectory(parent=self.root, title=self.cat('ui.saves_pick_folder'), mustexist=True)
        else:
            path = filedialog.askopenfilename(parent=self.root, title=self.cat('ui.saves_pick_zip'),
                                              filetypes=[(self.cat('ui.mods_zip_files'), '*.zip')])
        if not path:
            return
        source = Path(os.path.normpath(path))
        if folder:
            root = saves.find_root(source)
            if root is None:
                self.save_asked, self.save_note = None, self.save_error(saves.SaveError('not_a_save', path=str(source)))
                return self.refresh_saves(rescan=False)
            source = root
        self.ask_save(source, not folder, source.name)

    def ask_save(self, source, archive, name):
        """Bring a save in: at once into a game folder without one, after Replace where it takes a save's place."""
        if self.forced_saves is not None:
            return
        self.save_asked = (source, archive, name)
        if self.save_own is None:
            return self.confirm_save()
        keep = saves.profile_dir(self.mods_dest()) / saves.BEFORE
        self.save_note = (NOTE, self.cat('ui.saves_confirm', name=name, path=str(keep)))
        self.refresh_saves(rescan=False)
        self.cursor_keys = True
        self.set_cursor(self.saves_replace)

    def confirm_save(self):
        if self.save_asked is None or self.save_busy:
            return 'break'
        (source, archive, name), dest = self.save_asked, self.mods_dest()
        self.save_asked, self.save_busy = None, True
        self.save_note = (SOFT, self.cat('ui.saves_importing', name=name))
        self.refresh_saves(rescan=False)
        work = (lambda: saves.import_zip(dest, source)) if archive else (lambda: saves.import_save(dest, source))
        self.background(work, lambda value: self.imported_save(value, name))
        return 'break'

    def imported_save(self, value, name):
        self.save_busy = False
        if isinstance(value, Exception):
            self.save_note = self.save_error(value)
        elif value is None:
            self.save_note = (SOFT, self.cat('ui.saves_imported', name=name))
        else:
            self.save_note = (SOFT, self.cat('ui.saves_imported', name=name) + ' '
                              + self.cat('ui.saves_kept', path=str(value)))
        self.refresh_saves()

    def restore_save(self):
        """The newest save an import replaced comes back; the imported one takes its place among the backups."""
        if self.save_busy or not self.save_backups or self.forced_saves is not None:
            return 'break'
        self.save_asked = None
        try:
            saves.restore(self.mods_dest(), self.save_backups[0])
            self.save_note = (SOFT, self.cat('ui.saves_restored'))
        except (saves.SaveError, OSError) as e:
            self.save_note = self.save_error(e)
        self.refresh_saves()
        return 'break'

    def open_saves_folder(self):
        folder = saves.profile_dir(self.mods_dest())
        try:
            folder.mkdir(parents=True, exist_ok=True)
        except OSError:
            return
        if os.name == 'nt':
            os.startfile(str(folder))
        else:
            subprocess.Popen(['xdg-open', str(folder)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                             start_new_session=True)

    def check_updates(self, force=False):
        if self.checking or self.job:
            return
        self.checking = True
        self.refresh_update()
        root = self.layout.root
        self.background(lambda: update.check(root, force), self.got_update)

    def got_update(self, info):
        self.checking = False
        self.update_info = None if isinstance(info, Exception) else info
        self.refresh_update()
        self.fit()

    def refresh_update(self):
        """The update line of the home screen: its sentence, the buttons that go with it, and what is new in a
        release that is out; the band says the same in a few words on every screen, as the in-game menu's does."""
        info, root = self.update_info, self.layout.root
        version = net.local_version(root)
        staged = update.staged_version(root) if self.args and update.can_apply(root) else ''
        shown, news, notes = [], False, ''
        if staged:      # downloaded, but the swap has not happened: the launcher exe does it at its next start
            text, news, shown = self.cat('ui.oot_update_staged', version=staged), True, [self.update_restart]
        elif self.checking:
            text = self.cat('ui.oot_update_checking')
        elif info and info['available']:
            news = True
            page = [self.update_page] if info['page'].startswith('https://') else []
            if info['can_apply'] and info['asset']:
                text, shown = self.cat('ui.oot_update_available', version=info['version']), [self.update_now] + page
            else:       # a checkout, or a release without the archive: the page has it
                text = self.cat('ui.oot_update_available' if info['can_apply'] else 'ui.oot_update_available_dev',
                                version=info['version'])
                shown = page
            notes = notes_text(info.get('notes') or '') or self.cat('ui.oot_update_no_notes')
        elif info and info['status'] == 'current':
            text = self.cat('ui.oot_update_current', version=version)
        else:
            text = self.cat('ui.oot_update_unknown', version=version)
        self.update_label.configure(text=text)
        # "update available": darker, and bold where the face has a bold
        self.stage.itemconfigure(self.update_label.item, fill=INK_WARN if news else INK_DIM,
                                 font='RekStrong' if news else 'RekText')
        available = bool(info and info['available']) and not staged
        self.stage.itemconfigure(self.band_note, text=self.cat('ui.oot_update_available', version=info['version'])
                                 if available else '')
        self.update_shown = shown + [self.update_check]
        self.update_check.state(['disabled' if self.checking else '!disabled'])
        self.notes_lines = notes.split('\n') if notes else []
        self.layout_home()

    def open_release_page(self):
        page = (self.update_info or {}).get('page') or ''
        if page.startswith('https://'):
            webbrowser.open(page)

    def start_update(self):
        info, root = self.update_info, self.layout.root
        if not info or not info.get('asset') or self.job or self.process:
            return

        def work(emit, cancelled):
            def report(step, state, done, total):
                if state == 'progress':
                    emit({'type': 'progress', 'step': step, 'phase': 'download' if step == 'update_download' else 'unpack',
                          'done': done, 'total': total, 'unit': 'bytes'})
                else:
                    emit({'type': 'step', 'step': step, 'state': state})
            return update.install(info, root, report, cancelled)

        self.begin_job('update', update.STEPS, ('ui.oot_updating', {'version': info['version']}), work)

    def restart_for_update(self):
        """Hand over to the launcher exe, which swaps the staged release in while no Python of this folder runs."""
        if update.restart(self.layout.root):
            self.root.destroy()
            return True
        return False

    # ---- the optional components -----------------------------------------------------------------------------------

    def retry_components(self):
        """Download the libraries the game folder lacks, as the setup's components step does."""
        dest, layout = self.game_exe().parent, self.layout
        if self.job or self.process or not dest.is_dir():
            return
        known, gpu = self.gpu_known, self.gpu

        def work(emit, cancelled):
            step = 'oot_components'
            emit({'type': 'step', 'step': step, 'state': 'started'})
            emit({'type': 'progress', 'step': step, 'phase': 'detect'})
            card = gpu if known else components.detect()
            temp = dest / '.components-temp'
            try:
                if layout.shim.is_file() and 'dlssg' in components.groups_for(card):
                    shutil.copy2(layout.shim, dest / oot.SHIM)
                result = components.install(dest, card, temp, cancel=cancelled, progress=lambda group, done, total: emit(
                    {'type': 'progress', 'step': step, 'phase': group, 'done': done, 'total': total, 'unit': 'bytes'}))
            except OSError as e:
                raise JobFailed('oot.components_failed', files='-', detail=str(e)) from None
            finally:
                shutil.rmtree(temp, ignore_errors=True)
            if result['failed']:
                raise JobFailed('oot.components_failed', files=', '.join(result['failed']),
                                detail=next(iter(result['failed'].values()))[:300])
            emit({'type': 'step', 'step': step, 'state': 'done'})
            return card

        self.begin_job('components', ('oot_components',), ('ui.oot_components_title', {}), work)

    # ---- jobs: the launcher's own work on the run screen ----------------------------------------------------------

    def begin_job(self, kind, steps, title, work):
        self.job_title, self.finish_kind = title, kind
        self.model = Model()
        self.clear_log()
        self.cancel_button.configure(state='normal')
        self.bound = [b for b in self.bound if b[0] is not self.cancel_button]
        self.bind_text(self.cancel_button, 'text', 'ui.cancel')
        self.job_event({'type': 'start', 'steps': list(steps), 'mode': 'update', 'dest': self.vars['dest'].get().strip()})
        self.job_event({'type': 'eta', **eta.estimates({s: 'first' for s in steps})})
        self.build_step_rows(self.model.steps)
        self.show('run')
        self.refresh_run()
        self.job = Job(kind, work)
        self.root.after(POLL_MS, self.poll_job)

    def job_event(self, event):
        self.tracker.feed(event)
        self.model.apply(event)
        self.log_entries.append(event)
        self.append_log(describe_event(self.cat, event))

    def poll_job(self):
        job = self.job
        if job is None:
            return
        try:
            while True:
                self.job_event(job.events.get_nowait())
        except queue.Empty:
            pass
        self.refresh_run()
        if job.thread.is_alive() or not job.events.empty():
            self.root.after(POLL_MS, self.poll_job)
            return
        self.job = None
        if self.closing:
            self.root.destroy()
            return
        result, value = job.outcome or ('failed', ('internal', {'exception': 'Job', 'detail': 'no outcome'}))
        if result == 'ok' and job.kind == 'update':
            self.job_event({'type': 'step', 'step': 'update_restart', 'state': 'started'})
            self.refresh_run()
            if self.restart_for_update():
                return
            result, value = 'failed', ('update.restart_failed', {})
        if result == 'ok':          # the components are in: back to Play
            if job.kind == 'components':
                self.gpu, self.gpu_known = value, True
            self.go_home()
        elif result == 'cancelled':
            self.go_home()
        else:
            self.job_event({'type': 'error', 'id': value[0], 'params': value[1], 'step': self.model.current})
            self.job_event({'type': 'end', 'result': 'failed'})
            self.model.finish(1)
            self.refresh_run()
            self.refresh_finish()
            self.show('finish')

    def cancel(self):
        if self.job:
            self.cancel_button.configure(state='disabled')
            self.bound = [b for b in self.bound if b[0] is not self.cancel_button]
            self.bind_text(self.cancel_button, 'text', 'ui.cancelling')
            self.job.cancel.set()
            return
        super().cancel()

    def on_close(self):
        self.shader_stop()   # no question: the preparation goes on at the next start
        if self.job:
            if not messagebox.askyesno(self.cat('ui.oot_title'), self.cat('ui.confirm_cancel')):
                return
            self.closing = True
            self.cancel()
            return
        super().on_close()

    # ---- the setup run --------------------------------------------------------------------------------------------

    def start(self):
        self.shader_stop()
        self.shader_job = None   # a run of the setup may have changed what there is to prepare: judged again after it
        save_settings(game=self.extra['game'].get().strip(), dest=self.vars['dest'].get().strip())
        self.job_title, self.pending_title, self.finish_kind = self.pending_title, None, None
        self.tracker.reset()
        super().start()

    def feed(self, line):
        event = parse_line(line)
        if event is not None:
            self.tracker.feed(event)
        super().feed(line)

    def build_run_stage(self):
        """The run screen's fixed pieces on the stage; the step rows come with the steps (build_step_rows). The texts
        are those of gui.py's own run frame, read from its widgets each time they are refreshed, and the times are
        the tracker's (eta.py)."""
        c, tags = self.stage, ('scr:run',)
        self.stage_rows, self.steps_top, self.run_view = {}, STEPS_TOP, None
        self.run_title = c.create_text(34, 78, anchor='nw', fill=CREAM, font='RekTitle', width=STAGE_W - 68, tags=tags)
        self.run_cursor = c.create_image(0, 0, image=self.art['cursor-cell'], anchor='nw', tags=tags)
        self.run_phase = c.create_text(34, 0, anchor='nw', fill=SOFT, font='RekSmall', width=STAGE_W - 68, tags=tags)
        # the total line: the step's number at the left, the time left in the menu's note colour and the time gone
        # by at the right
        self.run_total = c.create_image(34, 0, image=self.art['total'], anchor='nw', tags=tags)
        self.run_step = c.create_text(0, 0, anchor='w', fill=CREAM, font='RekBody', tags=tags)
        self.run_left = c.create_text(0, 0, anchor='e', fill=NOTE, font='RekBody', tags=tags)
        self.run_elapsed = c.create_text(0, 0, anchor='e', fill=SOFT, font='RekText', tags=tags)
        self.run_warn = c.create_text(34, 0, anchor='nw', fill=NOTE, font='RekSmall', width=STAGE_W - 68, tags=tags)
        self.run_space = c.create_text(34, 0, anchor='nw', fill=DIM, font='RekSmall', width=STAGE_W - 68, tags=tags)
        self.run_log_plate = Plate(self, 'plate40', 200, self.toggle_log, 'RekStrong', tags)
        self.run_cancel_plate = Plate(self, 'plate40', 170, self.cancel, 'RekStrong', tags)
        c.itemconfigure('scr:run', state='hidden')

    def build_stage_rows(self, steps):
        """A row per step on the stage: the strip with its mark and name, the value in the dark cell at its right, and
        an amber bar in the cell of the step that runs."""
        c, tags = self.stage, ('scr:run', 'steps')
        c.delete('steps')
        self.stage_rows = {}
        for i, s in enumerate(steps):
            y = self.steps_top + i * STEP_PITCH
            row = c.create_image(34, y, image=self.art['row'], anchor='nw', tags=tags)
            fill = c.create_rectangle(0, 0, 0, 0, fill=AMBER, outline='', tags=tags)
            mark = c.create_text(34 + 28, y + 23, fill=CREAM, font='RekMark', tags=tags)
            shade = c.create_text(34 + 55, y + 24, anchor='w', fill=PLATE_SHADE, font='RekBody', tags=tags)
            name = c.create_text(34 + 54, y + 23, anchor='w', fill=CREAM, font='RekBody', tags=tags)
            value = c.create_text(34 + 722 + 164, y + 23, fill=DIM, font='RekText', tags=tags)
            self.stage_rows[s] = (row, fill, mark, name, value, shade)
        c.tag_raise(self.run_cursor)
        if self.screen != 'run' or self.log_open:
            c.itemconfigure('steps', state='hidden')

    def refresh_stage_run(self):
        """The drawn run screen: the header, the steps with their times, the phase, the total line and the lines
        under it, each block under the one before; the log's box in the place of the steps while it is open."""
        m, c, view = self.model, self.stage, self.run_view or self.tracker.view()
        times = {r['id']: r for r in view['steps']}
        # the title in one line, at the smaller size when the larger does not fit; a longer one wraps and the
        # steps start under it
        title, room = self.run_header.cget('text'), STAGE_W - 68
        c.itemconfigure(self.run_title, text=title,
                        font='RekTitle' if self.fonts['RekTitle'].measure(title) <= room else 'RekTitleSmall')
        box = c.bbox(self.run_title)
        top = max(STEPS_TOP, (box[3] if box else 0) + 10)
        if top != self.steps_top:
            c.move('steps', 0, top - self.steps_top)
            self.steps_top = top
        running = None
        if not self.log_open:
            c.itemconfigure('steps', state='normal')
        for i, (s, (row, fill, mark, name, value, shade)) in enumerate(self.stage_rows.items()):
            y = top + i * STEP_PITCH
            state = m.states.get(s, 'pending')
            waiting = state in ('pending', 'skipped')
            text = self.step_rows[s][1].cget('text') if s in self.step_rows else ''
            c.itemconfigure(row, image=self.art['row-dim' if waiting else 'row'])
            c.itemconfigure(mark, text=GLYPH[state], fill={'done': DONE_TEXT, 'failed': RED}.get(state, DIM if waiting else CREAM))
            c.itemconfigure(name, text=text, fill=DIM if waiting else CREAM, font='RekPlate' if state == 'running' else 'RekBody')
            c.itemconfigure(shade, text='' if waiting else text, font='RekPlate' if state == 'running' else 'RekBody')
            c.itemconfigure(value, text=self.step_times[s].cget('text') if s in self.step_times else '',
                            fill={'done': DONE_TEXT, 'running': CREAM, 'failed': RED}.get(state, DIM))
            if state == 'running':
                running = y
                c.coords(fill, 34 + 725, y + 6, 34 + 725 + round(321 * step_share(times.get(s))), y + 40)
            c.itemconfigure(fill, state='normal' if state == 'running' else 'hidden')
        c.itemconfigure(self.run_cursor, state='normal' if running is not None and not self.log_open else 'hidden')
        if running is not None:
            c.coords(self.run_cursor, 34 + 722 - 14, running - 14)
        count = len(self.stage_rows)
        if self.log_open:
            count = max(count, 7)
            c.itemconfigure('steps', state='hidden')
            c.coords(self.log_item, 34, top)
            c.itemconfigure(self.log_item, height=count * STEP_PITCH - 8)
        c.itemconfigure(self.log_item, state='normal' if self.log_open else 'hidden')
        y = top + count * STEP_PITCH - 8 + 10
        phase = self.phase_label.cget('text')
        if m.current and m.phase and m.total:   # the counter stays in one piece where the line wraps
            phase = (self.cat(f'phase.{m.current}.{m.phase}') + '  ' +
                     amount(self.cat, m.done, m.total, m.unit).replace(' ', ' '))
        c.itemconfigure(self.run_phase, text=phase)
        c.coords(self.run_phase, 34, y)
        if phase:
            y = c.bbox(self.run_phase)[3] + 8
        timed = bool(view['steps'])
        for item in (self.run_total, self.run_step, self.run_left, self.run_elapsed):
            c.itemconfigure(item, state='normal' if timed else 'hidden')
        if timed:
            going = bool(view['running'])
            c.coords(self.run_total, 34, y)
            c.itemconfigure(self.run_step, text=self.cat('eta.step_of', index=view['index'], count=view['count'])
                            if going and view['index'] else '')
            c.itemconfigure(self.run_left, text=eta.about_text(self.cat, view['remaining'], total=True) if going else '')
            c.itemconfigure(self.run_elapsed, text=self.cat('eta.elapsed', time=eta.clock_text(view['elapsed'])))
            c.coords(self.run_step, 34 + 18, y + 22)
            c.coords(self.run_elapsed, STAGE_W - 34 - 18, y + 22)
            c.coords(self.run_left, c.bbox(self.run_elapsed)[0] - 22, y + 22)
            y += 44 + 8
        self.run_log_plate.configure(text=self.log_toggle.cget('text'))
        self.run_log_plate.fit(least=170)
        self.run_log_plate.place(34, STAGE_H - 58)
        self.run_cancel_plate.configure(text=self.cancel_button.cget('text'))
        self.run_cancel_plate.fit(least=150)
        self.run_cancel_plate.place(STAGE_W - 34 - self.run_cancel_plate.width, STAGE_H - 58)
        self.run_cancel_plate.state(['disabled' if self.cancel_button.instate(['disabled']) else '!disabled'])
        # the warnings, then the free space, in the room that is left above the buttons: the oldest warnings give
        # way first (the log has them all), then the space line
        limit = STAGE_H - 58 - 6
        warnings = [line for line in self.warn_label.cget('text').split('\n') if line]
        c.coords(self.run_warn, 34, y)
        c.itemconfigure(self.run_warn, text='\n'.join(warnings))
        while len(warnings) > 1 and c.bbox(self.run_warn)[3] > limit:
            warnings.pop(0)
            c.itemconfigure(self.run_warn, text='\n'.join(warnings))
        if warnings:
            y = c.bbox(self.run_warn)[3] + 4
        c.coords(self.run_space, 34, y)
        c.itemconfigure(self.run_space, text=self.space_label.cget('text'))
        box = c.bbox(self.run_space)
        if box and box[3] > limit:
            c.itemconfigure(self.run_space, text='')
        self.place_cursor()

    def build_step_rows(self, steps):
        """A row per step, like a setting in the game: its mark and name on a strip, and at the right, in the dark
        value cell, the step's time (eta.py): how long it took, how far it is, or about how long it will take.
        gui.py writes each step's mark and name into labels of its own frame, which is not shown; they are made
        here, with one more for the time, and the rows on the stage read them."""
        for w in self.steps_box.winfo_children():
            w.destroy()
        self.step_rows, self.step_times = {}, {}
        for s in steps:
            self.step_rows[s] = (ttk.Label(self.steps_box), ttk.Label(self.steps_box))
            self.step_times[s] = ttk.Label(self.steps_box)
        if hasattr(self, 'stage'):
            self.build_stage_rows(steps)

    def refresh_run(self):
        view = self.run_view = self.tracker.view()
        rows = {r['id']: r for r in view['steps']}
        for s, label in self.step_times.items():
            label.configure(text=eta.step_text(self.cat, rows[s]) if s in rows else '')
        super().refresh_run()
        if self.job_title:
            self.run_header.configure(text=self.cat(self.job_title[0], **self.job_title[1]))
        if hasattr(self, 'stage') and self.screen == 'run':
            self.refresh_stage_run()

    def retext(self):
        self.fonts = set_fonts(self.root, self.cat.lang, self.menu_font, self.base_font)   # the new language's letters
        super().retext()
        self.root.title(self.cat('ui.oot_title'))
        self.fill_prompts()
        self.fill_textures()
        self.refresh_home()
        self.refresh_mods(rescan=False)

    def notice(self, parent):
        w = ttk.Label(parent, style='Notice.TLabel', wraplength=WRAP - 28, justify='left')
        self.bind_text(w, 'text', 'ui.oot_notice')
        return w

    # ---- form -----------------------------------------------------------------------------------------------------

    def build_form(self):
        """The form: every entry is a row of the game's settings screen, its name on a khaki strip and its value in
        a dark cell at the strip's right end, a folder's cell with Browse beside it."""
        f = ttk.Frame(self.body)
        bar = ttk.Frame(f)
        bar.pack(side='bottom', fill='x', pady=(16, 0))
        self.start_button = self.button(bar, 'ui.start', self.start, style='Go.TButton')
        self.start_button.pack(side='right')
        self.home_button = self.button(bar, 'ui.back', self.go_home)   # to the home screen, once there is a game
        self.home_button.pack(side='left')
        self.form_intro = self.label(f, 'ui.oot_intro', wraplength=WRAP, justify='left')
        self.form_intro.pack(anchor='w', pady=(0, 12))
        rows = ttk.Frame(f)
        rows.pack(fill='x')

        def row(label_id, pick=None, **params):
            """One row; returns the cell its value goes into. All cells are one size, so the values stand in a
            column; a name too long for its strip takes a second line."""
            strip = tk.Frame(rows, background=ROW, highlightbackground=ROW_EDGE, highlightthickness=1)
            strip.pack(fill='x', pady=(0, 6))
            cell = tk.Frame(strip, background=ROW, width=FORM_VALUE_W, height=32)
            cell.pack(side='right', padx=3, pady=3)
            cell.pack_propagate(False)
            if pick:
                self.button(cell, 'ui.browse', pick, style='Cell.TButton').pack(side='right', fill='y', padx=(3, 0))
            name = ttk.Label(strip, style='Row.TLabel', wraplength=WRAP - FORM_VALUE_W - 36, justify='left')
            self.bind_text(name, 'text', label_id, **params)
            name.pack(side='left', padx=(14, 10), pady=4)
            return cell

        def entry(cell, var):
            ttk.Entry(cell, textvariable=var, font='RekText').pack(side='left', fill='both', expand=True)

        def choice(cell, var):
            box = ttk.Combobox(cell, textvariable=var, state='readonly', font='RekText', height=16)
            box.pack(side='left', fill='both', expand=True)
            return box

        entry(row('ui.oot_game', self.pick_game), self.extra['game'])
        entry(row('ui.dest', self.pick_dest), self.vars['dest'])
        self.lang_box = choice(row('ui.game_language'), self.vars['lang'])
        self.lang_touched = False
        self.lang_box.bind('<<ComboboxSelected>>', lambda _e: setattr(self, 'lang_touched', True))
        self.fill_languages()
        self.prompts_box = choice(row('ui.oot_prompts'), self.prompts_var)
        self.prompts_code = PROMPTS[0]
        self.prompts_box.bind('<<ComboboxSelected>>', self.pick_prompts)
        self.fill_prompts()
        # the setup's texture step: a choice of two, on by default (2026-11-16, an RTX 5070 tour of eight places from a
        # fresh start: with the prepared cache the village's first visit ran at 96 fps with 4 frames over 25 ms a
        # minute, where converting its textures in play gave 53 fps and 354); its size and the drive's room below it
        self.textures_box = choice(row('ui.oot_textures', gib=TEXTURES_GIB), self.textures_var)
        self.textures_on = True
        self.textures_box.bind('<<ComboboxSelected>>', self.pick_textures)
        self.fill_textures()
        entry(row('ui.oot_dlls', self.pick_dlls), self.extra['dlls'])
        self.dlls_help = self.label(f, 'ui.oot_dlls_help', wraplength=WRAP, justify='left', style='Dim.TLabel')
        self.dlls_help.pack(anchor='w', pady=(2, 0))
        # The paragraph under the rows explains the libraries' row; while the texture row has the focus (opened, chosen
        # from, or reached with Tab) it explains that row instead, the one other row whose choice needs a sentence.
        self.textures_box.bind('<FocusIn>', lambda _e: self.form_help('ui.oot_textures_help'), add='+')
        self.textures_box.bind('<FocusOut>', lambda _e: self.form_help('ui.oot_dlls_help'), add='+')
        # lines that are empty most of the time: place_form_lines gives each its room only while it says something
        self.form_hw = ttk.Label(f, style='Dim.TLabel', wraplength=WRAP, justify='left')   # the card and its preset
        self.form_space = ttk.Label(f, style='Hint.TLabel', wraplength=WRAP, justify='left')   # room for the textures
        self.update_hint = ttk.Label(f, style='Hint.TLabel', wraplength=WRAP, justify='left')
        self.form_error = ttk.Label(f, style='Err.TLabel', wraplength=WRAP, justify='left')
        self.form_notice = self.notice(f)
        self.form_notice.pack(fill='x', pady=(12, 0))
        return f

    def form_help(self, id):
        """The form's help paragraph says the text of `id`, and keeps saying it after a language change."""
        self.bound = [(w, o, id if w is self.dlls_help else i, p) for w, o, i, p in self.bound]
        self.dlls_help.configure(text=self.cat(id))

    def place_form_lines(self):
        """The card's line, the "this folder is updated" line and the error line of the form, each in its place
        above the notice while it has text and gone while it has none (an empty label would still take a line)."""
        # a short drive's line takes the place of the libraries' help, which the window has no room for beside it
        self.dlls_help.pack_forget()
        if not self.form_space.cget('text'):
            self.dlls_help.pack(anchor='w', pady=(2, 0), before=self.form_notice)
        for w in (self.form_hw, self.form_space, self.update_hint, self.form_error):
            w.pack_forget()
            if w.cget('text'):
                w.pack(anchor='w', pady=(8, 0), before=self.form_notice)

    def fill_prompts(self):
        labels = [self.cat('ui.oot_prompts_' + p) for p in PROMPTS]
        self.prompts_box.configure(values=labels)
        self.prompts_var.set(labels[PROMPTS.index(self.prompts_code)])

    def fill_textures(self):
        self.textures_box.configure(values=[self.cat('ui.oot_textures_yes'), self.cat('ui.oot_textures_no')])
        self.textures_box.current(0 if self.textures_on else 1)

    def pick_textures(self, _event=None):
        i = self.textures_box.current()
        if i >= 0:
            self.textures_on = i == 0
            self.refresh_form()

    def textures_space_text(self):
        """The drive's room for the game and the prepared textures, said only when it is short: the setup then
        prepares what fits (tools/oot_prepare_textures.py stops with 2 GiB left) and the game makes the rest in play,
        so a short drive is a sentence here, not a failed install. Empty while it fits or the folder is unknown."""
        dest = self.vars['dest'].get().strip()
        if not self.textures_on or not dest or getattr(self, 'forced_free', None) is None and not os.path.isabs(dest):
            return ''
        try:
            free = self.forced_free if getattr(self, 'forced_free', None) is not None else oot.free_bytes(dest)
        except OSError:
            return ''
        need = oot.NEED_DEST + (TEXTURES_GIB << 30)
        if free >= need:
            return ''
        return self.cat('ui.oot_textures_space', free=round(free / 2**30), need=round(need / 2**30))

    def pick_prompts(self, _event=None):
        i = self.prompts_box.current()
        if i >= 0:
            self.prompts_code = PROMPTS[i]

    def pick_game(self):
        path = filedialog.askdirectory(title=self.cat('ui.oot_pick_game'), mustexist=True)
        if path:
            self.extra['game'].set(os.path.normpath(path))

    def pick_dlls(self):
        path = filedialog.askdirectory(title=self.cat('ui.oot_pick_dlls'), mustexist=True)
        if path:
            self.extra['dlls'].set(os.path.normpath(path))

    def form(self):
        return OotForm(game=self.extra['game'].get(), dest=self.vars['dest'].get(), lang=self.selected_lang(),
                       prompts=self.prompts_code, dlls=self.extra['dlls'].get(), textures=self.textures_on,
                       toolchain=str(self.args.toolchain) if self.args and self.args.toolchain else '')

    def refresh_form(self):
        self.update_hint.configure(text=self.cat('ui.oot_update_mode') if self.form().installed_state() else '')
        self.home_button.state(['!disabled' if self.game_exe().is_file() else 'disabled'])
        self.form_hw.configure(text=self.hardware_text())
        self.form_space.configure(text=self.textures_space_text())
        self.fit()

    def go_home(self):
        """Back to Play when there is a game, else to the form."""
        if self.game_exe().is_file():
            self.refresh_home()
            self.show('home')
        else:
            self.show('form')

    # ---- finish ---------------------------------------------------------------------------------------------------

    def build_finish(self):
        f = super().build_finish()
        bar = self.open_button.master
        self.play_button = self.button(bar, 'ui.oot_play', self.start_game, style='Go.TButton')
        self.play_button.pack(side='right', padx=(0, 8), before=self.open_button)
        self.back_button.configure(command=self.leave_finish)
        return f

    def leave_finish(self):
        """Back from the finish screen: to the form after a setup run, to the home screen after one of the
        launcher's own jobs."""
        if self.finish_kind:
            self.go_home()
        else:
            self.show('form')

    def refresh_finish(self):
        super().refresh_finish()
        m = self.model
        result = m.result or 'ok'
        if result == 'ok':
            self.finish_text.configure(text=self.cat('ui.oot_done_text', dest=m.dest))
        elif result == 'failed' and self.finish_kind:
            self.finish_title.configure(text=self.cat('ui.oot_update_failed' if self.finish_kind == 'update'
                                                      else 'ui.oot_components_failed'))
        view = self.tracker.view()
        if view['steps'] and view['elapsed'] >= 1:
            self.finish_text.configure(text=self.finish_text.cget('text') + '\n\n' +
                                       self.cat('eta.finished', time=eta.clock_text(view['elapsed'])))
        for label, step in zip(self.finish_steps.winfo_children(), m.steps):   # gui.py's colours are for a light window
            label.configure(foreground={'done': DONE_TEXT, 'failed': RED}.get(m.states.get(step, 'pending'), DIM))
        # the notice is about the folder a setup built: it has its place after a setup that worked
        self.finish_notice.pack_forget()
        if result == 'ok' and not self.finish_kind:
            self.finish_notice.pack(fill='x', pady=(8, 0))
        exe = Path(m.dest or '.') / oot.GAME_EXE
        self.play_button.state(['!disabled' if exe.is_file() else 'disabled'])
        # amber is the one thing to press next: Play after a run that worked, else nothing here is
        self.play_button.configure(style='Go.TButton' if result == 'ok' else 'TButton')
        self.fit()
        if self.screen == 'finish':
            self.panel_focus()

    def start_game(self):
        dest = Path(self.model.dest or self.vars['dest'].get().strip())
        exe = dest / oot.GAME_EXE
        if exe.is_file():
            save_settings(game=self.extra['game'].get().strip(), dest=str(dest))
            pending = self.shader_wanted() and not (self.shader_job is not None and self.shader_job.paused)
            self.shader_stop()   # nothing of the preparation runs while the game does
            flags = subprocess.DETACHED_PROCESS if os.name == 'nt' else 0
            subprocess.Popen([str(exe)], cwd=str(dest), creationflags=flags, close_fds=True)
            if pending:
                # The preparation is not done: the window goes out of sight (not destroyed: the press came from a
                # binding of the stage's canvas) and takes the job up again when the game has ended (shader_step).
                self.shader_hidden = True
                self.root.withdraw()
                return
            # The press came from a binding of the stage's canvas (the Play plate): taking the window down from inside
            # its own event makes Tcl free what it is still running and panic with a "Fatal Error" box. The settings are
            # saved and the game runs on its own, so the launcher just ends, without Tk's teardown.
            for stream in (sys.stdout, sys.stderr):
                try:
                    stream.flush()
                except (OSError, ValueError, AttributeError):
                    pass
            os._exit(0)


# ---- self test ---------------------------------------------------------------------------------------------------

STEPS = [s.id for s in oot.all_steps()]
SAMPLE_START = {'type': 'start', 'release': 'selftest', 'mode': 'install', 'dest': 'C:/Games/Rekarina',
                'log': 'D:\\log.txt', 'steps': STEPS, 't': 0}
SAMPLE_ETA = {'type': 'eta', 't': 0, **eta.estimates({s: 'first' for s in STEPS}, n_cores=16, timings={})}
SAMPLE_RUN = [
    SAMPLE_START, SAMPLE_ETA,
    {'type': 'plan', 'run': STEPS, 'skip': {}, 't': 0},
    {'type': 'step', 'step': 'oot_check', 'state': 'started', 't': 0},
    {'type': 'space', 'dest': 'C:/Games/Rekarina', 'need': 24_000_000_000, 'free': 210_000_000_000, 'temp': 'D:\\t',
     'temp_need': 0, 'temp_free': 0, 'same_disk': True, 't': 8},
    {'type': 'step', 'step': 'oot_check', 'state': 'done', 'seconds': 9.0, 't': 9},
    {'type': 'step', 'step': 'oot_extract', 'state': 'started', 't': 9},
    {'type': 'step', 'step': 'oot_extract', 'state': 'done', 'seconds': 21.2, 't': 30},
    {'type': 'step', 'step': 'oot_lift', 'state': 'started', 't': 30},
    {'type': 'step', 'step': 'oot_lift', 'state': 'done', 'seconds': 98.0, 't': 128},
    {'type': 'step', 'step': 'oot_compile', 'state': 'started', 't': 128},
    {'type': 'progress', 'step': 'oot_compile', 'phase': 'compile', 'done': 412, 'total': 681, 'unit': 'objects', 't': 432},
]
# the same run a little later: the libraries for the graphics card are coming in
SAMPLE_COMPONENTS = SAMPLE_RUN + [
    {'type': 'step', 'step': 'oot_compile', 'state': 'done', 'seconds': 640.0, 't': 768},
    {'type': 'step', 'step': 'oot_components', 'state': 'started', 't': 768},
    {'type': 'progress', 'step': 'oot_components', 'phase': 'dlss', 'done': 31_000_000, 'total': 79_000_000,
     'unit': 'bytes', 't': 790},
]
SAMPLE_OK = [SAMPLE_START, SAMPLE_ETA, {'type': 'plan', 'run': STEPS, 'skip': {}, 't': 0}] + \
            [{'type': 'step', 'step': s, 'state': st, 't': 300 * (i + (st == 'done')), **({'seconds': 300.0} if st == 'done' else {})}
             for i, s in enumerate(STEPS) for st in ('started', 'done')] + \
            [{'type': 'end', 'result': 'ok', 'seconds': 2100, 't': 2100}]
SAMPLE_FAIL = [SAMPLE_START, SAMPLE_ETA, {'type': 'plan', 'run': STEPS, 'skip': {}, 't': 0},
               {'type': 'step', 'step': 'oot_check', 'state': 'started', 't': 0},
               {'type': 'error', 'id': 'oot.other_version', 'step': 'oot_check', 't': 2,
                'params': {'path': 'D:\\dump\\game.nup', 'version': '1.1.1', 'files': 'main, sdk'}},
               {'type': 'end', 'result': 'failed', 'seconds': 2, 't': 2}]
UPDATE_STEPS = list(update.STEPS)
SAMPLE_UPDATE = [
    {'type': 'start', 'steps': UPDATE_STEPS, 'mode': 'update', 'dest': 'C:/Games/Rekarina', 't': 0},
    {'type': 'eta', 't': 0, **eta.estimates({s: 'first' for s in UPDATE_STEPS}, n_cores=16, timings={})},
    {'type': 'step', 'step': 'update_download', 'state': 'started', 't': 0},
    {'type': 'progress', 'step': 'update_download', 'phase': 'download', 'done': 212_000_000, 'total': 561_000_000,
     'unit': 'bytes', 't': 31},
]
SAMPLE_GPU = components.Gpu('NVIDIA GeForce RTX 5070', 'nvidia', 12 << 30)
SAMPLE_NOTES = ('Rekarina 1.0.1\n\n- Shorter loading between areas.\n- The water ripple keeps its speed above 60 fps.\n'
                '- New button prompt set for PlayStation pads.\n- The setup downloads DLSS and FSR 3.1 by itself.\n\n'
                'The update keeps your saves, settings and shader caches. After it the setup rebuilds the game from the '
                'dump you already have; that takes a few minutes.')
SCREENS = ('form', 'form-textures-help', 'form-space', 'home', 'home-update', 'home-shaders-running', 'home-shaders-waiting', 'home-shaders-done', 'mods', 'mods-empty', 'saves', 'saves-ask', 'saves-empty', 'update', 'run',
           'components', 'ok', 'failed')


def sample_mods():
    """A mods folder for the pictures: files, a code patch that is built in and one that waits, a mod switched off,
    cheats, and a mod the game cannot use."""
    make = lambda name, **kw: mods.Mod(name=name, path=Path('C:/Games/Rekarina/mods') / name, **kw)   # noqa: E731
    return [make('Sharper interface pack', kinds=(mods.FILES,), files=214),
            make('Auto jump', kinds=(mods.PATCH,), patches=1),
            make('Harder enemies', kinds=(mods.FILES, mods.PATCH), files=12, patches=2, waits=True),
            make('Longer draw distance', kinds=(mods.FILES,), files=38, enabled=False),
            make('My cheats', kinds=(mods.CHEATS,), cheats=6),
            make('Camera plugin', kinds=(mods.FILES,), files=3, problem=mods.PLUGIN)]


def sample_saves():
    """Saves for the pictures: another port's, an emulator's, and the game folder's own."""
    make = lambda source, name, path, slots, when: saves.Save(   # noqa: E731
        Path(path), source, name, slots, 2 + 2 * slots, 160_000 * slots, when)
    found = [make(saves.PORT, 'Another port', 'C:/Users/Player/Games/Another port/user/save/5a3c9e1f0b7d2468', 3, 1794400000),
             make(saves.EMULATOR, 'Ryujinx', 'C:/Users/Player/AppData/Roaming/Ryujinx/bis/user/save/0000000000000007/0',
                  2, 1793800000)]
    return found, make(saves.OWN, '', 'C:/Games/Rekarina/profile/save/7ac0f61a', 1, 1794700000)


def sample_update(version, available):
    return {'status': 'available' if available else 'current', 'available': available, 'current': '1.0.0',
            'version': version, 'notes': SAMPLE_NOTES if available else '', 'page': 'https://github.com/' + update.REPO,
            'asset': {'name': update.asset_name(version), 'url': '', 'size': 561_000_000, 'digest': ''} if available else None,
            'sums_url': '', 'checked': 0, 'can_apply': True, 'reason': ''}


def selftest(layout, shots=None):
    """Every screen in English and Russian with sample events; a PNG of each with --shots. 0 when all were drawn.
    On Windows the window is never seen and never takes the keyboard (shots.py): the test can run while something
    else is in front."""
    fix_tcl_paths()
    unseen = screenshots.AVAILABLE
    root = screenshots.unseen_root() if unseen else tk.Tk()
    drawn = []

    def shot(name):
        if shots and not unseen:   # the picture is taken off the screen: the window has to be on top of what is open
            root.attributes('-topmost', True)
            root.lift()
        root.update()
        drawn.append(name)
        if shots:
            shots.mkdir(parents=True, exist_ok=True)
            if unseen:
                screenshots.save(root, shots / f'{name}.png')
            else:
                grab(root, shots / f'{name}.png')

    def play(app, events):
        """Sample events into the model and the step times, the clock standing at the last event's time."""
        app.model.__init__()
        app.clear_log()
        for e in events:
            app.tracker.feed(e, now=e.get('t', 0))
            app.model.apply(e)
        app.tracker.clock = lambda: events[-1].get('t', 0)
        app.model.dest = events[0]['dest']

    def run_all():
        for lang in ('en', 'ru'):
            app = RekarinaApp(root, lang, layout)
            app.extra['game'].set('C:\\Dumps\\OoT')
            app.vars['dest'].set('C:\\Games\\Rekarina')
            app.gpu, app.gpu_known, app.forced_missing, app.forced_mods = SAMPLE_GPU, True, [], sample_mods()
            app.refresh_form()
            app.show('form')
            shot(f'oot-form-{lang}')
            app.form_help('ui.oot_textures_help')   # the texture row in focus: its paragraph instead of the libraries'
            shot(f'oot-form-textures-help-{lang}')
            app.form_help('ui.oot_dlls_help')
            app.textures_on = True       # chosen, on a drive with 20 GiB free: the textures' line says what happens
            app.fill_textures()
            app.forced_free = 20 << 30
            app.refresh_form()
            shot(f'oot-form-space-{lang}')
            app.forced_free = None
            app.textures_on = False
            app.fill_textures()
            app.update_info = sample_update('1.0.0', False)
            app.refresh_home()
            app.home_play.state(['!disabled'])
            app.home_text.configure(text=app.cat('ui.oot_home_text', dest=app.vars['dest'].get()))
            app.show('home')
            shot(f'oot-home-{lang}')
            app.update_info, app.forced_missing = sample_update('1.0.1', True), ['dlssg']
            app.refresh_home()
            app.home_play.state(['!disabled'])
            app.home_text.configure(text=app.cat('ui.oot_home_text', dest=app.vars['dest'].get()))
            shot(f'oot-home-update-{lang}')
            for name, view in (('running', {'status': app.cat('ui.oot_shaders_ready'), 'fraction': 0.38, 'button': 'ui.oot_shaders_pause',
                                            'note': app.cat('ui.oot_shaders_note'),
                                            'detail': app.cat('ui.oot_shaders_running', done='18 240', total='47 542', minutes=22)}),
                               ('waiting', {'status': app.cat('ui.oot_shaders_ready'), 'fraction': None, 'button': None,
                                            'note': app.cat('ui.oot_shaders_note'), 'detail': app.cat('ui.oot_shaders_waiting')}),
                               ('done', {'status': app.cat('ui.oot_shaders_done'), 'fraction': None, 'button': None, 'note': '', 'detail': ''})):
                app.forced_shaders = view
                app.update_info, app.forced_missing = sample_update('1.0.0', False), []
                app.refresh_home()
                app.home_play.state(['!disabled'])
                app.home_text.configure(text=app.cat('ui.oot_home_text', dest=app.vars['dest'].get()))
                shot(f'oot-home-shaders-{name}-{lang}')
            app.forced_shaders = None
            app.open_mods()
            shot(f'oot-mods-{lang}')
            app.forced_mods = []
            app.refresh_mods()
            shot(f'oot-mods-empty-{lang}')
            app.forced_saves = sample_saves()
            app.open_saves()
            shot(f'oot-saves-{lang}')
            app.save_asked = (app.save_list[0].path, False, app.save_list[0].name)
            app.save_note = (NOTE, app.cat('ui.saves_confirm', name=app.save_list[0].name,
                                           path='C:\\Games\\Rekarina\\profile\\save-before-import'))
            app.refresh_saves(rescan=False)
            shot(f'oot-saves-ask-{lang}')
            app.forced_saves = ([], None)
            app.save_asked, app.save_note = None, None
            app.refresh_saves()
            shot(f'oot-saves-empty-{lang}')
            for name, events in (('update', SAMPLE_UPDATE), ('run', SAMPLE_RUN), ('components', SAMPLE_COMPONENTS),
                                 ('ok', SAMPLE_OK), ('failed', SAMPLE_FAIL)):
                play(app, events)
                app.job_title = ('ui.oot_updating', {'version': '1.0.1'}) if name == 'update' else None
                if name in ('update', 'run', 'components'):
                    app.build_step_rows(app.model.steps)
                    app.show('run')
                    app.refresh_run()
                else:
                    app.model.finish(0 if name == 'ok' else 1)
                    app.refresh_finish()
                    app.show('finish')
                shot(f'oot-{name}-{lang}')
            for w in root.winfo_children():
                w.destroy()
        root.destroy()

    root.after(50, run_all)
    root.mainloop()
    print(f'gui_oot selftest: {len(drawn)} screens drawn: {", ".join(drawn)}')
    return 0 if len(drawn) == 2 * len(SCREENS) else 1


def fit_small_screen():
    """Windows: the window is a fixed picture of STAGE_W x STAGE_H, which Windows enlarges by the display scale for a
    program that does not say it handles it (pythonw does not). On a handheld's 1920x1080 screen at its usual 150 %
    that is 1680x1050 plus the title bar: taller than the space above the taskbar, so the bottom was cut off. When the
    enlarged window would not fit, the process says it handles the scale itself and the window keeps its own pixels
    (smaller on screen, whole). Before the first window; nothing changes where it fits."""
    if os.name != 'nt':
        return
    try:
        import ctypes
        from ctypes import wintypes
        rect = wintypes.RECT()
        if not ctypes.windll.user32.SystemParametersInfoW(0x0030, 0, ctypes.byref(rect), 0):   # SPI_GETWORKAREA
            return
        if rect.bottom - rect.top >= STAGE_H + 40 and rect.right - rect.left >= STAGE_W:   # 40: the title bar, roughly
            return
        ctypes.windll.shcore.SetProcessDpiAwareness(1)   # PROCESS_SYSTEM_DPI_AWARE
    except (AttributeError, OSError):
        pass


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--selftest', action='store_true')
    ap.add_argument('--shots', type=Path)
    ap.add_argument('--lang', choices=UI_LANGUAGES)
    ap.add_argument('--root', type=Path)
    ap.add_argument('--game', type=Path)
    ap.add_argument('--dest', type=Path)
    ap.add_argument('--toolchain', type=Path)
    ap.add_argument('--after-update', action='store_true')
    ap.add_argument('--rebuild', type=Path)
    args = ap.parse_args(argv)
    layout = oot.find_layout(args.root, args.toolchain)
    if args.selftest:
        return selftest(layout, args.shots)
    fix_tcl_paths()
    fit_small_screen()
    root = tk.Tk()
    RekarinaApp(root, args.lang or windows_ui_language(), layout, args)
    root.mainloop()
    return 0


if __name__ == '__main__':
    sys.exit(main())
