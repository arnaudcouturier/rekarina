// Win32 adapter of the in-game menu: window messages to menu events (menu.h). The only Win32 code of the menu; the SDL3
// window has its own (ui/menu_sdl.cpp). Window thread.
//
// F1 opens and closes the menu, Esc opens it (with the menu open, Esc goes to the menu, which closes). Esc also releases
// the cursor (input_window_msg runs first; closing the menu gives the capture back). While the menu is open, its keys,
// the mouse and focus changes go to it; the game polls the keyboard on its own (input_win.c) and gets nothing while the
// menu is open (input_menu).
#include <windows.h>
#include <windowsx.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "imgui.h"

extern "C" {
#include "cfg.h"
#include "input.h"
#include "input_win.h"
#include "ui/menu.h"
void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
}

static ImGuiKey key_of(WPARAM vk) {
    switch (vk) {
    case VK_UP: return ImGuiKey_UpArrow;
    case VK_DOWN: return ImGuiKey_DownArrow;
    case VK_LEFT: return ImGuiKey_LeftArrow;
    case VK_RIGHT: return ImGuiKey_RightArrow;
    case VK_RETURN: return ImGuiKey_Enter;
    case VK_SPACE: return ImGuiKey_Space;
    case VK_ESCAPE: return ImGuiKey_Escape;
    case VK_TAB: return ImGuiKey_Tab;
    case VK_BACK: return ImGuiKey_Backspace;
    case VK_DELETE: return ImGuiKey_Delete;   // clears a cell of the remapping rows
    case VK_HOME: return ImGuiKey_Home;
    case VK_END: return ImGuiKey_End;
    case VK_PRIOR: return ImGuiKey_PageUp;
    case VK_NEXT: return ImGuiKey_PageDown;
    case VK_SHIFT: return ImGuiKey_LeftShift;
    case VK_CONTROL: return ImGuiKey_LeftCtrl;
    case 'Q': return ImGuiKey_Q;   // previous tab
    case 'E': return ImGuiKey_E;   // next tab
    default: return ImGuiKey_None;
    }
}

// The window the messages come to, for menu_window_client (window thread writes, the menu reads a whole handle).
static HWND g_menu_hwnd;

// DEBUG-DPIVIRT: REKARINA_DEBUG_DPI_VIRT=<scale> (1.25, 1.5, ...) hands the menu the mouse positions and the client size
// divided by that scale, as Windows does for a window it scales for DPI (a DPI-unaware process on a monitor at 125 or
// 150 %), while the swapchain keeps its pixels. Read once.
static float dpi_virt(void) {
    static float v = -1;
    if (v < 0) {
        const char *s = cfg_get("REKARINA_DEBUG_DPI_VIRT");
        float f = s ? (float)atof(s) : 1.0f;
        v = f > 1.0f && f <= 4.0f ? f : 1.0f;
        if (v > 1.0f) rt_log("menu: DEBUG-DPIVIRT: mouse positions and the client size reach the menu divided by %.2f", v);
    }
    return v;
}

// The client area in the space the mouse positions are measured in.
static int client_size(HWND hw, int *w, int *h) {
    RECT r;
    if (!hw || !GetClientRect(hw, &r)) return 0;
    float v = dpi_virt();
    *w = (int)lroundf((float)(r.right - r.left) / v), *h = (int)lroundf((float)(r.bottom - r.top) / v);
    return 1;
}

// DEBUG-MENUCLIENT: the client area the mouse positions are measured in, for the menu to compare with its own size.
extern "C" int menu_window_client(int *w, int *h) { return client_size(g_menu_hwnd, w, h); }

// A mouse position of a message, in client pixels, to the menu with the size of the client area it was measured in:
// the menu maps it to its own drawing size (menu.cpp pointer_to_menu), which is the swapchain's.
static void pointer(HWND h, LPARAM l) {
    int w, hh;
    if (!client_size(h, &w, &hh)) return;
    float v = dpi_virt();
    menu_event_pointer((float)GET_X_LPARAM(l) / v, (float)GET_Y_LPARAM(l) / v, w, hh);
}

