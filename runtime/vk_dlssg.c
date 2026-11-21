// DLSS Frame Generation (REKARINA_FRAMEGEN = dlss; applies after a restart) through rekarina_dlssg.dll
// (runtime/dlssg/nx2_sl.cpp), which drives NVIDIA Streamline. Like AMD's frame generation (vk_fg.cpp) it needs DLSS or
// FSR 3.1 upscaling on, whose depth, motion vectors and camera it hands over, and an RTX 40 or 50 GPU.
//
// The DLL and Streamline (sl.interposer.dll and its plugins, nvngx_dlssg.dll) sit next to the game. The instance and
// device are created through Streamline's proxies (vk_device.c), so it can add the extensions, features and queues
// DLSS-G needs; the present path calls Streamline's swapchain functions (gfx_present.c), and every present records the
// frame's constants and tags first (vk_dlssg_frame). Generation pauses while the menu is open and in the game's frames
// without a camera (its pause screens).
//
// DLSS-G needs Reflex, whose low-latency mode paces the game (REKARINA_REFLEX, default 1; 0 off, 2 on with boost): the
// game's frame sleeps in Reflex where it starts (its acquire of a window image), and each frame carries the latency
// markers from that start to the host present, all with the frame's number, the count of the game's presents up to it.
// The mode is the swapchain's, so it is set again after each one (vk_dlssg_create_swapchain).
#ifdef _WIN32
#include <windows.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "vk.h"
#include "vk_dlaa.h"
#include "vk_dlssg.h"
#include "vk_fg.h"
#include "platform/platform.h"
#include "report.h"
#include "dlssg/nx2_sl.h"
#include "ui/menu.h"

static struct {
    int wanted, loaded, active;
    int asked;          // REKARINA_FRAMEGEN was dlss at start (vk_dlssg_wanted)
    int retains;        // the library keeps generation's resources while it is off (interface version 4 and later)
    int avail;          // Streamline's DLSS-G plugin runs on this card (vk_dlssg_device_ready; 0 before)
    const char *why;    // why it is not running although asked, for the menu's row (vk_dlssg_off_why)
    PFN_nx2sl_init init;
    PFN_nx2sl_instance_proc gipa;
    PFN_nx2sl_device_proc gdpa;
    PFN_nx2sl_available available;
    PFN_nx2sl_frame frame;
    PFN_nx2sl_present_marker marker;
    PFN_nx2sl_sleep sleep;
    PFN_nx2sl_submit_end submit_end;
    PFN_nx2sl_shown shown;
    PFN_nx2sl_reflex reflex;
    uint32_t game;   // the game's presents (vk_dlssg_game_present)
    uint32_t cur;    // the frame the present thread blits and presents next (vk_dlssg_present_frame)
    uint32_t started;      // the last frame vk_dlssg_frame_start started
    uint64_t hosted_ns;    // kern_now_ns after the last host present
    PFN_nx2sl_shutdown shutdown;
    PFN_vkCreateSwapchainKHR create_sc;
    PFN_vkDestroySwapchainKHR destroy_sc;
    PFN_vkGetSwapchainImagesKHR images;
    PFN_vkAcquireNextImageKHR acquire;
    PFN_vkQueuePresentKHR present;
} S = {.wanted = -1};

static void sl_line(const char *line) { rt_log("%s", line); }

static int reflex_mode(void) {
    static int m = -1;
    if (m < 0) {
        const char *s = cfg_get("REKARINA_REFLEX");
        m = s && *s ? atoi(s) : 1;
        if (m < 0 || m > 2) m = 1;
    }
    return m;
}

int vk_dlssg_wanted(void) {
    if (S.wanted < 0) {
        S.asked = S.wanted = !strcmp(cfg_value("REKARINA_FRAMEGEN"), "dlss");
        if (S.wanted && fg_guard_blocked()) {   // it stopped the last start while being set up (report.c)
            S.why = FG_GUARD_WHY;
            S.wanted = 0;
        } else if (S.wanted && !vk_dlaa_wanted()) {
            rt_log("dlssg: frame generation needs DLSS or FSR 3.1 upscaling (any mode): off");
            S.why = "needs DLSS / DLAA on";
            S.wanted = 0;
        }
    }
    return S.wanted;
}

