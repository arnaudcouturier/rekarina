// The Win32 back end of the window (gfx_wsys.h): the window and its message loop on a thread of their own, F11
// fullscreen, the settings REKARINA_WINDOW_SIZE and REKARINA_FULLSCREEN, the monitor's refresh rate, focus, the Vulkan surface.
// The swapchain and the present are gfx_present.c's. The in-game menu's keyboard and mouse come from the window
// procedure (ui/menu_win.cpp), and so do the game's (input_win.c).
//
// The process is per-monitor DPI aware (rekarina.manifest): every size here is physical pixels, REKARINA_WINDOW_SIZE included,
// and the swapchain matches the screen pixel for pixel. A window moved to a monitor at another display scale keeps its
// client size (WM_GETDPISCALEDSIZE); the menu takes its text size floor from the window's DPI (gfx_window_dpi).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <imm.h>

#include "cfg.h"
#include "flight.h"
#include "fs.h"
#include "gfx.h"
#include "gfx_wsys.h"
#include "input.h"
#include "input_win.h"
#include "nvn_api.h"
#include "ui/menu.h"
#include "vk.h"
#include "host.h"
#include "platform/platform.h"
#include <vulkan/vulkan_win32.h>
#include "vk_dlssg.h"

// ---- window state
static HWND g_hwnd;
static PlatEvent g_wnd_ready;   // manual-reset: the window thread made the window or gave up
static int g_wnd_failed;
static int g_background;                   // REKARINA_BACKGROUND_WINDOW: the window stays behind the user's windows
static int g_fs_pending;                   // window thread: REKARINA_FULLSCREEN waits for the first activation
static volatile int32_t g_dpi = USER_DEFAULT_SCREEN_DPI;   // the window's DPI, set by the window thread

HWND gfx_window_handle(void) { return g_hwnd; }

int gfx_window_vk_extensions(const char **out, int max) {
    if (max < 2) return 0;
    out[0] = VK_KHR_SURFACE_EXTENSION_NAME;
    out[1] = VK_KHR_WIN32_SURFACE_EXTENSION_NAME;
    return 2;
}

int gfx_window_vk_presentable(VkInstance inst, VkPhysicalDevice pd, uint32_t family) {
    (void)inst;
    return vkGetPhysicalDeviceWin32PresentationSupportKHR(pd, family) != VK_FALSE;
}

// Full-screen exclusive disallowed (VK_EXT_full_screen_exclusive, enabled by vk_device.c). Our fullscreen is a borderless
// window over the monitor, and the default lets a driver take the display for the window: NVIDIA's own Vulkan present
// (FIFO and immediate; mailbox goes through a DXGI swap chain) and AMD's do. Taking it switches the display (the screen
// flashes when the present mode changes in the menu), leaves Windows' colour handling behind (with HDR or automatic
// colour management on, the SDR picture shows oversaturated) and changes Alt+Tab. REKARINA_VK_FSE=0: the driver's default.
const void *gfx_window_swapchain_next(int has_fse) {
    static const VkSurfaceFullScreenExclusiveInfoEXT fse = {.sType = VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT,
                                                            .fullScreenExclusive = VK_FULL_SCREEN_EXCLUSIVE_DISALLOWED_EXT};
    return has_fse ? &fse : NULL;
}

// ---- window thread
#define WND_STYLE WS_OVERLAPPEDWINDOW
#define WM_APP_SETTINGS (WM_APP + 1)
#define WM_APP_FULLSCREEN (WM_APP + 2)   // the fullscreen setting, held back until the window is first activated

static WINDOWPLACEMENT g_saved_place = {sizeof g_saved_place};   // the window behind fullscreen (window thread)

static int is_fullscreen(HWND h) { return !(GetWindowLongW(h, GWL_STYLE) & WS_OVERLAPPEDWINDOW); }

#define cfg_fullscreen gfx_cfg_fullscreen
#define cfg_window_size gfx_cfg_window_size

unsigned gfx_window_dpi(void) { return (unsigned)plat_cas32(&g_dpi, 0, 0); }