// DEBUG-MENUCLIENT: a click whose message names another place than the cursor's, while the cursor is over the client
// area, or a cursor that Windows reports apart from its physical place (the window is scaled for DPI): logged, the
// first few of a session. A click posted by a test, with the cursor elsewhere or over a window in front of this one,
// says nothing.
static void click_check(HWND h, LPARAM l) {
    static int said;
    POINT c, p;
    RECT r;
    if (said >= 8 || !GetCursorPos(&c) || WindowFromPoint(c) != h || !GetClientRect(h, &r)) return;
    if (!GetPhysicalCursorPos(&p)) p = c;
    POINT in = c;
    if (!ScreenToClient(h, &in) || !PtInRect(&r, in)) return;
    int mx = GET_X_LPARAM(l), my = GET_Y_LPARAM(l);
    if (abs(in.x - mx) > 2 || abs(in.y - my) > 2 || p.x != c.x || p.y != c.y) {
        said++;
        rt_log("menu: a click's message is at %d,%d in the client area, the cursor at %ld,%ld (screen %ld,%ld, physical "
               "%ld,%ld)", mx, my, in.x, in.y, c.x, c.y, p.x, p.y);
    }
}

// DEBUG-MENUCLIENT: the window's DPI (96 at 100 %), 0 when unknown.
extern "C" unsigned menu_window_dpi(void) {
    HWND hw = g_menu_hwnd;
    return hw ? GetDpiForWindow(hw) : 0;
}

// DEBUG-MENUCLIENT: where Windows says the pointer and the window are, for the menu's opening line. A pointer that the
// menu takes rows away from where the player sees it has a reason in these numbers: a process Windows scales for DPI
// (a compatibility override of the exe) reports the cursor apart from its physical place, a window off its monitor or
// a client area that is not the swapchain's size moves or scales the pointer.
extern "C" int menu_window_pointer(char *out, int n) {
    HWND hw = g_menu_hwnd;
    POINT c, p, org = {0, 0};
    RECT wr;
    MONITORINFO mi = {sizeof mi};
    if (!hw || !GetCursorPos(&c) || !ClientToScreen(hw, &org) || !GetWindowRect(hw, &wr) ||
        !GetMonitorInfoW(MonitorFromWindow(hw, MONITOR_DEFAULTTONEAREST), &mi))
        return 0;
    if (!GetPhysicalCursorPos(&p)) p = c;
    static const char *const aware[] = {"unaware", "system aware", "per-monitor aware"};
    int a = (int)GetAwarenessFromDpiAwarenessContext(GetWindowDpiAwarenessContext(hw));
    return snprintf(out, (size_t)n,
                    "pointer at %ld,%ld in the client (physical cursor %ld,%ld on the screen), client origin %ld,%ld, "
                    "window %ld,%ld %ldx%ld, monitor %ld,%ld %ldx%ld, the window is %s",
                    c.x - org.x, c.y - org.y, p.x, p.y, org.x, org.y, wr.left, wr.top, wr.right - wr.left,
                    wr.bottom - wr.top, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                    mi.rcMonitor.bottom - mi.rcMonitor.top, a >= 0 && a <= 2 ? aware[a] : "of unknown DPI awareness");
}

// DEBUG (the menu script's winclick): a left click at a fraction of the client area, posted to the window as the mouse
// would send it, so it takes the pointer's own path to the menu. The point goes through the screen and back
// (ClientToScreen, ScreenToClient), as a cursor's does for a window anywhere on the desktop.
extern "C" int menu_window_post_click(float fx, float fy) {
    RECT r;
    HWND hw = g_menu_hwnd;
    if (!hw || !GetClientRect(hw, &r)) return 0;
    POINT pt = {(LONG)(fx * (float)(r.right - r.left)), (LONG)(fy * (float)(r.bottom - r.top))};
    if (!ClientToScreen(hw, &pt) || !ScreenToClient(hw, &pt)) return 0;
    LPARAM at = MAKELPARAM((WORD)(SHORT)pt.x, (WORD)(SHORT)pt.y);
    return PostMessageW(hw, WM_MOUSEMOVE, 0, at) && PostMessageW(hw, WM_LBUTTONDOWN, MK_LBUTTON, at) &&
           PostMessageW(hw, WM_LBUTTONUP, 0, at);
}