int vk_dlssg_early(void) {
    if (!vk_dlssg_wanted()) return 0;
    if (S.loaded) return S.loaded > 0;
    S.loaded = -1;
    fg_guard_arm("dlss");   // a crash from here to the first generated frames: the next start goes without (report.c)
#ifdef _WIN32
    wchar_t dir[MAX_PATH], path[MAX_PATH + 32];
    DWORD n = GetModuleFileNameW(NULL, dir, MAX_PATH);
    while (n && dir[n - 1] != L'\\') n--;
    dir[n ? n - 1 : 0] = 0;
    swprintf(path, MAX_PATH + 32, L"%ls\\%hs_dlssg.dll", dir, RT_FILE_STEM);
    HMODULE m = NULL;
    {   // DEBUG-DLSSGDLL: REKARINA_DLSSG_DLL=<path> loads our library from there (a test of a new build of it beside
        // a game folder whose own copy other builds still use); Streamline's files stay the game folder's
        const char *e = cfg_get("REKARINA_DLSSG_DLL");
        if (e && *e) {
            wchar_t w[MAX_PATH];
            if (MultiByteToWideChar(CP_UTF8, 0, e, -1, w, MAX_PATH) > 0 && (m = LoadLibraryW(w)))
                rt_log("dlssg: our library from %s (REKARINA_DLSSG_DLL)", e);
        }
    }
    if (!m) m = LoadLibraryW(path);
#ifdef RT_OLD_FILE_STEM   // a folder packaged before the file was renamed, or a build folder
    if (!m) {
        swprintf(path, MAX_PATH + 32, L"%ls\\%hs_dlssg.dll", dir, RT_OLD_FILE_STEM);
        m = LoadLibraryW(path);
    }
#endif
    if (!m) {
        rt_log("dlssg: " RT_FILE_STEM "_dlssg.dll is not next to the game: DLSS Frame Generation off");
        S.why = RT_FILE_STEM "_dlssg.dll is missing (run the setup again)";
        return 0;
    }
    PFN_nx2sl_version ver = (PFN_nx2sl_version)(void *)GetProcAddress(m, "nx2sl_version");
    S.init = (PFN_nx2sl_init)(void *)GetProcAddress(m, "nx2sl_init");
    S.gipa = (PFN_nx2sl_instance_proc)(void *)GetProcAddress(m, "nx2sl_instance_proc");
    S.gdpa = (PFN_nx2sl_device_proc)(void *)GetProcAddress(m, "nx2sl_device_proc");
    S.available = (PFN_nx2sl_available)(void *)GetProcAddress(m, "nx2sl_available");
    S.frame = (PFN_nx2sl_frame)(void *)GetProcAddress(m, "nx2sl_frame");
    S.marker = (PFN_nx2sl_present_marker)(void *)GetProcAddress(m, "nx2sl_present_marker");
    S.sleep = (PFN_nx2sl_sleep)(void *)GetProcAddress(m, "nx2sl_sleep");
    S.submit_end = (PFN_nx2sl_submit_end)(void *)GetProcAddress(m, "nx2sl_submit_end");
    S.shown = (PFN_nx2sl_shown)(void *)GetProcAddress(m, "nx2sl_shown");
    S.shutdown = (PFN_nx2sl_shutdown)(void *)GetProcAddress(m, "nx2sl_shutdown");
    S.reflex = (PFN_nx2sl_reflex)(void *)GetProcAddress(m, "nx2sl_reflex");
    int v = ver ? ver() : 0;
    S.retains = v >= 4;
    if (v < NX2SL_VERSION_OLDEST || v > NX2SL_VERSION || !S.init || !S.gipa || !S.gdpa || !S.available || !S.frame ||
        !S.marker || !S.sleep || !S.submit_end || !S.shown || !S.reflex) {
        rt_log("dlssg: " RT_FILE_STEM "_dlssg.dll is from another build: DLSS Frame Generation off");
        S.why = RT_FILE_STEM "_dlssg.dll is from another build (run the setup again)";
        return 0;
    }
    if (!S.init(dir, sl_line)) {   // the library said why (sl.interposer.dll missing, slInit failed)
        S.why = "NVIDIA Streamline did not start (the dlssg lines of the log say why)";
        return 0;
    }
    S.loaded = 1;
    return 1;
#else
    return 0;
#endif
}