// The outer size of a framed window whose client area is cw x ch. The frame is measured at the window's DPI, or the
// system DPI before the window exists: plain AdjustWindowRect always takes the system DPI, which is wrong on a monitor
// at another display scale.
static void outer_size(HWND h, int cw, int ch, int *ow, int *oh) {
    RECT r = {0, 0, cw, ch};
    AdjustWindowRectExForDpi(&r, WND_STYLE, FALSE, 0, h ? GetDpiForWindow(h) : GetDpiForSystem());
    *ow = r.right - r.left;
    *oh = r.bottom - r.top;
}

// Windows turns the display off and sleeps after the idle timeout, and gamepad input does not count as activity. While
// the window is up and not minimized it holds the display and the system on, as other games do. The state belongs to
// the calling thread, so only the window thread (window_main and wnd_proc) calls this; the process exit releases it.
static void keep_awake(int on) {
    static int cur = -1;
    if (on == cur) return;
    cur = on;
    SetThreadExecutionState(on ? ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED : ES_CONTINUOUS);
}

// F11: borderless window over the whole monitor, and back to the saved window. The swapchain follows through WM_SIZE.
// Not Alt+Enter: Enter is the Plus button (input_win.c). A background window (test runs) keeps its place behind the
// user's windows and takes no focus.
static void toggle_fullscreen(HWND h) {
    LONG style = GetWindowLongW(h, GWL_STYLE);
    if (style & WS_OVERLAPPEDWINDOW) {
        MONITORINFO mi = {sizeof mi};
        if (!GetWindowPlacement(h, &g_saved_place) || !GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTOPRIMARY), &mi))
            return;
        SetWindowLongW(h, GWL_STYLE, (style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED | (g_background ? SWP_NOZORDER | SWP_NOACTIVATE : 0));
    } else {
        SetWindowLongW(h, GWL_STYLE, (style & ~WS_POPUP) | WND_STYLE);
        if (g_background) g_saved_place.showCmd = SW_SHOWNOACTIVATE;
        SetWindowPlacement(h, &g_saved_place);
        SetWindowPos(h, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED |
                         (g_background ? SWP_NOACTIVATE : 0));
    }
}

// The window size and fullscreen state last taken from the settings (window thread). apply_settings acts on a change of
// either value only, so a window resized by hand keeps its size when another setting changes.
static char g_applied_size[32];
static int g_applied_fs;

static void note_applied(void) {
    snprintf(g_applied_size, sizeof g_applied_size, "%s", cfg_value("REKARINA_WINDOW_SIZE"));
    g_applied_fs = cfg_fullscreen();
}

// Window thread: the window takes the size and fullscreen state of the settings when they changed. In fullscreen the
// size goes into the placement restored when fullscreen ends.
static void apply_settings(HWND h) {
    int size_changed = strcmp(cfg_value("REKARINA_WINDOW_SIZE"), g_applied_size) != 0, fs = cfg_fullscreen(), fs_changed = fs != g_applied_fs;
    note_applied();
    if (size_changed) {
        int cw, ch, ow, oh;
        cfg_window_size(&cw, &ch);
        outer_size(h, cw, ch, &ow, &oh);
        if (is_fullscreen(h)) {
            RECT *n = &g_saved_place.rcNormalPosition;
            n->right = n->left + ow;
            n->bottom = n->top + oh;
        } else {
            RECT have;
            if (GetClientRect(h, &have) && (have.right != cw || have.bottom != ch)) {
                if (IsZoomed(h)) ShowWindow(h, SW_RESTORE);
                SetWindowPos(h, NULL, 0, 0, ow, oh, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
        }
    }
    if (fs_changed && fs != is_fullscreen(h)) {
        input_cursor_suspend(1);
        toggle_fullscreen(h);
        input_cursor_suspend(0);
    }
}

void gfx_window_apply_settings(void) {
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP_SETTINGS, 0, 0);
}


void gfx_window_request_close(void) {
    if (g_hwnd) PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
}

// ---- refresh rate of the window's monitor (REKARINA_FPS_LIMIT=display), measured when the window opens, lands on another
// monitor or the display mode changes (gfx_window_set_refresh keeps it). QueryDisplayConfig gives the exact rational
// rate (59.94, 143.998); EnumDisplaySettings' whole hertz is the fallback.
static HMONITOR g_rate_mon;

static double monitor_rate_hz(HMONITOR mon) {
    MONITORINFOEXW mi = {.cbSize = sizeof mi};
    if (!GetMonitorInfoW(mon, (MONITORINFO *)&mi)) return 0;
    double hz = 0;
    UINT32 np = 0, nm = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) == ERROR_SUCCESS && np && nm) {
        DISPLAYCONFIG_PATH_INFO *paths = calloc(np, sizeof *paths);
        DISPLAYCONFIG_MODE_INFO *modes = calloc(nm, sizeof *modes);
        if (paths && modes && QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths, &nm, modes, NULL) == ERROR_SUCCESS)
            for (UINT32 i = 0; i < np && !hz; i++) {
                DISPLAYCONFIG_SOURCE_DEVICE_NAME src = {.header = {.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME,
                                                                   .size = sizeof src,
                                                                   .adapterId = paths[i].sourceInfo.adapterId,
                                                                   .id = paths[i].sourceInfo.id}};
                DISPLAYCONFIG_RATIONAL r = paths[i].targetInfo.refreshRate;
                if (DisplayConfigGetDeviceInfo(&src.header) == ERROR_SUCCESS && !wcscmp(src.viewGdiDeviceName, mi.szDevice) &&
                    r.Denominator)
                    hz = (double)r.Numerator / r.Denominator;
            }
        free(paths);
        free(modes);
    }
    DEVMODEW dm = {.dmSize = sizeof dm};
    if (!hz && EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
        hz = dm.dmDisplayFrequency;   // 0 and 1 mean the hardware default
    return hz;
}