// DEBUG (the menu script's winpos, winsize, winkey): the window moved (its outer top-left corner, which may be off every
// monitor or negative, as on a second monitor left of or above the first), its client area resized, or a key sent as
// the keyboard would (F11: borderless over the monitor and back). Done by the window thread (WM_APP_MENU_CMD).
enum { WM_APP_MENU_CMD = WM_APP + 0x40, CMD_POS = 1, CMD_SIZE, CMD_KEY };
extern "C" int menu_window_command(int op, int a, int b) {
    HWND hw = g_menu_hwnd;
    if (!hw) return 0;
    if (op == CMD_KEY) return PostMessageW(hw, WM_KEYDOWN, (WPARAM)a, 1) && PostMessageW(hw, WM_KEYUP, (WPARAM)a, 0xC0000001);
    return PostMessageW(hw, WM_APP_MENU_CMD, (WPARAM)op, MAKELPARAM((WORD)(SHORT)a, (WORD)(SHORT)b));
}

static void window_command(HWND h, int op, int a, int b) {
    if (op == CMD_POS) {
        SetWindowPos(h, NULL, a, b, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    } else if (op == CMD_SIZE) {
        RECT r = {0, 0, a, b};
        AdjustWindowRectExForDpi(&r, (DWORD)GetWindowLongW(h, GWL_STYLE), FALSE, (DWORD)GetWindowLongW(h, GWL_EXSTYLE),
                                 GetDpiForWindow(h));
        SetWindowPos(h, NULL, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    RECT wr, cr;
    if (GetWindowRect(h, &wr) && GetClientRect(h, &cr))
        rt_log("menu: script window command %d (%d, %d): window %ld,%ld %ldx%ld, client area %ldx%ld", op, a, b, wr.left,
               wr.top, wr.right - wr.left, wr.bottom - wr.top, cr.right, cr.bottom);
}

extern "C" int menu_window_msg(HWND h, UINT m, WPARAM w, LPARAM l, LRESULT *res) {
    g_menu_hwnd = h;
    switch (m) {
    case WM_APP_MENU_CMD:
        window_command(h, (int)w, GET_X_LPARAM(l), GET_Y_LPARAM(l));
        *res = 0;
        return 1;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        // bit 30: auto-repeat
        // Esc with the menu open (opened by F1 too) is not a toggle here: it goes to the menu, which closes itself
        if (!(l & (1 << 30)) && (w == VK_F1 || (w == VK_ESCAPE && !menu_is_open()))) {
            menu_toggle();
            *res = 0;
            return 1;
        }
        if (w == VK_F10) {   // also keeps F10 from opening the window menu
            if (!(l & (1 << 30))) menu_perf_cycle();
            *res = 0;
            return 1;
        }
        [[fallthrough]];
    case WM_KEYUP:
    case WM_SYSKEYUP:
        if (!menu_is_open()) return 0;
        if (ImGuiKey k = key_of(w)) menu_event_key(k, m == WM_KEYDOWN || m == WM_SYSKEYDOWN);
        return 0;   // F11 and the system keys keep working
    case WM_MOUSEMOVE:
        if (menu_is_open()) pointer(h, l);
        return 0;
    case WM_LBUTTONDOWN: case WM_LBUTTONUP:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: {
        if (!menu_is_open()) return 0;
        int b = m == WM_LBUTTONDOWN || m == WM_LBUTTONUP ? 0 : m == WM_RBUTTONDOWN || m == WM_RBUTTONUP ? 1 : 2;
        int down = m == WM_LBUTTONDOWN || m == WM_RBUTTONDOWN || m == WM_MBUTTONDOWN;
        if (down) click_check(h, l);
        pointer(h, l);
        menu_event_mouse_button(b, down);
        return 0;
    }
    case WM_MOUSEWHEEL:
        if (menu_is_open()) menu_event_wheel(0, (float)GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA);
        return 0;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        menu_event_focus(m == WM_SETFOCUS);
        return 0;
    }
    return 0;
}