VkResult vk_dlssg_create_instance(const VkInstanceCreateInfo *ci, VkInstance *out) {
    PFN_vkCreateInstance f = S.loaded > 0 ? (PFN_vkCreateInstance)S.gipa()(VK_NULL_HANDLE, "vkCreateInstance") : NULL;
    return f ? f(ci, NULL, out) : vkCreateInstance(ci, NULL, out);
}

// An instance function through the interposer while Streamline is loaded, else NULL (the caller takes the loader's).
// The window surface goes through it: Streamline learns the window a swapchain shows from vkCreateWin32SurfaceKHR, and
// without that its swapchain hook found "no window corresponding to this application" and generated no frames.
PFN_vkVoidFunction vk_dlssg_instance_proc(VkInstance inst, const char *name) {
    return S.loaded > 0 ? S.gipa()(inst, name) : NULL;
}

// The interposer's vkCreateDevice finds the device's instance in a map its own vkEnumeratePhysicalDevices fills; a
// physical device enumerated through the loader alone left it a null instance, so the plugins got an empty instance
// table and sl.common called a null GetPhysicalDeviceMemoryProperties while it set up. Enumerate through it as well.
VkResult vk_dlssg_enumerate(VkInstance inst, uint32_t *n, VkPhysicalDevice *pd) {
    PFN_vkEnumeratePhysicalDevices f =
        S.loaded > 0 ? (PFN_vkEnumeratePhysicalDevices)S.gipa()(inst, "vkEnumeratePhysicalDevices") : NULL;
    return f ? f(inst, n, pd) : vkEnumeratePhysicalDevices(inst, n, pd);
}

VkResult vk_dlssg_create_device(VkInstance inst, VkPhysicalDevice pd, const VkDeviceCreateInfo *ci, VkDevice *out) {
    PFN_vkCreateDevice f = S.loaded > 0 ? (PFN_vkCreateDevice)S.gipa()(inst, "vkCreateDevice") : NULL;
    return f ? f(pd, ci, NULL, out) : vkCreateDevice(pd, ci, NULL, out);
}

// REKARINA_DLSSG_UNAVAILABLE=1 (a test knob): DLSS Frame Generation counts as missing on this card, as on an RTX 20 or
// 30 card, so the AMD FSR frame generation that takes its place can be checked on a card that has it.
static int test_unavailable(void) {
    static int v = -1;
    if (v < 0) v = cfg_bool("REKARINA_DLSSG_UNAVAILABLE") != 0;
    return v;
}

void vk_dlssg_device_ready(VkDevice dev) {
    if (S.loaded <= 0) {   // asked for, and our library or Streamline did not load: AMD's in its place
        if (S.asked && S.wanted) vk_fg_take_over(S.why ? S.why : "DLSS Frame Generation did not load");
        return;
    }
    S.avail = S.available() && !test_unavailable();
    if (!S.avail) {   // Streamline's DLSS-G plugin refused this GPU, driver or system (or the test knob)
        rt_log("dlssg: DLSS Frame Generation off: it needs an RTX 40 or 50 series card, a current NVIDIA driver and "
               "Windows' hardware-accelerated GPU scheduling on (Settings > Display > Graphics)%s",
               test_unavailable() ? " (REKARINA_DLSSG_UNAVAILABLE)" : "");
        S.why = "not available: it needs an RTX 40 / 50 card, a current driver and hardware-accelerated GPU scheduling";
        vk_fg_take_over("no RTX 40 / 50 card, an older driver or GPU scheduling off");
        return;
    }
    PFN_vkGetDeviceProcAddr g = S.gdpa();
    S.create_sc = (PFN_vkCreateSwapchainKHR)g(dev, "vkCreateSwapchainKHR");
    S.destroy_sc = (PFN_vkDestroySwapchainKHR)g(dev, "vkDestroySwapchainKHR");
    S.images = (PFN_vkGetSwapchainImagesKHR)g(dev, "vkGetSwapchainImagesKHR");
    S.acquire = (PFN_vkAcquireNextImageKHR)g(dev, "vkAcquireNextImageKHR");
    S.present = (PFN_vkQueuePresentKHR)g(dev, "vkQueuePresentKHR");
    S.active = S.create_sc && S.destroy_sc && S.images && S.acquire && S.present;
    rt_log("dlssg: %s", S.active ? "DLSS Frame Generation on the swapchain" : "the swapchain hooks are missing: off");
    if (!S.active) S.why = "Streamline's swapchain hooks are missing";
    if (S.active) S.reflex(reflex_mode());
}

int vk_dlssg_active(void) { return S.active; }