// Windows' HDR ("Use HDR", advanced colour) on the display the window is on: an SDR swapchain there is composed into
// HDR with Windows' SDR content brightness, which makes the picture pale and milky next to the same game with HDR off.
// Logged when it changes, with what to do, while REKARINA_HDR is off: a player's "washed-out colours" then shows in the
// log. -1 unknown, 0 off, 1 on.
static int monitor_hdr_on(HMONITOR mon) {
    MONITORINFOEXW mi = {.cbSize = sizeof mi};
    if (!GetMonitorInfoW(mon, (MONITORINFO *)&mi)) return -1;
    UINT32 np = 0, nm = 0;
    int on = -1;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS || !np || !nm) return -1;
    DISPLAYCONFIG_PATH_INFO *paths = calloc(np, sizeof *paths);
    DISPLAYCONFIG_MODE_INFO *modes = calloc(nm, sizeof *modes);
    if (paths && modes && QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths, &nm, modes, NULL) == ERROR_SUCCESS)
        for (UINT32 i = 0; i < np && on < 0; i++) {
            DISPLAYCONFIG_SOURCE_DEVICE_NAME src = {.header = {.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME,
                                                               .size = sizeof src,
                                                               .adapterId = paths[i].sourceInfo.adapterId,
                                                               .id = paths[i].sourceInfo.id}};
            if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS || wcscmp(src.viewGdiDeviceName, mi.szDevice))
                continue;
            DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO ac = {.header = {.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO,
                                                                   .size = sizeof ac,
                                                                   .adapterId = paths[i].targetInfo.adapterId,
                                                                   .id = paths[i].targetInfo.id}};
            if (DisplayConfigGetDeviceInfo(&ac.header) == ERROR_SUCCESS) on = ac.advancedColorEnabled ? 1 : 0;
        }
    free(paths);
    free(modes);
    return on;
}

static void hdr_mode_note(HMONITOR mon) {
    static int last = -2;
    int on = monitor_hdr_on(mon);
    if (on == last) return;
    last = on;
    if (on == 1 && !cfg_bool("REKARINA_HDR"))
        rt_log("gfx: Windows has HDR on for this display and the game's picture is SDR: Windows shows it with its SDR "
               "content brightness, which looks pale or washed out; HDR in the Screen tab, or \"Use HDR\" off in Windows' "
               "display settings, avoids that");
    else if (on >= 0)
        rt_log("gfx: Windows HDR is %s for this display", on ? "on" : "off");
}

