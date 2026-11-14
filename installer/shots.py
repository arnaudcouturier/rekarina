"""Pictures of a Tk window that nobody sees, for the self tests of the windows (gui_oot.py --selftest --shots).

A self test used to put its window on top of everything and copy that part of the screen: the window covered what the
user had open, took the keyboard from it (a game in front lost its focus), and whatever else lay over that part of
the screen got into the picture. Here the window is there but cannot be seen or touched:

  - fully transparent, without a title bar or a taskbar button, under every other window, passing the mouse through;
  - never activated: a hook of this thread refuses the activation Tk asks for when it maps a window, so the keyboard
    stays with whatever is in front;
  - its picture is the window's own drawing of itself (PrintWindow), read back from the compositor, not from the
    screen.

Windows only (ctypes); saving a picture needs Pillow, which a checkout's Python has and a release's does not.

    root = shots.unseen_root()          # in place of tk.Tk()
    ...build and change the window...
    shots.save(root, path)              # the client area as a PNG
"""
import ctypes
import os
import time
import tkinter as tk

try:
    from PIL import Image   # only saving a picture needs it
except ImportError:
    Image = None

AVAILABLE = os.name == 'nt'
HCBT_ACTIVATE, HCBT_SETFOCUS, WH_CBT = 5, 9, 5
GWL_EXSTYLE = -20
WS_EX_LAYERED, WS_EX_TRANSPARENT, WS_EX_TOOLWINDOW, WS_EX_NOACTIVATE = 0x80000, 0x20, 0x80, 0x08000000
HWND_BOTTOM, SWP_NOSIZE_NOMOVE_NOACTIVATE = 1, 0x0013
PW_CLIENT_FROM_COMPOSITOR = 3   # PW_CLIENTONLY | PW_RENDERFULLCONTENT

if AVAILABLE:
    from ctypes import wintypes

    user32, gdi32 = ctypes.WinDLL('user32'), ctypes.WinDLL('gdi32')   # own copies: the argument types set here stay here
    HOOKPROC = ctypes.WINFUNCTYPE(ctypes.c_ssize_t, ctypes.c_int, wintypes.WPARAM, wintypes.LPARAM)
    user32.SetWindowsHookExW.restype = wintypes.HHOOK
    user32.SetWindowsHookExW.argtypes = [ctypes.c_int, HOOKPROC, wintypes.HINSTANCE, wintypes.DWORD]
    user32.CallNextHookEx.restype = ctypes.c_ssize_t
    user32.CallNextHookEx.argtypes = [wintypes.HHOOK, ctypes.c_int, wintypes.WPARAM, wintypes.LPARAM]
    user32.GetWindowLongPtrW.restype = ctypes.c_ssize_t
    user32.GetWindowLongPtrW.argtypes = [wintypes.HWND, ctypes.c_int]
    user32.SetWindowLongPtrW.restype = ctypes.c_ssize_t
    user32.SetWindowLongPtrW.argtypes = [wintypes.HWND, ctypes.c_int, ctypes.c_ssize_t]
    user32.SetWindowPos.argtypes = [wintypes.HWND, wintypes.HWND, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                    wintypes.UINT]
    user32.GetDC.restype = wintypes.HDC
    user32.GetDC.argtypes = [wintypes.HWND]
    user32.ReleaseDC.argtypes = [wintypes.HWND, wintypes.HDC]
    user32.PrintWindow.argtypes = [wintypes.HWND, wintypes.HDC, wintypes.UINT]
    user32.GetForegroundWindow.restype = wintypes.HWND
    gdi32.CreateCompatibleDC.restype = wintypes.HDC
    gdi32.CreateCompatibleDC.argtypes = [wintypes.HDC]
    gdi32.CreateCompatibleBitmap.restype = wintypes.HBITMAP
    gdi32.CreateCompatibleBitmap.argtypes = [wintypes.HDC, ctypes.c_int, ctypes.c_int]
    gdi32.SelectObject.restype = wintypes.HGDIOBJ
    gdi32.SelectObject.argtypes = [wintypes.HDC, wintypes.HGDIOBJ]
    gdi32.DeleteObject.argtypes = [wintypes.HGDIOBJ]
    gdi32.DeleteDC.argtypes = [wintypes.HDC]
    gdi32.GetDIBits.argtypes = [wintypes.HDC, wintypes.HBITMAP, wintypes.UINT, wintypes.UINT, ctypes.c_void_p,
                                ctypes.c_void_p, wintypes.UINT]

    class BITMAPINFOHEADER(ctypes.Structure):
        _fields_ = [('biSize', wintypes.DWORD), ('biWidth', wintypes.LONG), ('biHeight', wintypes.LONG),
                    ('biPlanes', wintypes.WORD), ('biBitCount', wintypes.WORD), ('biCompression', wintypes.DWORD),
                    ('biSizeImage', wintypes.DWORD), ('biXPelsPerMeter', wintypes.LONG),
                    ('biYPelsPerMeter', wintypes.LONG), ('biClrUsed', wintypes.DWORD), ('biClrImportant', wintypes.DWORD)]

    @HOOKPROC
    def refuse_activation(code, wparam, lparam):
        if code in (HCBT_ACTIVATE, HCBT_SETFOCUS):
            return 1
        return user32.CallNextHookEx(None, code, wparam, lparam)

    hooked = []   # the hook's handle, once: it lasts as long as the thread