// The menu's frame generation row: whether DLSS Frame Generation can run here. Streamline's own answer when it was
// loaded at start; else an NVIDIA card of the RTX 40 or 50 series (Ada and later: the generation's optical flow runs
// there; a Turing Quadro RTX 4000 is not one) with our library and Streamline's beside the game. 0 with *why.
int vk_dlssg_offered(const char **why) {
    if (test_unavailable() || (S.loaded > 0 && !S.avail)) {
        *why = "DLSS frame generation needs an RTX 40 or 50 card";
        return 0;
    }
    if (S.loaded > 0) return 1;
    const VkCtx *c = vk_ctx();
    const char *name = c ? c->props.deviceName : "";
    int rtx = c && c->props.vendorID == 0x10de && !strstr(name, "Quadro") &&
              (strstr(name, "RTX 40") || strstr(name, "RTX 50") || strstr(name, "Ada") || strstr(name, "RTX PRO"));
    if (!rtx) {
        *why = "DLSS frame generation needs an RTX 40 or 50 card";
        return 0;
    }
    static int have = -1;   // the files do not change while the game runs: looked at once
    if (have < 0) {
        char dir[PLAT_PATH_MAX], path[PLAT_PATH_MAX + 64];
        int n = plat_exe_path(dir, sizeof dir);
        char *cut = n ? plat_last_sep(dir) : NULL;
        have = 1;
        if (cut) {
            *cut = 0;
            static const char *const files[] = {"sl.interposer.dll", "sl.dlss_g.dll", "nvngx_dlssg.dll"};
            for (size_t i = 0; i < sizeof files / sizeof *files && have; i++)
                snprintf(path, sizeof path, "%s/%s", dir, files[i]), have = plat_file_exists(path);
            snprintf(path, sizeof path, "%s/" RT_FILE_STEM "_dlssg.dll", dir);
            int ours = plat_file_exists(path);
#ifdef RT_OLD_FILE_STEM
            snprintf(path, sizeof path, "%s/" RT_OLD_FILE_STEM "_dlssg.dll", dir);
            ours = ours || plat_file_exists(path);
#endif
            have = have && ours;
        }
    }
    if (have) return 1;
    *why = "DLSS frame generation's libraries are missing (run the setup again)";
    return 0;
}

const char *vk_dlssg_off_why(void) {
    if (!S.asked || S.active || (vk_fg_instead() && vk_fg_active())) return NULL;
    return S.why ? S.why : "it did not start (the dlssg lines of the log say why)";
}

VkResult vk_dlssg_create_swapchain(VkDevice d, const VkSwapchainCreateInfoKHR *ci, VkSwapchainKHR *sc) {
    VkResult r = S.create_sc(d, ci, NULL, sc);
    if (r == VK_SUCCESS) S.reflex(reflex_mode());
    return r;
}
void vk_dlssg_destroy_swapchain(VkDevice d, VkSwapchainKHR sc) { S.destroy_sc(d, sc, NULL); }
VkResult vk_dlssg_get_images(VkDevice d, VkSwapchainKHR sc, uint32_t *n, VkImage *img) { return S.images(d, sc, n, img); }
VkResult vk_dlssg_acquire(VkDevice d, VkSwapchainKHR sc, uint64_t t, VkSemaphore sem, uint32_t *idx) {
    return S.acquire(d, sc, t, sem, VK_NULL_HANDLE, idx);
}

VkResult vk_dlssg_present(VkQueue q, const VkPresentInfoKHR *pi) {
    S.marker(0, S.cur);
    VkResult r = S.present(q, pi);
    S.marker(1, S.cur);
    __atomic_store_n(&S.hosted_ns, kern_now_ns(), __ATOMIC_RELAXED);
    return r;
}

// Reflex holds the frame's start until the time the driver computes from the measured render and queue times, so the
// game's CPU work ends about when the GPU is free for it; then the frame's first markers. Once a frame: the game may
// acquire again before it presents. No hold while no host present went out for HOLD_IDLE_NS (the window minimized:
// gfx_present.c skips the host presents while the game runs on): the driver then has no present to time the frame by,
// and the sleep would wait out Streamline's half-second timeout each frame.
#define HOLD_IDLE_NS 200000000ull
void vk_dlssg_frame_start(void) {
    if (!S.active) return;
    uint32_t f = __atomic_load_n(&S.game, __ATOMIC_RELAXED) + 1;
    if (f == S.started) return;
    S.started = f;
    uint64_t hosted = __atomic_load_n(&S.hosted_ns, __ATOMIC_RELAXED);
    S.sleep(f, hosted && kern_now_ns() - hosted < HOLD_IDLE_NS);
}