// Window thread. force: the display mode changed, measure even on the same monitor.
static void refresh_rate_update(HWND h, int force) {
    HMONITOR mon = MonitorFromWindow(h, MONITOR_DEFAULTTOPRIMARY);
    if (!force && mon == g_rate_mon) return;
    g_rate_mon = mon;
    gfx_window_set_refresh(monitor_rate_hz(mon));
    hdr_mode_note(mon);
}

int gfx_window_monitor_size(int *w, int *h) {
    MONITORINFO mi = {sizeof mi};
    if (!g_hwnd || !GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY), &mi)) return 0;
    *w = mi.rcMonitor.right - mi.rcMonitor.left;
    *h = mi.rcMonitor.bottom - mi.rcMonitor.top;
    return 1;
}

static int g_presented;   // the first frame reached the swapchain (gfx_window_presented)
void gfx_window_presented(void) { __atomic_store_n(&g_presented, 1, __ATOMIC_RELEASE); }

static LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    LRESULT input_res;
    if (input_window_msg(h, m, w, l, &input_res)) return input_res;
    if (menu_window_msg(h, m, w, l, &input_res)) return input_res;
    switch (m) {
    case WM_KEYDOWN:
        if (w == VK_F11 && !(l & (1 << 30))) {   // bit 30: auto-repeat
            input_cursor_suspend(1);
            toggle_fullscreen(h);
            input_cursor_suspend(0);
            cfg_set("REKARINA_FULLSCREEN", is_fullscreen(h) ? "1" : "0");   // the menu shows it; the next start keeps it
            cfg_save();
            note_applied();   // what cfg_get reads now (the environment may still override it)
        }
        break;
    case WM_APP_SETTINGS:
        apply_settings(h);
        return 0;
    case WM_APP_FULLSCREEN:
        if (cfg_fullscreen() && !is_fullscreen(h)) {
            input_cursor_suspend(1);
            toggle_fullscreen(h);
            input_cursor_suspend(0);
        }
        return 0;
    case WM_SIZE:
        keep_awake(w != SIZE_MINIMIZED);
        if (w != SIZE_MINIMIZED) gfx_window_mark_dirty();
        refresh_rate_update(h, 0);
        return 0;
    case WM_EXITSIZEMOVE:   // dragged onto another monitor
        refresh_rate_update(h, 0);
        break;
    case WM_DISPLAYCHANGE:   // a resolution, layout or HDR change of the desktop
        refresh_rate_update(h, 1);
        if (is_fullscreen(h) && !IsIconic(h)) {   // keep covering the monitor at its new size (a same size sends no WM_SIZE)
            MONITORINFO mi = {sizeof mi};
            if (GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi))
                SetWindowPos(h, NULL, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                             mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        gfx_window_mark_dirty();   // the swapchain is made again: its size, and its colour space after an HDR switch
        break;
    case WM_GETDPISCALEDSIZE:   // a framed window keeps its client size in pixels on a monitor at another scale
        if (!is_fullscreen(h) && !IsZoomed(h)) {
            RECT c, r;
            if (GetClientRect(h, &c)) {
                r = (RECT){0, 0, c.right, c.bottom};
                if (AdjustWindowRectExForDpi(&r, WND_STYLE, FALSE, 0, LOWORD(w))) {
                    *(SIZE *)l = (SIZE){r.right - r.left, r.bottom - r.top};
                    return TRUE;
                }
            }
        }
        break;   // DefWindowProc: Windows scales the size with the DPI
    case WM_DPICHANGED: {   // display scale changed, or the window reached a monitor at another scale
        const RECT *s = (const RECT *)l;
        plat_xchg32(&g_dpi, LOWORD(w));
        rt_log("gfx: window DPI %u (%u%%)", LOWORD(w), LOWORD(w) * 100u / USER_DEFAULT_SCREEN_DPI);
        if (is_fullscreen(h)) {   // keep covering the monitor the suggested rect lies on
            MONITORINFO mi = {sizeof mi};
            if (GetMonitorInfoW(MonitorFromRect(s, MONITOR_DEFAULTTONEAREST), &mi))
                SetWindowPos(h, NULL, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                             mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOZORDER | SWP_NOACTIVATE);
        } else {
            SetWindowPos(h, NULL, s->left, s->top, s->right - s->left, s->bottom - s->top, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;
    }
    case WM_ACTIVATEAPP: {
        int in = w != 0;
        gfx_window_focus_changed(in);
        if (in && g_fs_pending) {
            g_fs_pending = 0;
            PostMessageW(h, WM_APP_FULLSCREEN, 0, 0);
        }
        return 0;
    }
    case WM_CLOSE:
        gfx_window_closed();
        return 1;
    case WM_ERASEBKGND:
        if (!__atomic_load_n(&g_presented, __ATOMIC_ACQUIRE)) {   // black, not the white of an unpainted window, until the swapchain shows
            RECT rc;
            GetClientRect(h, &rc);
            FillRect((HDC)w, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
        }
        return 1;
    }
    return DefWindowProcW(h, m, w, l);
}

// Brings the game window to the front. Windows refuses SetForegroundWindow to a process that was not the last to get
// input, which is the case for a game started by a frontend; attaching this thread's input queue to the foreground
// window's thread for the call is the documented way around it. The window is not left topmost.
static void raise_window(HWND h) {
    HWND fg = GetForegroundWindow();
    DWORD me = GetCurrentThreadId(), other = fg ? GetWindowThreadProcessId(fg, NULL) : 0;
    int attached = other && other != me && AttachThreadInput(me, other, TRUE);
    ShowWindow(h, IsIconic(h) ? SW_RESTORE : SW_SHOW);
    BringWindowToTop(h);
    SetForegroundWindow(h);
    SetActiveWindow(h);
    if (attached) AttachThreadInput(me, other, FALSE);
    if (GetForegroundWindow() != h) {   // still refused: a topmost toggle puts it above the others without keeping it there
        SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        SetWindowPos(h, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    }
}

static void window_main(void *arg) {
    (void)arg;
    HINSTANCE inst = GetModuleHandleW(NULL);
    // Icon resource 1 of rekarina.rc (runtime/totk.ico): the title bar, the taskbar button and Alt+Tab.
    WNDCLASSEXW wc = {.cbSize = sizeof wc, .lpfnWndProc = wnd_proc, .hInstance = inst,
                      .hIcon = LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                          GetSystemMetrics(SM_CYICON), LR_SHARED),
                      .hIconSm = LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                            GetSystemMetrics(SM_CYSMICON), LR_SHARED),
                      .hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW), .hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH),
                      .lpszClassName = L"TotKRecompWindow"};
    RegisterClassExW(&wc);
    int cw, ch, ow, oh;
    cfg_window_size(&cw, &ch);
    outer_size(NULL, cw, ch, &ow, &oh);
    // The window opens behind the user's windows first and is then raised (raise_window): a game started by a frontend
    // has no other way to reach it. The game counts itself in focus until the first WM_ACTIVATEAPP, so loading goes on,
    // and keyboard input waits for the foreground (input_win.c). If Windows still refuses the foreground, the taskbar
    // button flashes, and REKARINA_FULLSCREEN waits for the first activation.
    // REKARINA_BACKGROUND_WINDOW=1 (test runs): no flashing, never fullscreen at start, and F11 keeps the window's place.
    int background = g_background = cfg_get("REKARINA_BACKGROUND_WINDOW") && atoi(cfg_get("REKARINA_BACKGROUND_WINDOW"));
    {   // DEBUG-DPIAWARE: REKARINA_DEBUG_DPI_AWARENESS=unaware|system makes the window with that DPI awareness instead of
        // the manifest's per-monitor one (a window takes its thread's at creation), as a compatibility override of the
        // exe would; on a monitor at another scale than 100 % Windows then scales its client area and mouse positions.
        const char *aw = cfg_get("REKARINA_DEBUG_DPI_AWARENESS");
        DPI_AWARENESS_CONTEXT ctx = !aw ? NULL : !strcmp(aw, "unaware") ? DPI_AWARENESS_CONTEXT_UNAWARE
                                               : !strcmp(aw, "system") ? DPI_AWARENESS_CONTEXT_SYSTEM_AWARE : NULL;
        if (ctx && SetThreadDpiAwarenessContext(ctx)) rt_log("gfx: DEBUG-DPIAWARE: the window is made %s", aw);
    }
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"" RT_WINDOW_TITLE, WND_STYLE, CW_USEDEFAULT, CW_USEDEFAULT, ow, oh, NULL,
                             NULL, inst, NULL);
    if (!g_hwnd) {
        rt_log("gfx: CreateWindowEx failed (%lu)", GetLastError());
        g_wnd_failed = 1;
    } else {
        // An IME switched on over the game attaches to this window and then shows its candidate window and eats the keys
        // it composes, although there is nothing here to type into (a Chinese IME takes Shift, the B button by default).
        // Detaching the game's window leaves the software keyboard alone: its edit box sits in a window of its own
        // (swkbd_win.c) and gets the IME it needs.
        ImmAssociateContext(g_hwnd, NULL);
        // The frame was measured at the system DPI; the window may have opened on a monitor at another scale.
        plat_xchg32(&g_dpi, (int32_t)GetDpiForWindow(g_hwnd));
        outer_size(g_hwnd, cw, ch, &ow, &oh);
        SetWindowPos(g_hwnd, HWND_BOTTOM, 0, 0, ow, oh, SWP_NOMOVE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        if (!background) {
            // set before the window is raised: when Windows lets it take the foreground at once, the activation
            // arrives inside raise_window, and a flag set after it waited for an activation that had already come
            // (the game then stayed a window under fullscreen = 1)
            g_fs_pending = cfg_fullscreen();
            raise_window(g_hwnd);   // a frontend's player may have no way to reach a window left behind
            FLASHWINFO fw = {.cbSize = sizeof fw, .hwnd = g_hwnd, .dwFlags = FLASHW_TRAY | FLASHW_TIMERNOFG};
            FlashWindowEx(&fw);
        }
        note_applied();
        refresh_rate_update(g_hwnd, 1);
        keep_awake(1);
    }
    plat_event_set(&g_wnd_ready);
    if (g_wnd_failed) return;
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return;
}