def unseen_root():
    """A Tk root nobody sees (the module text). Every window this thread makes after the call stays inactive, so
    call it only in a process that is there to draw pictures."""
    if not hooked:
        hooked.append(user32.SetWindowsHookExW(WH_CBT, refuse_activation, None, ctypes.windll.kernel32.GetCurrentThreadId()))
    root = tk.Tk()
    root.overrideredirect(True)
    root.attributes('-alpha', 0.0)
    root.geometry('+60+40')
    return root


def frame_of(root):
    return int(root.wm_frame(), 16)


def keep_unseen(root):
    """Tk makes a window's frame anew when its style changes: give the frame its unseen state again."""
    hwnd = frame_of(root)
    style = user32.GetWindowLongPtrW(hwnd, GWL_EXSTYLE)
    wanted = style | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE
    if wanted != style:
        user32.SetWindowLongPtrW(hwnd, GWL_EXSTYLE, wanted)
    user32.SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOSIZE_NOMOVE_NOACTIVATE)


def in_front(root):
    """Whether the window has the keyboard: a self test checks that it never does."""
    return user32.GetForegroundWindow() == frame_of(root)


def picture(root):
    """The window's client area as it draws itself: a Pillow image."""
    if Image is None:
        raise SystemExit('a picture of the window needs Pillow')
    root.update_idletasks()
    keep_unseen(root)
    for _ in range(2):   # what was just drawn reaches the compositor a frame later; the picture is read from there
        root.update()
        ctypes.windll.dwmapi.DwmFlush()
        time.sleep(0.03)
    hwnd, w, h = frame_of(root), root.winfo_width(), root.winfo_height()
    screen = user32.GetDC(None)
    dc = gdi32.CreateCompatibleDC(screen)
    bitmap = gdi32.CreateCompatibleBitmap(screen, w, h)
    old = gdi32.SelectObject(dc, bitmap)
    user32.PrintWindow(hwnd, dc, PW_CLIENT_FROM_COMPOSITOR)
    head = BITMAPINFOHEADER(ctypes.sizeof(BITMAPINFOHEADER), w, -h, 1, 32, 0, 0, 0, 0, 0, 0)
    data = ctypes.create_string_buffer(w * h * 4)
    gdi32.GetDIBits(dc, bitmap, 0, h, data, ctypes.byref(head), 0)
    gdi32.SelectObject(dc, old)
    gdi32.DeleteObject(bitmap)
    gdi32.DeleteDC(dc)
    user32.ReleaseDC(None, screen)
    return Image.frombuffer('RGBA', (w, h), data, 'raw', 'BGRA', 0, 1).convert('RGB')


def save(root, path):
    picture(root).save(path)