void vk_dlssg_game_present(void) { __atomic_add_fetch(&S.game, 1, __ATOMIC_RELAXED); }

void vk_dlssg_submitted(uint64_t frame) {
    if (S.active) S.submit_end((uint32_t)frame);
}

void vk_dlssg_present_frame(uint64_t frame) { S.cur = (uint32_t)frame; }

uint64_t vk_dlssg_shown(void) { return S.active ? S.shown() : 0; }

void vk_dlssg_frame(VkCommandBuffer cb, VkExtent2D ext, VkImage hudless, VkImageView hudless_view, VkFormat hudless_fmt) {
    if (!S.active) return;
    static int asked;
    if (!asked) asked = 1, vk_dlaa_hudless_wanted = 1;   // the upscaling pass keeps a copy of the picture before the HUD
    VkFfxImage d, m;
    VkImageView dv, mvv;
    VkDlaaCamera c;
    float jx = 0, jy = 0;
    int reset = 0;
    int have = vk_dlaa_fg_inputs(&d, &m, &jx, &jy, &reset) && vk_dlaa_fg_views(&dv, &mvv) && vk_dlaa_fg_camera(&c);
    // Off in every frame without the inputs: the game's pause screens have no camera, and generation kept on there
    // blended whole frames of them (a tab switch of a pause screen blinked through the frames between, our overlay's
    // numbers showed twice; owner's report 2026-11-20). The library keeps the resources while it is off (nx2_sl.h
    // version 4), so on again costs nothing, and the first frame back starts generation's history anew (reset): the
    // frame before it is the last one before the pause. A library of version 3 frees them when off (400-750 ms a
    // frame for four or five frames at each inventory opening): with it generation stays on through such frames once
    // it went on, as before.
    static int went_on, was_on;
    int on = (have || (went_on && !S.retains)) && !menu_is_open();
    if (on) went_on = 1;
    if (on && have && S.retains && !was_on) reset = 1;
    was_on = on;
    static uint64_t generating;
    if (on) fg_guard_running(++generating);
    Nx2SlImage zd = {d.image, dv, d.format, d.width, d.height, VK_IMAGE_LAYOUT_GENERAL,
                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT};
    Nx2SlImage zm = {m.image, mvv, m.format, m.width, m.height, VK_IMAGE_LAYOUT_GENERAL,
                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT};
    Nx2SlCamera k;
    memset(&k, 0, sizeof k);
    if (have) {
        memcpy(k.view_to_clip, c.view_to_clip, 64), memcpy(k.clip_to_view, c.clip_to_view, 64);
        memcpy(k.clip_to_prev_clip, c.clip_to_prev_clip, 64), memcpy(k.prev_clip_to_clip, c.prev_clip_to_clip, 64);
        memcpy(k.pos, c.pos, 12), memcpy(k.up, c.up, 12), memcpy(k.right, c.right, 12), memcpy(k.fwd, c.fwd, 12);
        k.znear = 0.1f, k.zfar = 1e8f, k.fov_y = c.fov_y, k.aspect = c.aspect;   // OoT's projection (vk_ffx.cpp)
        k.jitter[0] = jx, k.jitter[1] = jy;
        k.mv_scale[0] = 1.0f / (float)d.width, k.mv_scale[1] = 1.0f / (float)d.height;   // render pixels
        k.reset = reset;
    }
    // The picture without the HUD (gfx_present.c hudless_record): without it the generated frames blend the game's
    // interface as if it were the scene, and a dialogue box that changes between two frames blinks in the frames between.
    Nx2SlImage zh = {hudless, hudless_view, hudless_fmt, ext.width, ext.height, VK_IMAGE_LAYOUT_GENERAL,
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                         VK_IMAGE_USAGE_STORAGE_BIT};
    S.frame(cb, have ? &zd : NULL, have ? &zm : NULL, have && hudless && hudless_view ? &zh : NULL, have ? &k : NULL,
            ext.width, ext.height, on, S.cur);
}

void vk_dlssg_release(void) {
    if (S.loaded > 0 && S.shutdown) S.shutdown();
    S.active = 0;
}