// ---- gfx_wsys.h
// Makes the window on its thread and waits until it is there.
int wsys_open(void) {
    g_wnd_failed = 0;
    plat_event_init(&g_wnd_ready, 1, 0);
    if (!plat_thread_spawn(window_main, NULL, "window", 0)) rt_fatal("gfx: cannot start the window thread");
    plat_event_wait(&g_wnd_ready);
    return !g_wnd_failed;
}

VkSurfaceKHR wsys_surface(VkInstance inst) {
    VkWin32SurfaceCreateInfoKHR si = {.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
                                      .hinstance = GetModuleHandleW(NULL), .hwnd = g_hwnd};
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    // through Streamline when DLSS Frame Generation is loaded: it maps swapchains to windows from this call
    PFN_vkCreateWin32SurfaceKHR create = (PFN_vkCreateWin32SurfaceKHR)vk_dlssg_instance_proc(inst, "vkCreateWin32SurfaceKHR");
    VkResult r = (create ? create : vkCreateWin32SurfaceKHR)(inst, &si, NULL, &surface);
    if (r != VK_SUCCESS) rt_log("gfx: vkCreateWin32SurfaceKHR failed (%d)", (int)r);
    return r == VK_SUCCESS ? surface : VK_NULL_HANDLE;
}

void wsys_client_size(int *w, int *h) {
    RECT rc = {0};
    GetClientRect(g_hwnd, &rc);
    *w = rc.right, *h = rc.bottom;
}

int wsys_minimized(void) { return IsIconic(g_hwnd) != 0; }

void wsys_resize(int cw, int ch) {
    int ow, oh;
    outer_size(g_hwnd, cw, ch, &ow, &oh);
    SetWindowPos(g_hwnd, NULL, 0, 0, ow, oh, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void wsys_probe_focus(int in) { SendMessageW(g_hwnd, WM_ACTIVATEAPP, in ? TRUE : FALSE, 0); }

void wsys_probe_escape(void) { SendMessageW(g_hwnd, WM_KEYDOWN, VK_ESCAPE, 0); }

int wsys_probe_click(int where) {
    if (where == 0) SendMessageW(g_hwnd, WM_LBUTTONDOWN, MK_LBUTTON, 0);
    else SendMessageW(g_hwnd, WM_MOUSEACTIVATE, (WPARAM)g_hwnd, MAKELPARAM(where == 1 ? HTCLIENT : HTCAPTION, WM_LBUTTONDOWN));
    return 1;
}
