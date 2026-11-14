// The in-game menu core: Dear ImGui state, the event queue the window adapter fills, gamepad navigation, the Vulkan back
// end on our device and the menu's pages. Free of any window system (menu.h). ImGui runs on the present path only,
// under gfx_present.c's present lock; the event functions only queue.
//
// The Vulkan set-up follows LAS Recomp's runtime/nvn/vulkan/imgui_glue.cpp (https://github.com/peiikai/LASRecomp @
// da35b83a9611d046a9a681aae668d7085954d37f, MIT, Copyright (c) 2026 Peiikai): dynamic rendering into the swapchain
// image, an internal descriptor pool, vertex buffers rotated over more frames than are in flight.
//
// The look follows the game's own Settings screen and system menu (OoT): a brown band across the top with
// the tabs (L / R and Q / E switch them), a dark panel of khaki rows on the left, the live picture and the selected
// row's description on the right, button hints at the bottom right. The colours are sampled from the game's screens;
// the drawing is ours (flat fills, a grain texture from tools/make_menu_textures.py, no game artwork). The tab bar
// with L / R and the two-pane idea came from SavageWindRecomp's runtime/ui/menu.mm
// (https://github.com/Revivedx/SavageWindRecomp @ 575ad88e02f9a89a9a885d90039d48af33c982f4, MIT, Copyright (c) 2026
// Revivedx and the Savage Wind Recomp contributors, Copyright (c) 2026 Peiikai); the pages are rows of option_rows.cpp.
#include <atomic>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "imgui.h"
#include "imgui_internal.h"   // SetFocusID, NavCursorVisible
#include "backends/imgui_impl_vulkan.h"
// A private copy of stb_truetype, the parser Dear ImGui builds its font atlas with, to check a system font before the
// atlas gets it (font_usable). Its asserts only mark the font bad here.
static int g_font_bad;
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_assert(x) ((x) ? (void)0 : (void)(g_font_bad = 1))
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#include "imstb_truetype.h"
#pragma clang diagnostic pop
#include "ui/amiibo_page.h"
#include "ui/binds_page.h"
#include "ui/option_rows.h"

extern "C" {
#include "amiibo.h"
#include "cheats.h"
#include "cfg.h"
#include "fs.h"
#include "input.h"
#include "input_map.h"
#include "mods.h"
#include "perfmon.h"
#include "platform/platform.h"
#include "sysinfo.h"
#include "ui/menu.h"
#include "ui_text.h"
#include "update_note.h"
// host.h and vk.h pull in cpu.h, which is C only: the functions the menu needs from them, as declared there.
void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void vk_queue_lock(void);
void vk_queue_unlock(void);
void res_sizes(unsigned out[2], unsigned scene[2]);
double frame_step_limit_hz(void);
extern const int rt_game_version;
extern "C" const char *rt_game_version_text(void);
extern const char rt_port_version[];
void cheats_frame(uint64_t frame);   // cheats.c
bool tab_cheats(float scale);        // ui/cheats_tab.cpp; true: restart into the rebuild
}

// Roboto Medium (Apache-2.0, ui/fonts/LICENSE-Roboto.txt) with Latin and Cyrillic glyphs.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"   // #embed in C++
static const unsigned char roboto_ttf[] = {
#embed "fonts/Roboto-Medium.ttf"
};
// The grain of the menu's surfaces: GRAIN x GRAIN bytes, tileable (tools/make_menu_textures.py). It rides in the font
// atlas as the alpha of a white square (fonts_up) and is drawn in a dark tint over flat fills (opt_grain).
enum { GRAIN = 256 };
static const unsigned char grain_a8[] = {
#embed "tex/grain.a8"
};
// The port's logo, the mark and the name (tools/make_rekarina_logo.py): its coverage, rows top down, after the width
// and the height as two 16-bit little-endian numbers. It rides in the font atlas like the grain; the band tints it.
static const unsigned char logo_a8[] = {
#embed "tex/logo.a8"
};
#pragma clang diagnostic pop

// ---- open state, any thread
static std::atomic<int> g_open;     // the menu is open
static std::atomic<int> g_reset;    // opened or closed since the last laid-out frame: drop stale input

// g_open, the input block and the log line change together under open_lock, so they cannot disagree when the window thread
// (F1, Esc), the polling thread (combo) and the present path (Continue, B) race.
static PlatMutex open_lock = PLAT_MUTEX_INIT;

// want 0 or 1 sets the state; -1 flips it. Returns the new state.
static int change_open(int want) {
    plat_mutex_lock(&open_lock);
    int open = want < 0 ? !g_open.load() : want;
    if (g_open.load() != open) {
        g_open.store(open);
        g_reset.store(1);
        input_menu(open);
        rt_log("menu: %s", open ? "open" : "closed");
    }
    plat_mutex_unlock(&open_lock);
    return open;
}

static void set_open(int open) { change_open(open); }

static int g_failed_flag();
extern "C" int menu_is_open(void) { return g_open.load(std::memory_order_relaxed); }
extern "C" void menu_toggle(void) {
    if (!g_failed_flag()) change_open(-1);
}

// ---- events from the window adapter, queued while the menu is open; the present path hands them to ImGui
enum { EV_KEY, EV_POS, EV_BUTTON, EV_WHEEL, EV_FOCUS, EV_MAX = 256 };
struct Ev { int type, a, b; float x, y; };
static PlatMutex ev_lock = PLAT_MUTEX_INIT;
static Ev ev_q[EV_MAX];
static int ev_n;

static void push(Ev e) {
    if (!menu_is_open()) return;
    plat_mutex_lock(&ev_lock);
    if (e.type == EV_POS && ev_n && ev_q[ev_n - 1].type == EV_POS) ev_q[ev_n - 1] = e;   // only the newest position
    else if (ev_n < EV_MAX) ev_q[ev_n++] = e;
    plat_mutex_unlock(&ev_lock);
}

extern "C" void menu_event_key(int key, int down) { push({EV_KEY, key, down, 0, 0}); }
extern "C" void menu_event_mouse_pos(float x, float y) { push({EV_POS, 0, 0, x, y}); }
extern "C" void menu_event_mouse_button(int button, int down) { push({EV_BUTTON, button, down, 0, 0}); }
extern "C" void menu_event_wheel(float dx, float dy) { push({EV_WHEEL, 0, 0, dx, dy}); }
extern "C" void menu_event_focus(int focused) { push({EV_FOCUS, focused, 0, 0, 0}); }

static void drain_events(ImGuiIO &io) {
    Ev q[EV_MAX];
    plat_mutex_lock(&ev_lock);
    int n = ev_n;
    for (int i = 0; i < n; i++) q[i] = ev_q[i];
    ev_n = 0;
    plat_mutex_unlock(&ev_lock);
    for (int i = 0; i < n; i++) {
        const Ev &e = q[i];
        switch (e.type) {
        case EV_KEY: io.AddKeyEvent((ImGuiKey)e.a, e.b != 0); break;
        case EV_POS: io.AddMousePosEvent(e.x, e.y); break;
        case EV_BUTTON: io.AddMouseButtonEvent(e.a, e.b != 0); break;
        case EV_WHEEL: io.AddMouseWheelEvent(e.x, e.y); break;
        case EV_FOCUS: io.AddFocusEvent(e.a != 0); break;
        }
    }
}

// ---- gamepad navigation: the Switch buttons of our SDL3 gamepad to ImGui's gamepad keys. Nintendo layout: A (east)
// activates, B (south) cancels.
static const struct { int bit; ImGuiKey key; } pad_keys[] = {
    {B_A, ImGuiKey_GamepadFaceDown}, {B_B, ImGuiKey_GamepadFaceRight}, {B_X, ImGuiKey_GamepadFaceUp},
    {B_Y, ImGuiKey_GamepadFaceLeft}, {B_L, ImGuiKey_GamepadL1}, {B_R, ImGuiKey_GamepadR1},
    {B_ZL, ImGuiKey_GamepadL2}, {B_ZR, ImGuiKey_GamepadR2}, {B_LEFT, ImGuiKey_GamepadDpadLeft},
    {B_UP, ImGuiKey_GamepadDpadUp}, {B_RIGHT, ImGuiKey_GamepadDpadRight}, {B_DOWN, ImGuiKey_GamepadDpadDown},
};
static uint64_t pad_prev;   // buttons at the last frame; a button held when the menu opened counts once released
static int stick_dir = -1;  // the D-pad button the left stick holds, -1 none

// The left stick moves the cursor as the D-pad does (ImGui's own stick handling only scrolls the page): past half way
// it holds the D-pad direction of its larger axis, kept until that axis falls under 0.35; ImGui repeats a held one.
static int stick_to_dpad(int32_t lx, int32_t ly) {
    float x = lx / 32767.0f, y = ly / 32767.0f;   // y up
    float along = stick_dir == B_LEFT ? -x : stick_dir == B_RIGHT ? x : stick_dir == B_UP ? y : stick_dir == B_DOWN ? -y : 0;
    if (stick_dir >= 0 && along > 0.35f) return stick_dir;
    if (fmaxf(fabsf(x), fabsf(y)) < 0.5f) return -1;
    return fabsf(x) > fabsf(y) ? (x < 0 ? B_LEFT : B_RIGHT) : (y > 0 ? B_UP : B_DOWN);
}

static void pad_events(ImGuiIO &io, int reset) {
    uint64_t b;
    int32_t lx, ly;
    int have = input_pad_read(&b, &lx, &ly);
    if (have) io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    else io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
    stick_dir = stick_to_dpad(lx, ly);
    if (stick_dir >= 0) b |= 1ull << stick_dir;
    if (reset) {
        pad_prev = b;
        return;
    }
    for (const auto &k : pad_keys) {
        uint64_t m = 1ull << k.bit;
        if ((b ^ pad_prev) & m) io.AddKeyEvent(k.key, (b & m) != 0);
    }
    pad_prev = b;
}

// ---- ImGui and its Vulkan back end (present path)
static int g_ctx_ok;              // context made
static std::atomic<int> g_failed; // set-up failed for good (the menu stays closed)
static int g_failed_flag() { return g_failed.load(std::memory_order_relaxed); }
static int g_vk_ok;               // back end initialised for g_vk_fmt
static VkFormat g_vk_fmt;
static float g_font_scale;        // the scale of the uploaded font atlas; 0 when none is uploaded
static float g_style_scale;       // the scale the style was last sized for
static ImGuiStyle g_base_style;   // the menu's style at 1080 lines
static uint64_t g_last_ns;
static uint64_t g_cost_ns, g_cost_frames;   // CPU time of layout and recording while open, logged per 600 frames

// Sizes are tuned for 1080 lines and follow the window's height. Under 720 lines at 100% display scale they stay at the
// 720 size; that floor grows with the window's DPI, so a small window on a 150% or 200% display (sizes are physical
// pixels) still gets text as large as the display scale asks for.
static float menu_scale(uint32_t h) {
    float least = 720.0f * (float)gfx_window_dpi() / 96.0f;
    return ((float)h >= least ? (float)h : least) / 1080.0f;
}

// The game's menu colours for everything ImGui draws itself: confirmations, the performance panel, the Amiibo and
// Cheats pages. Square corners, as every plate of the game's menus has.
static void style_init(ImGuiStyle &s) {
    ImGui::StyleColorsDark(&s);
    s.WindowRounding = 0; s.FrameRounding = 0; s.GrabRounding = 0; s.TabRounding = 0; s.ChildRounding = 0; s.PopupRounding = 0;
    s.WindowPadding = ImVec2(28, 22); s.FramePadding = ImVec2(14, 8); s.ItemSpacing = ImVec2(14, 12);
    s.WindowBorderSize = 1; s.ScrollbarSize = 10; s.ScrollbarRounding = 5;
    ImVec4 *c = s.Colors;
    const ImVec4 amber(0.85f, 0.58f, 0.12f, 1), khaki(0.435f, 0.353f, 0.208f, 1), khaki_hot(0.486f, 0.400f, 0.243f, 1);
    c[ImGuiCol_WindowBg] = ImVec4(0.165f, 0.114f, 0.071f, 0.97f);
    c[ImGuiCol_PopupBg] = ImVec4(0.165f, 0.114f, 0.071f, 0.98f);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_Border] = ImVec4(0.549f, 0.463f, 0.282f, 1);
    c[ImGuiCol_Text] = ImVec4(0.965f, 0.937f, 0.839f, 1);
    c[ImGuiCol_TextDisabled] = ImVec4(0.74f, 0.70f, 0.58f, 1);
    c[ImGuiCol_Button] = khaki;
    c[ImGuiCol_ButtonHovered] = khaki_hot;
    c[ImGuiCol_ButtonActive] = amber;
    c[ImGuiCol_FrameBg] = ImVec4(0.118f, 0.102f, 0.071f, 1);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.18f, 0.15f, 0.10f, 1);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.22f, 0.18f, 0.11f, 1);
    c[ImGuiCol_Header] = ImVec4(0.435f, 0.353f, 0.208f, 0.7f);
    c[ImGuiCol_HeaderHovered] = khaki_hot;
    c[ImGuiCol_HeaderActive] = amber;
    c[ImGuiCol_CheckMark] = amber;
    c[ImGuiCol_SliderGrab] = amber;
    c[ImGuiCol_SliderGrabActive] = amber;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0.25f);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(0.48f, 0.35f, 0.13f, 1);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.60f, 0.44f, 0.16f, 1);
    c[ImGuiCol_ScrollbarGrabActive] = amber;
    c[ImGuiCol_Tab] = khaki;
    c[ImGuiCol_TabHovered] = khaki_hot;
    c[ImGuiCol_TabSelected] = ImVec4(0.54f, 0.43f, 0.24f, 1);
    c[ImGuiCol_TabSelectedOverline] = amber;
    c[ImGuiCol_NavCursor] = ImVec4(1, 1, 1, 1);
    c[ImGuiCol_Separator] = ImVec4(0.36f, 0.29f, 0.18f, 1);
    c[ImGuiCol_PlotLines] = ImVec4(0.965f, 0.937f, 0.839f, 1);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.02f, 0.01f, 0.0f, 0.6f);
}

static void make_context(void) {
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendPlatformName = "totk_menu";
    style_init(g_base_style);
    ImGui::GetStyle() = g_base_style;
    g_ctx_ok = 1;
}

static void vk_down(void) {
    if (!g_vk_ok) return;
    ImGui_ImplVulkan_Shutdown();
    g_vk_ok = 0;
    g_font_scale = 0;
}

static int vk_up(const MenuTarget *t) {
    if (g_vk_ok && g_vk_fmt == t->format) return 1;
    if (g_vk_ok) {   // the swapchain changed format: pipelines for the new one
        vk_queue_lock();
        vkQueueWaitIdle(t->queue);
        vk_queue_unlock();
        vk_down();
    }
    ImGui_ImplVulkan_InitInfo ii = {};
    ii.ApiVersion = VK_API_VERSION_1_3;
    ii.Instance = t->instance;
    ii.PhysicalDevice = t->phys;
    ii.Device = t->device;
    ii.QueueFamily = t->queue_family;
    ii.Queue = t->queue;
    ii.DescriptorPoolSize = 16;
    ii.MinImageCount = 2;
    ii.ImageCount = 4;   // vertex buffers rotate per recorded frame: more than gfx_present.c's 2 frames in flight
    ii.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    ii.UseDynamicRendering = true;
    g_vk_fmt = t->format;
    ii.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    ii.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    ii.PipelineRenderingCreateInfo.pColorAttachmentFormats = &g_vk_fmt;
    if (!ImGui_ImplVulkan_Init(&ii)) return 0;
    g_vk_ok = 1;
    return 1;
}

// Roboto Medium (Latin, Cyrillic, dashes and quotes) at 26 px per 1080 lines, with the glyphs of the Japanese, Korean
// and Chinese language names merged in from a system font when there is one (each language is named in its own script).
// The menu language's own font goes in first with every string of its catalog row: a merge keeps the glyphs the atlas
// already has, and the four scripts share Han characters, so a Chinese menu behind the Japanese font would show a third
// of its characters in the Japanese forms. The other three give only their names. Made again when the window height or
// the menu language changes; the old texture goes after the queue is idle (CreateFontsTexture).
static const ImWchar kRanges[] = {0x0020, 0x00FF, 0x0152, 0x0153, 0x0400, 0x052F, 0x2010, 0x2027, 0};
static const struct { const char *lang; UiTextId name; } kCjk[] = {
    {"ja", UI_MENU_LANGNAME_JA}, {"ko", UI_MENU_LANGNAME_KO}, {"zh-Hans", UI_MENU_LANGNAME_ZH_HANS},
    {"zh-Hant", UI_MENU_LANGNAME_ZH_HANT},
};

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

// 1 when Dear ImGui can take this font file: more than the 100 bytes AddFontFromMemoryTTF asserts on, every table of its
// directory inside the file (stb_truetype reads tables without bounds checks, so a cut-off file faults when the glyphs
// are made), and stb_truetype parses it. ImGui asserts on a font it cannot parse, and an assert ends the process with
// 0xc0000409 and no line in the log.
static int font_usable(const uint8_t *d, uint64_t n) {
    if (!d || n <= 100 || n > INT_MAX) return 0;
    int off = stbtt_GetFontOffsetForIndex(d, 0);
    if (off < 0 || (uint64_t)off + 12 > n) return 0;
    uint32_t tables = (uint32_t)d[off + 4] << 8 | d[off + 5];
    if ((uint64_t)off + 12 + 16ull * tables > n) return 0;
    for (uint32_t i = 0; i < tables; i++) {
        const uint8_t *r = d + off + 12 + 16 * i;
        if ((uint64_t)be32(r + 8) + be32(r + 12) > n) return 0;
    }
    stbtt_fontinfo fi;
    g_font_bad = 0;
    return stbtt_InitFont(&fi, d, off) && !g_font_bad;
}

extern "C" int menu_font_check(void) {
    uint64_t n = sizeof roboto_ttf;
    uint8_t *bad = (uint8_t *)malloc(n);
    if (!bad) return 0;
    memcpy(bad, roboto_ttf, n);
    uint32_t tables = (uint32_t)bad[4] << 8 | bad[5];
    for (uint32_t i = 0; i < tables; i++)
        if (!memcmp(bad + 12 + 16 * i, "cmap", 4)) memset(bad + 12 + 16 * i, 0, 4);   // no character map
    int ok = font_usable(roboto_ttf, n) && !font_usable(roboto_ttf, 4096) && !font_usable(roboto_ttf, 100) &&
             !font_usable(bad, n);
    memset(bad, 0, 4);             // an unknown font tag
    ok = ok && !font_usable(bad, n);
    free(bad);
    return ok;
}

// The glyphs of every string of a catalog row (ui_text_visit), for the font that draws that language.
static void add_row_glyphs(void *ctx, const char *text) {
    static_cast<ImFontGlyphRangesBuilder *>(ctx)->AddText(text);
}

static bool g_grain_ok;
static bool g_logo_ok;
static ImVec2 g_logo_uv[2];    // the logo's corners in the atlas
static float g_logo_aspect;    // its width over its height
static ImVec2 g_grain_uv[2];   // the grain square's corners in the atlas

// The grain over [a, b] at the given strength (alpha of a dark tint, 0..255): the square repeats every GRAIN pixels of
// the 1080-line layout, anchored to the screen so neighbouring plates do not show the same patch.
void opt_grain(ImDrawList *dl, ImVec2 a, ImVec2 b, int alpha) {
    if (!g_grain_ok || b.x <= a.x || b.y <= a.y) return;
    float t = floorf(GRAIN * (g_style_scale > 0 ? g_style_scale : 1));
    if (t < 16) return;
    ImTextureID tex = ImGui::GetIO().Fonts->TexID;
    ImVec2 du((g_grain_uv[1].x - g_grain_uv[0].x) / t, (g_grain_uv[1].y - g_grain_uv[0].y) / t);
    ImU32 col = IM_COL32(18, 10, 4, alpha);
    for (float ty = floorf(a.y / t) * t; ty < b.y; ty += t)
        for (float tx = floorf(a.x / t) * t; tx < b.x; tx += t) {
            float x0 = fmaxf(tx, a.x), y0 = fmaxf(ty, a.y), x1 = fminf(tx + t, b.x), y1 = fminf(ty + t, b.y);
            // half a pixel in from the square's edge, so the filter never reads the atlas beside it
            float u0 = fmaxf(x0 - tx, 0.5f), v0 = fmaxf(y0 - ty, 0.5f), u1 = fminf(x1 - tx, t - 0.5f), v1 = fminf(y1 - ty, t - 0.5f);
            dl->AddImage(tex, ImVec2(x0, y0), ImVec2(x1, y1), ImVec2(g_grain_uv[0].x + u0 * du.x, g_grain_uv[0].y + v0 * du.y),
                         ImVec2(g_grain_uv[0].x + u1 * du.x, g_grain_uv[0].y + v1 * du.y), col);
        }
}

static char g_font_lang[16];   // the menu language the uploaded atlas was made for
static ImFont *g_font_big;     // the overlay's large frame rate: digits at their own size; NULL before the atlas is made

// The pictures of the prompt sets (romfs_prompt_previews): each button's mask is a square of the atlas, white with
// the mask as its alpha, like the grain. g_pv_uv: a button's corners in the atlas; g_pv_sets 0 when the game folder
// has no pictures or the atlas had no room for them.
enum { PV_SETS = 8 };
static int g_pv_sets, g_pv_count;
static ImVec2 g_pv_uv[PV_SETS][ROMFS_PREVIEW_BUTTONS][2];
static int g_pk_count;                              // the keys of the keyboard set's picture; 0 none
static ImVec2 g_pk_uv[ROMFS_PREVIEW_KEYS][2];

// The buttons of the prompt set the settings ask for, in a row from (x, y), each `size` pixels square and at most
// `width` in all; returns the row's height, 0 when there is no picture of that set (the keyboard prompts).
static float prompts_preview(ImDrawList *dl, float x, float y, float width, float size) {
    const char *layout = cfg_value("TOTK_PAD_LAYOUT");
    int set = romfs_prompt_preview_set(input_prompts_folder(input_prompts_pick(cfg_get("TOTK_PROMPTS"), layout), layout));
    if (set < 0 || set >= g_pv_sets || g_pv_count <= 0) return 0;
    float gap = floorf(size * 0.12f);
    if (g_pv_count * size + (g_pv_count - 1) * gap > width) size = floorf((width - (g_pv_count - 1) * gap) / g_pv_count);
    if (size < 8) return 0;
    ImTextureID tex = ImGui::GetIO().Fonts->TexID;
    for (int k = 0; k < g_pv_count; k++) {
        float bx = x + k * (size + gap);
        int key = romfs_prompt_preview_key(set, k);   // the keyboard set: the key bound to this button now
        const ImVec2 *uv = key >= 0 && key < g_pk_count ? g_pk_uv[key] : g_pv_uv[set][k];
        dl->AddImage(tex, ImVec2(bx, y), ImVec2(bx + size, y + size), uv[0], uv[1], IM_COL32(246, 239, 214, 255));
    }
    return size;
}

static int fonts_up(float scale) {
    enum { N = sizeof kCjk / sizeof *kCjk };
    const char *menu_lang = menu_text_lang();
    int order[N], first = -1;
    for (int i = 0; i < N; i++)
        if (!strcmp(kCjk[i].lang, menu_lang)) first = i;
    for (int i = 0, k = first >= 0; i < N; i++)
        if (i != first) order[k++] = i;
    if (first >= 0) order[0] = first;
    ImFontAtlas *fa = ImGui::GetIO().Fonts;
    fa->Clear();
    ImFontConfig fc;
    fc.FontDataOwnedByAtlas = false;
    fa->AddFontFromMemoryTTF((void *)roboto_ttf, (int)sizeof roboto_ttf, 26.0f * scale, &fc, kRanges);
    uint8_t *data[N] = {};
    ImVector<ImWchar> ranges[N];
    int merged = 0;
    for (int o = 0; o < N; o++) {
        int i = order[o];
        char path[512];
        uint64_t size = 0;
        if (!plat_font_path(kCjk[i].lang, path, sizeof path) || !(data[i] = plat_file_read(path, &size))) continue;
        if (!font_usable(data[i], size)) {
            rt_log("menu: system font %s left out: the font parser cannot read it (%llu bytes)", path,
                   (unsigned long long)size);
            free(data[i]);
            data[i] = NULL;
            continue;
        }
        merged++;
        ImFontGlyphRangesBuilder b;
        b.AddText(ui_text_in("en", kCjk[i].name));
        if (i == first) ui_text_visit(kCjk[i].lang, add_row_glyphs, &b);
        // Text the menu shows that no catalog row holds: a mod's cheat names and its folder name,
        // both read from disk at runtime. Their glyphs would otherwise fall back to the missing-glyph box.
        for (int k = 0; k < cheats_count(); k++)
            if (const CheatInfo *ci = cheats_info(k); ci && ci->name) b.AddText(ci->name);
        for (int k = 0; k < mods_count(); k++)
            if (const ModInfo *mi = mods_at(k); mi) b.AddText(mi->name);
        b.BuildRanges(&ranges[i]);
        ImFontConfig mc;
        mc.MergeMode = true;
        mc.FontDataOwnedByAtlas = false;
        fa->AddFontFromMemoryTTF(data[i], (int)size, 26.0f * scale, &mc, ranges[i].Data);
    }
    {   // The overlay's large frame rate (perf_full_draw): the digits at the size they are drawn. Enlarged from the
        // text font's glyphs they showed their pixels.
        static const ImWchar digits[] = {0x002D, 0x0039, 0};
        ImFontConfig bc;
        bc.FontDataOwnedByAtlas = false;
        g_font_big = fa->AddFontFromMemoryTTF((void *)roboto_ttf, (int)sizeof roboto_ttf, floorf(26.0f * scale * 1.75f), &bc, digits);
    }
    {   // the grain: a square of the atlas, white with the grain as its alpha
        g_grain_ok = false;
        int rect = fa->AddCustomRectRegular(GRAIN, GRAIN);
        // the prompt sets' buttons, a square each (a whole set in one rectangle is wider than a small atlas)
        int pv_cell = 0, pv_count = 0, pv_sets = romfs_prompt_previews(&pv_cell, &pv_count);
        static int pv_rect[PV_SETS][ROMFS_PREVIEW_BUTTONS];
        if (pv_sets > PV_SETS) pv_sets = PV_SETS;
        for (int i = 0; i < pv_sets; i++)
            for (int k = 0; k < pv_count; k++) pv_rect[i][k] = fa->AddCustomRectRegular(pv_cell, pv_cell);
        const int logo_w = logo_a8[0] | logo_a8[1] << 8, logo_h = logo_a8[2] | logo_a8[3] << 8;
        int logo_rect = (size_t)logo_w * logo_h + 4 == sizeof logo_a8 ? fa->AddCustomRectRegular(logo_w, logo_h) : -1;
        int pk_cell = 0, pk_count = 0;
        const uint8_t *pk_masks = romfs_prompt_key_masks(&pk_cell, &pk_count);
        static int pk_rect[ROMFS_PREVIEW_KEYS];
        if (!pk_masks || pk_count > ROMFS_PREVIEW_KEYS) pk_count = 0;
        for (int k = 0; k < pk_count; k++) pk_rect[k] = fa->AddCustomRectRegular(pk_cell, pk_cell);
        unsigned char *px = nullptr;
        int tw = 0, th = 0;
        fa->GetTexDataAsRGBA32(&px, &tw, &th);   // builds the atlas; the upload below takes these pixels
        g_logo_ok = false;
        if (const ImFontAtlasCustomRect *lr = logo_rect >= 0 ? fa->GetCustomRectByIndex(logo_rect) : nullptr;
            px && lr && lr->IsPacked()) {
            for (int y = 0; y < logo_h; y++)
                for (int x = 0; x < logo_w; x++) {
                    unsigned char *d = px + ((size_t)(lr->Y + y) * tw + lr->X + x) * 4;
                    d[0] = d[1] = d[2] = 255;
                    d[3] = logo_a8[4 + y * logo_w + x];
                }
            fa->CalcCustomRectUV(lr, &g_logo_uv[0], &g_logo_uv[1]);
            g_logo_aspect = (float)logo_w / (float)logo_h;
            g_logo_ok = true;
        }
        g_pk_count = 0;
        bool pk_ok = px && pk_count > 0;
        for (int k = 0; k < pk_count && pk_ok; k++) {
            const ImFontAtlasCustomRect *pr = pk_rect[k] >= 0 ? fa->GetCustomRectByIndex(pk_rect[k]) : nullptr;
            if (!pr || !pr->IsPacked()) { pk_ok = false; break; }
            const uint8_t *m = pk_masks + (size_t)k * pk_cell * pk_cell;
            for (int y = 0; y < pk_cell; y++)
                for (int x = 0; x < pk_cell; x++) {
                    unsigned char *d = px + ((size_t)(pr->Y + y) * tw + pr->X + x) * 4;
                    d[0] = d[1] = d[2] = 255;
                    d[3] = m[y * pk_cell + x];
                }
            fa->CalcCustomRectUV(pr, &g_pk_uv[k][0], &g_pk_uv[k][1]);
        }
        if (pk_ok) g_pk_count = pk_count;
        g_pv_sets = 0;
        bool pv_ok = px && pv_sets > 0;
        for (int i = 0; i < pv_sets && pv_ok; i++) {
            const uint8_t *masks = romfs_prompt_preview_masks(i);
            for (int k = 0; k < pv_count && pv_ok; k++) {
                const ImFontAtlasCustomRect *pr = pv_rect[i][k] >= 0 ? fa->GetCustomRectByIndex(pv_rect[i][k]) : nullptr;
                if (!masks || !pr || !pr->IsPacked()) { pv_ok = false; break; }
                const uint8_t *m = masks + (size_t)k * pv_cell * pv_cell;
                for (int y = 0; y < pv_cell; y++)
                    for (int x = 0; x < pv_cell; x++) {
                        unsigned char *d = px + ((size_t)(pr->Y + y) * tw + pr->X + x) * 4;
                        d[0] = d[1] = d[2] = 255;
                        d[3] = m[y * pv_cell + x];
                    }
                fa->CalcCustomRectUV(pr, &g_pv_uv[i][k][0], &g_pv_uv[i][k][1]);
            }
        }
        if (pv_ok) g_pv_sets = pv_sets, g_pv_count = pv_count;
        const ImFontAtlasCustomRect *r = rect >= 0 ? fa->GetCustomRectByIndex(rect) : nullptr;
        if (px && r && r->IsPacked()) {
            for (int y = 0; y < GRAIN; y++)
                for (int x = 0; x < GRAIN; x++) {
                    unsigned char *d = px + ((size_t)(r->Y + y) * tw + r->X + x) * 4;
                    d[0] = d[1] = d[2] = 255;
                    d[3] = grain_a8[y * GRAIN + x];
                }
            fa->CalcCustomRectUV(r, &g_grain_uv[0], &g_grain_uv[1]);
            g_grain_ok = true;
        }
    }
    vk_queue_lock();   // the upload submits on the queue and waits for it
    int ok = ImGui_ImplVulkan_CreateFontsTexture();
    vk_queue_unlock();
    fa->ClearInputData();   // the glyphs are in the texture: the font files and the pixels are not needed again
    fa->ClearTexData();
    for (uint8_t *d : data) free(d);
    rt_log("menu: font atlas at %.1f px for %s, language names from %d of %d system fonts", 26.0f * scale, menu_lang,
           merged, (int)N);
    g_font_scale = ok ? scale : 0;
    snprintf(g_font_lang, sizeof g_font_lang, "%s", menu_lang);
    return ok;
}

extern "C" void menu_vk_release(void) {
    if (g_ctx_ok) vk_down();
}

// ---- test hooks
// TOTK_MENU_TEST=<frame>[:<frames per page>]: at that presented frame the menu opens on its first tab; each tab stays
// <frames per page> frames (default 30) and its last frame's swapchain image goes to TOTK_MENU_TEST_DIR (default: the
// working directory) as menu-test-<frame>-<page>.png; after the last tab the menu closes.
// TOTK_MENU_TEST_SET=<frame>:<key>=<value>[,<key>=<value>...][;<frame>:...]: cfg_set of each key (TOTK_ prefix
// optional) at that presented frame; changes live settings with no input, menu open or not. totk.ini is written only by
// the pseudo-key SAVE=1 (cfg_save, as a row change does; a failed write shows the menu's save note). The pseudo-key
// FOCUS=<setting> puts the cursor on that setting's row of the open tab (opt_focus_key), for a shot of its description.
enum { TAB_SCREEN, TAB_GAME, TAB_SOUND, TAB_CONTROLS, TAB_AMIIBO, TAB_MENU, TAB_ADVANCED, TAB_MODS, TAB_CHEATS, TAB_COUNT };
// The tabs this game's menu shows, in the band's order. OoT: the game has its own amiibo settings and the
// cheat files are TotK's, so those two pages stay out.
#ifdef GAME_OOT
static const int kTabs[] = {TAB_SCREEN, TAB_GAME, TAB_SOUND, TAB_CONTROLS, TAB_MENU, TAB_MODS, TAB_ADVANCED};
#else
static const int kTabs[] = {TAB_SCREEN, TAB_GAME, TAB_SOUND, TAB_CONTROLS, TAB_AMIIBO, TAB_MENU, TAB_ADVANCED, TAB_MODS, TAB_CHEATS};
#endif
enum { TABS = sizeof kTabs / sizeof *kTabs };
enum { TEST_PAGE_FRAMES = 30, TEST_SETS = 16 };
struct TestSet { uint64_t frame; char key[64], value[64]; };
static struct {
    int parsed;
    uint64_t at;          // TOTK_MENU_TEST frame; 0 none
    uint64_t per_page;
    uint64_t opened;      // the frame the hook opened the menu; 0 not yet
    int page;             // the tab being shot; -1 none
    TestSet sets[TEST_SETS];
    int n_sets, next_set; // TOTK_MENU_TEST_SET, in frame order
} g_test = {0, 0, 0, 0, -1, {}, 0, 0};

static void test_parse(void) {
    g_test.parsed = 1;
    if (const char *e = cfg_get("TOTK_MENU_TEST")) {
        long long at = atoll(e), per = 0;
        if (const char *c = strchr(e, ':')) per = atoll(c + 1);
        g_test.at = at > 0 ? (uint64_t)at : 0;
        g_test.per_page = per > 0 ? (uint64_t)per : TEST_PAGE_FRAMES;
    }
    const char *s = cfg_get("TOTK_MENU_TEST_SET");
    while (s && *s) {   // groups "<frame>:<key>=<value>,..." separated by ';'
        char *end;
        unsigned long long f = strtoull(s, &end, 10);
        if (*end != ':') break;
        s = end + 1;
        while (*s && *s != ';' && g_test.n_sets < TEST_SETS) {
            size_t n = strcspn(s, ",;");
            const char *eq = (const char *)memchr(s, '=', n);
            if (eq) {
                TestSet &t = g_test.sets[g_test.n_sets++];
                t.frame = f;
                int kn = (int)(eq - s), vn = (int)(s + n - eq - 1);
                snprintf(t.key, sizeof t.key, "%s%.*s", strncmp(s, "TOTK_", 5) ? "TOTK_" : "", kn, s);
                snprintf(t.value, sizeof t.value, "%.*s", vn, eq + 1);
            }
            s += n;
            if (*s == ',') s++;
        }
        while (*s && *s != ';') s++;
        if (*s == ';') s++;
    }
    if (g_test.at || g_test.n_sets)
        rt_log("menu: test hook: open at frame %llu, %llu frames per page, %d settings changes",
               (unsigned long long)g_test.at, (unsigned long long)g_test.per_page, g_test.n_sets);
}

static void test_apply_sets(uint64_t frame) {
    for (; g_test.next_set < g_test.n_sets && g_test.sets[g_test.next_set].frame <= frame; g_test.next_set++) {
        const TestSet &t = g_test.sets[g_test.next_set];
        rt_log("menu: test hook: frame %llu: %s=%s", (unsigned long long)frame, t.key, t.value);
        if (!strcmp(t.key, "TOTK_SAVE")) rt_log("menu: test hook: cfg_save %s", cfg_save() ? "wrote" : "failed");
        else if (!strcmp(t.key, "TOTK_FOCUS")) opt_focus_key(t.value);
        else cfg_set(t.key, t.value);
    }
}

// ---- the lists the Screen and Game tabs build
enum { MAX_GPUS = 8 };
static char g_gpu_names[MAX_GPUS][VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
static int g_gpu_n = -1;   // -1: not listed yet

// The Vulkan devices in vkEnumeratePhysicalDevices order, which TOTK_GPU_INDEX counts in (vk_device.c pick_device).
static void list_gpus(VkInstance inst) {
    if (g_gpu_n >= 0) return;
    VkPhysicalDevice pd[MAX_GPUS];
    uint32_t n = MAX_GPUS;
    if (vkEnumeratePhysicalDevices(inst, &n, pd) < 0) n = 0;   // VK_INCOMPLETE keeps the first MAX_GPUS
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pd[i], &p);
        snprintf(g_gpu_names[i], sizeof g_gpu_names[i], "%s", p.deviceName);
    }
    g_gpu_n = (int)n;
}

// Window sizes: the common 16:9 sizes that fit the monitor, the 1920x1080 default and the current size.
static const int kSizes[][2] = {{1280, 720}, {1366, 768}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3200, 1800},
                                {3840, 2160}};
enum { MAX_SIZES = sizeof kSizes / sizeof *kSizes + 1 };

static void row_window_size(void) {
    int sz[MAX_SIZES][2], n = 0, mw = 0, mh = 0, cw = 0, ch = 0;
    int have = gfx_window_monitor_size(&mw, &mh);
    int cur_ok = sscanf(cfg_value("TOTK_WINDOW_SIZE"), "%dx%d", &cw, &ch) == 2;
    bool listed = false;
    for (size_t i = 0; i < sizeof kSizes / sizeof *kSizes; i++) {
        int w = kSizes[i][0], h = kSizes[i][1];
        bool cur = cur_ok && w == cw && h == ch;
        if (have && (w > mw || h > mh) && !cur && !(w == 1920 && h == 1080)) continue;
        listed |= cur;
        sz[n][0] = w, sz[n][1] = h, n++;
    }
    if (cur_ok && !listed) {   // the current size where its area puts it
        int at = n;
        while (at > 0 && (long)sz[at - 1][0] * sz[at - 1][1] > (long)cw * ch) at--;
        memmove(sz[at + 1], sz[at], sizeof sz[0] * (n - at));
        sz[at][0] = cw, sz[at][1] = ch, n++;
    }
    char vals[MAX_SIZES][24], shown[MAX_SIZES][32];
    const char *pv[MAX_SIZES], *pn[MAX_SIZES];
    for (int i = 0; i < n; i++) {
        snprintf(vals[i], sizeof vals[i], "%dx%d", sz[i][0], sz[i][1]);
        snprintf(shown[i], sizeof shown[i], "%d × %d", sz[i][0], sz[i][1]);   // U+00D7, the multiplication sign
        pv[i] = vals[i];
        pn[i] = shown[i];
    }
    opt_choice_list("TOTK_WINDOW_SIZE", UI_MENU_WINDOW_SIZE, pv, pn, n, UI_MENU_WINDOW_SIZE_DESC);
}

// The game resolution (resolution.c): 16:9 sizes from its smallest, 1280x720 (for weak GPUs), up, the common 21:9 and
// 32:9 monitor sizes, the 16:10 screens of the Steam Deck, handhelds and laptops and the 3:2 screens of laptops and
// tablets (Surface, Framework; the camera and the HUD follow
// another shape, resolution.c res_init; the installer lays the HUD out for each, installer/steps.py ASPECT_SIZES), and
// the current value when it is another size.
[[maybe_unused]] static void row_output_res(void) {
    static const char *const vals[] = {"1280x720", "1600x900", "1920x1080", "2560x1440", "3200x1800", "3840x2160",
                                       "2560x1080", "3440x1440", "3840x1600", "5120x2160", "3840x1080", "5120x1440",
                                       "1280x800", "1680x1050", "1920x1200", "2560x1600",
                                       "1920x1280", "2160x1440", "2256x1504", "2880x1920", "3000x2000"};
    enum { N = sizeof vals / sizeof *vals };
    char shown[N][32];
    const char *names[N];
    for (int i = 0; i < N; i++) {
        int w = 0, h = 0;
        sscanf(vals[i], "%dx%d", &w, &h);
        snprintf(shown[i], sizeof shown[i], "%d × %d", w, h);   // U+00D7, the multiplication sign
        names[i] = shown[i];
    }
    opt_choice_list("TOTK_OUTPUT_RES", UI_MENU_OUTPUT_RES, vals, names, N, UI_MENU_OUTPUT_RES_DESC);
}

static void row_render_scale(void) {
#ifdef GAME_OOT   // OoT: shares of the picture up to the whole of it; an older "game" in totk.ini reads as 100
    static const char *const ovals[] = {"50", "67", "75", "100"};
    static const char *const onames[] = {"50 %", "67 %", "75 %", "100 %"};
    int cur = -1;
    for (int i = 0; i < 4; i++)
        if (!strcmp(cfg_value("TOTK_RENDER_SCALE"), ovals[i])) cur = i;
    opt_choice_list("TOTK_RENDER_SCALE", UI_MENU_RENDER_SCALE, ovals, onames, 4, UI_MENU_RENDER_SCALE_DESC, cur < 0 ? 3 : cur);
    return;
#endif
    static const char *const vals[] = {"game", "50", "67", "75", "100", "125", "150", "200"};
    enum { N = sizeof vals / sizeof *vals };
    char shown[N][32];
    const char *names[N];
    names[0] = menu_text(UI_MENU_RENDER_SCALE_GAME);
    for (int i = 1; i < N; i++) {
        snprintf(shown[i], sizeof shown[i], "%s %%", vals[i]);
        names[i] = shown[i];
    }
    opt_choice_list("TOTK_RENDER_SCALE", UI_MENU_RENDER_SCALE, vals, names, N, UI_MENU_RENDER_SCALE_DESC);
}

[[maybe_unused]] static void row_shadow_res(void) {
    static const char *const vals[] = {"game", "256", "512", "768", "1024", "1536", "2048"};
    enum { N = sizeof vals / sizeof *vals };
    const char *names[N];
    names[0] = menu_text(UI_MENU_SHADOW_RES_GAME);
    for (int i = 1; i < N; i++) names[i] = vals[i];
    opt_choice_list("TOTK_SHADOW_RES", UI_MENU_SHADOW_RES, vals, names, N, UI_MENU_SHADOW_RES_DESC);
}

[[maybe_unused]] static void row_shadow_scale(void) {
    static const char *const vals[] = {"1", "2", "3", "4"};
    static const char *const names[] = {"960 (1x)", "1920 (2x)", "2880 (3x)", "3840 (4x)"};
    opt_choice_list("TOTK_SHADOW_SCALE", UI_MENU_SHADOW_SCALE, vals, names, 4, UI_MENU_SHADOW_SCALE_DESC);
}

[[maybe_unused]] static void row_shadow_range(void) {
    static const char *const vals[] = {"100", "150", "200", "300"};
    static const char *const names[] = {"160 m (1x)", "240 m (1.5x)", "320 m (2x)", "480 m (3x)"};
    opt_choice_list("TOTK_SHADOW_RANGE", UI_MENU_SHADOW_RANGE, vals, names, 4, UI_MENU_SHADOW_RANGE_DESC);
}

static void row_gpu(void) {
    const char *vals[MAX_GPUS + 1], *names[MAX_GPUS + 1];
    char idx[MAX_GPUS][4];
    vals[0] = "";
    names[0] = menu_text(UI_MENU_GPU_AUTO);
    for (int i = 0; i < g_gpu_n; i++) {
        snprintf(idx[i], sizeof idx[i], "%d", i);
        vals[i + 1] = idx[i];
        names[i + 1] = g_gpu_names[i];
    }
    opt_choice_list("TOTK_GPU_INDEX", UI_MENU_GPU, vals, names, g_gpu_n + 1, UI_MENU_GPU_DESC);
}

// The game language: one entry per Mals language file the running game build ships (ui_text_game_langs), each under
// its own name. A code that names no entry exactly (older "en", "fr-CA" on a build without it) selects the first entry
// of the same catalog language.
static void row_game_lang(void) {
    UiGameLang langs[16];
    int n = ui_text_game_langs(rt_game_version, langs, 16);
    const char *vals[17], *names[17];
    vals[0] = "";
    names[0] = menu_text(UI_MENU_GAME_LANG_AUTO);
    const char *now = cfg_value("TOTK_LANG");
    int cur = *now ? -1 : 0;
    for (int i = 1; i <= n; i++) {
        vals[i] = langs[i - 1].code;
        names[i] = menu_text(langs[i - 1].name);
        if (*now && !strcmp(now, vals[i])) cur = i;
    }
    for (int i = 1; i <= n && *now && cur < 0; i++)
        if (!strcmp(ui_text_lang(now), ui_text_lang(vals[i]))) cur = i;
    opt_choice_list("TOTK_LANG", UI_MENU_GAME_LANG, vals, names, n + 1, UI_MENU_GAME_LANG_DESC, cur);
}

// The frame limit (frame_rate.c): unset, the older fps60 = 0 still means 30. The monitor choice shows the rate it gives.
static void row_fps_limit() {
    // 40 and 45: the Steam Deck's screen runs at 40 to 60 Hz (the OLED to 90); 40 divides 40 and 120 Hz evenly
    static const char *const vals[] = {"30", "40", "45", "60", "90", "120", "144", "display"};
    static const UiTextId ids[] = {UI_MENU_FPS_30, UI_MENU_FPS_40,  UI_MENU_FPS_45,  UI_MENU_FPS_60,
                                   UI_MENU_FPS_90, UI_MENU_FPS_120, UI_MENU_FPS_144, UI_MENU_FPS_DISPLAY};
    enum { N = sizeof vals / sizeof *vals };
    const char *names[N];
    for (int i = 0; i < N; i++) names[i] = menu_text(ids[i]);
    static char display[64];
    double hz = gfx_window_refresh_hz();
    if (hz > 0) {
        snprintf(display, sizeof display, menu_text(UI_MENU_FPS_DISPLAY_HZ), hz);
        names[N - 1] = display;
    }
    const char *set = cfg_get("TOTK_FPS_LIMIT"), *old = cfg_get("TOTK_FPS60");
    int cur = (!set || !*set) && old && !strcmp(old, "0") ? 0 : -1;
    opt_choice_list("TOTK_FPS_LIMIT", UI_MENU_FPS_LIMIT, vals, names, N, UI_MENU_FPS_LIMIT_DESC, cur);
}

// The menu's name of a TOTK_VK_PRESENT mode (gfx_window_present_mode); NULL for another.
static const char *present_name(const char *mode) {
    static const struct { const char *mode; UiTextId name; } names[] = {
        {"mailbox", UI_MENU_PRESENT_MAILBOX}, {"fifo", UI_MENU_PRESENT_VSYNC}, {"immediate", UI_MENU_PRESENT_IMMEDIATE},
    };
    for (const auto &n : names)
        if (!strcmp(n.mode, mode)) return menu_text(n.name);
    return nullptr;
}

// "in use: VSync" under the Presentation value when the swapchain got another mode than the setting (a driver without
// mailbox gets fifo); NULL when they agree or before the first swapchain.
static const char *present_note(void) {
    const char *got = gfx_window_present_mode(), *name = present_name(got);
    if (!*got || !strcmp(got, cfg_value("TOTK_VK_PRESENT")) || !name) return nullptr;
    static char note[96];
    snprintf(note, sizeof note, menu_text(UI_MENU_PRESENT_IN_USE), name);
    return note;
}

// ---- the tabs
// Colour adjustment (vk_color.c): a profile writes the four values (STS78721's presets, issue #136); moving one of them
// makes the profile Custom.
static void rows_color(void) {
    static const struct { const char *name, *b, *c, *s, *g; } presets[] = {
        {"default", "0", "100", "100", "1"}, {"vivid", "0", "115", "135", "0.95"}, {"clarity", "2", "110", "115", "1"}};
    static const UiTextId names[] = {UI_MENU_COLOR_DEFAULT, UI_MENU_COLOR_VIVID, UI_MENU_COLOR_CLARITY,
                                     UI_MENU_COLOR_CUSTOM};
    opt_group(UI_MENU_COLOR);
    if (opt_choice("TOTK_COLOR_PRESET", UI_MENU_COLOR_PRESET, names, 4, UI_MENU_COLOR_PRESET_DESC)) {
        const char *p = cfg_value("TOTK_COLOR_PRESET");
        for (const auto &e : presets)
            if (!strcmp(p, e.name)) {
                cfg_set("TOTK_COLOR_BRIGHTNESS", e.b), cfg_set("TOTK_COLOR_CONTRAST", e.c);
                cfg_set("TOTK_COLOR_SATURATION", e.s), cfg_set("TOTK_COLOR_GAMMA", e.g);
                cfg_save();
            }
    }
    bool moved = opt_slider("TOTK_COLOR_BRIGHTNESS", UI_MENU_COLOR_BRIGHTNESS, -30, 30, UI_MENU_COLOR_BRIGHTNESS_DESC, 1,
                            "%+.0f %%");
    moved |= opt_slider("TOTK_COLOR_CONTRAST", UI_MENU_COLOR_CONTRAST, 70, 150, UI_MENU_COLOR_CONTRAST_DESC, 5, "%.0f %%");
    moved |= opt_slider("TOTK_COLOR_SATURATION", UI_MENU_COLOR_SATURATION, 50, 200, UI_MENU_COLOR_SATURATION_DESC, 5,
                        "%.0f %%");
    moved |= opt_slider("TOTK_COLOR_GAMMA", UI_MENU_COLOR_GAMMA, 0.8f, 1.2f, UI_MENU_COLOR_GAMMA_DESC, 0.02f, "%.2f");
    if (moved && strcmp(cfg_value("TOTK_COLOR_PRESET"), "custom")) {
        cfg_set("TOTK_COLOR_PRESET", "custom");
        cfg_save();
    }
}

extern "C" int wide_available(void);
extern "C" const char *vk_ffx_versions(void);   // vk_ffx.cpp: "" until AMD's library is loaded

static void tab_screen(float scale) {
    opt_begin("##screen", scale);
    opt_toggle("TOTK_FULLSCREEN", UI_MENU_FULLSCREEN, UI_MENU_FULLSCREEN_DESC);
    row_window_size();
    static const UiTextId present[] = {UI_MENU_PRESENT_MAILBOX, UI_MENU_PRESENT_VSYNC, UI_MENU_PRESENT_IMMEDIATE};
    opt_choice("TOTK_VK_PRESENT", UI_MENU_PRESENT, present, 3, UI_MENU_PRESENT_DESC, present_note());
    row_gpu();
    static const UiTextId perf[] = {UI_MENU_PERF_OFF, UI_MENU_PERF_FPS, UI_MENU_PERF_FULL};
    opt_choice("TOTK_PERF_OVERLAY", UI_MENU_PERF_OVERLAY, perf, 3, UI_MENU_PERF_OVERLAY_DESC);
    opt_group(UI_MENU_GRAPHICS);
#ifdef GAME_OOT   // OoT: the picture's size is the game resolution row below; the scene scale applies at once
    row_render_scale();
    {
        static const UiTextId texq[] = {UI_MENU_TEX_QUALITY_AUTO, UI_MENU_TEX_QUALITY_FULL, UI_MENU_TEX_QUALITY_HALF,
                                        UI_MENU_TEX_QUALITY_QUARTER};
        opt_choice("TOTK_TEX_QUALITY", UI_MENU_TEX_QUALITY, texq, 4, UI_MENU_TEX_QUALITY_DESC);
    }
#else
    row_output_res();
    row_render_scale();
    row_shadow_res();
#endif
    static const UiTextId up[] = {UI_MENU_UPSCALER_FSR, UI_MENU_UPSCALER_BILINEAR};
    opt_choice("TOTK_UPSCALER", UI_MENU_UPSCALER, up, 2, UI_MENU_UPSCALER_DESC);
    opt_slider("TOTK_SHARPNESS", UI_MENU_SHARPNESS, 0, 100, UI_MENU_SHARPNESS_DESC, 5, "%.0f %%");
    static const UiTextId aspect[] = {UI_MENU_ASPECT_FIT, UI_MENU_ASPECT_STRETCH};
    opt_choice("TOTK_ASPECT", UI_MENU_ASPECT, aspect, 2, UI_MENU_ASPECT_DESC);
    if (wide_available()) {   // the generated C has the widescreen patch sets (widescreen.c)
        static const char *const shape[] = {"16:9", "window", "21:9", "32:9"};
        const char *shape_names[] = {"16:9", menu_text(UI_MENU_GAME_RES_NATIVE), "21:9", "32:9"};
        opt_choice_list("TOTK_GAME_ASPECT", UI_MENU_GAME_ASPECT, shape, shape_names, 4, UI_MENU_GAME_ASPECT_DESC);
        static const char *const pic_res[] = {"1080", "1440", "1800", "2160", "2880", "window"};
        const char *pic_res_names[] = {"1080p", "1440p", "1800p", "2160p (4K)", "2880p (5K)", menu_text(UI_MENU_GAME_RES_NATIVE)};
        opt_choice_list("TOTK_GAME_RES", UI_MENU_GAME_RES, pic_res, pic_res_names, 6, UI_MENU_GAME_RES_DESC);
    }
#ifndef GAME_OOT   // the field of view and the shadow settings are patches of TotK
    opt_slider("TOTK_FOV", UI_MENU_FOV, 70, 140, UI_MENU_FOV_DESC, 5, "%.0f %%");
#endif
    static const char *const aniso[] = {"game", "2", "4", "8", "16"};
    const char *aniso_names[] = {menu_text(UI_MENU_ANISO_GAME), "2x", "4x", "8x", "16x"};
    opt_choice_list("TOTK_ANISO", UI_MENU_ANISO, aniso, aniso_names, 5, UI_MENU_ANISO_DESC);
#ifndef GAME_OOT
    row_shadow_scale();
    row_shadow_range();
    opt_slider("TOTK_PCSS", UI_MENU_PCSS, 0, 200, UI_MENU_PCSS_DESC, 10, "%.0f %%");
    opt_slider("TOTK_CONTACT_SHADOWS", UI_MENU_CONTACT_SHADOWS, 0, 100, UI_MENU_CONTACT_SHADOWS_DESC, 10, "%.0f %%");
#endif
    rows_color();   // with the picture's rows: the group below is the upscalers'
    opt_group(UI_MENU_UPSCALE_GROUP);
    opt_toggle("TOTK_DLAA", UI_MENU_DLAA, UI_MENU_DLAA_DESC);
    static const char *const taa[] = {"dlss", "fsr3"};
    const char *taa_names[] = {"NVIDIA DLSS", "AMD FSR"};
    opt_choice_list("TOTK_TAA", UI_MENU_TAA, taa, taa_names, 2, UI_MENU_TAA_DESC);
    if (!cfg_bool("TOTK_DLAA")) opt_disable_next(UI_MENU_FRAMEGEN_NEEDS);
    static const char *const framegen[] = {"off", "fsr3", "dlss"};
    const char *framegen_names[] = {menu_text(UI_MENU_DLSS_CHARS_OFF), "AMD FSR", "NVIDIA DLSS"};
    opt_choice_list("TOTK_FRAMEGEN", UI_MENU_FRAMEGEN, framegen, framegen_names, 3, UI_MENU_FRAMEGEN_DESC);
    static const UiTextId dlss[] = {UI_MENU_DLSS_DLAA, UI_MENU_DLSS_QUALITY, UI_MENU_DLSS_BALANCED,
                                    UI_MENU_DLSS_PERFORMANCE, UI_MENU_DLSS_ULTRA};
    opt_choice("TOTK_DLSS", UI_MENU_DLSS, dlss, 5, UI_MENU_DLSS_DESC);
    {   // the DLSS model: the letters are NVIDIA's own names for them (vk_dlaa.c preset_value)
        static const char *const preset[] = {"default", "k", "m", "l", "j", "e"};
        const char *preset_names[] = {menu_text(UI_MENU_DLSS_PRESET_DEFAULT), "K \xc2\xb7 Transformer", "M \xc2\xb7 Transformer 2",
                                      "L \xc2\xb7 Transformer 2", "J \xc2\xb7 Transformer", "E \xc2\xb7 CNN"};
        opt_choice_list("TOTK_DLSS_PRESET", UI_MENU_DLSS_PRESET, preset, preset_names, 6, UI_MENU_DLSS_PRESET_DESC);
    }
    {   // the FSR upscaler's version; under the value, the versions AMD's library offers on this card once it is loaded
        static const char *const fsr[] = {"auto", "4", "3"};
        const char *fsr_names[] = {menu_text(UI_MENU_DLSS_PRESET_DEFAULT), "FSR 4.1", "FSR 3.1"};
        const char *have = vk_ffx_versions();
        opt_choice_list("TOTK_FSR_VERSION", UI_MENU_FSR_VERSION, fsr, fsr_names, 3, UI_MENU_FSR_VERSION_DESC, -1,
                        *have ? have : nullptr);
    }
    static const UiTextId chars[] = {UI_MENU_DLSS_CHARS_OFF, UI_MENU_DLSS_CHARS_LIGHT, UI_MENU_DLSS_CHARS_MEDIUM,
                                     UI_MENU_DLSS_CHARS_FULL};
    opt_choice("TOTK_DLSS_CHAR_MASK", UI_MENU_DLSS_CHARS, chars, 4, UI_MENU_DLSS_CHARS_DESC);
#ifndef GAME_OOT
    opt_toggle("TOTK_FXAA", UI_MENU_FXAA, UI_MENU_FXAA_DESC);
#endif
    opt_group(UI_MENU_HDR_GROUP);
    opt_toggle("TOTK_HDR", UI_MENU_HDR, UI_MENU_HDR_DESC);
    opt_slider("TOTK_HDR_PEAK_NITS", UI_MENU_HDR_PEAK, 400, 10000, UI_MENU_HDR_PEAK_DESC, 50, "%.0f");
    opt_slider("TOTK_HDR_GAME_NITS", UI_MENU_HDR_GAME, 80, 500, UI_MENU_HDR_GAME_DESC, 5, "%.0f");
    opt_slider("TOTK_HDR_UI_NITS", UI_MENU_HDR_UI, 80, 500, UI_MENU_HDR_UI_DESC, 5, "%.0f");
    opt_slider("TOTK_HDR_HUE", UI_MENU_HDR_HUE, 0, 100, UI_MENU_HDR_HUE_DESC, 5, "%.0f %%");
    opt_end();
}

static void tab_game(float scale) {
    opt_begin("##game", scale);
    row_fps_limit();
    row_game_lang();
    static const UiTextId mode[] = {UI_MENU_MODE_DOCKED, UI_MENU_MODE_HANDHELD};
    opt_choice("TOTK_OPERATION_MODE", UI_MENU_OPERATION_MODE, mode, 2, UI_MENU_OPERATION_MODE_DESC);
    opt_toggle("TOTK_ASSUME_FOCUS", UI_MENU_ASSUME_FOCUS, UI_MENU_ASSUME_FOCUS_DESC);
    opt_end();
}

static void tab_sound(float scale) {
    opt_begin("##sound", scale);
    opt_slider("TOTK_VOLUME", UI_MENU_VOLUME, 0, 100, UI_MENU_VOLUME_DESC, 5, "%.0f %%");
    static const UiTextId channels[] = {UI_MENU_CHANNELS_AUTO, UI_MENU_CHANNELS_STEREO, UI_MENU_CHANNELS_51};
    opt_choice("TOTK_AUDIO_CHANNELS", UI_MENU_AUDIO_CHANNELS, channels, 3, UI_MENU_AUDIO_CHANNELS_DESC);
    opt_end();
}

// A confirmation is open (Controls: restart, Advanced: reset): B and Esc close it, not the menu; L / R stay on the page.
static bool g_modal;

// The note under the layout: Automatic with a gamepad connected, the layout it picked ("in use: Xbox"). NULL otherwise.
static const char *pad_layout_note(void) {
    int got = input_pad_layout();
    if (got < 0 || strcmp(cfg_value("TOTK_PAD_LAYOUT"), "auto")) return nullptr;
    static char note[96];
    snprintf(note, sizeof note, menu_text(UI_MENU_PAD_LAYOUT_IN_USE),
             menu_text(got == PAD_LAYOUT_XBOX ? UI_MENU_PAD_LAYOUT_XBOX : UI_MENU_PAD_LAYOUT_NINTENDO));
    return note;
}

// The action a key presses, or -1: the walk key is a key of its own, but Shift is B in the default bindings, and a
// walk key that also presses something walks and does that something at the same time. The bindings come from a
// snapshot, and every key of an action is looked at: binds_refresh rebuilds the live table on the polling thread while
// the menu draws, and an action may hold more than the two keys its cells show.
static int key_action(const InputMap *m, int key) {
    for (int a = 0; a < ACTION_COUNT; a++) {
        uint8_t keys[INPUT_BINDING_MAX];
        int n = input_map_keys_of(m, a, keys, INPUT_BINDING_MAX);
        for (int i = 0; i < n; i++)
            if (keys[i] == key) return a;
    }
    return -1;
}

// The walk key as the handful of keys a player is likely to pick. "none" rather than an empty value closes it: an
// unset key falls back to the default in the settings table, the same as a fresh installation. totk.ini takes any name
// input_map.c knows (walks of the keyboard sticks, not of the gamepad). A key the current bindings also use says which
// action it presses.
static void row_walk_key(void) {
    static const char *const keys[] = {"none", "lshift", "lalt", "lctrl", "tab", "capslock"};
    const char *names[sizeof keys / sizeof *keys] = {menu_text(UI_MENU_OFF), "Shift", "Alt", "Ctrl", "Tab", "Caps Lock"};
    const char *cur = cfg_value("TOTK_WALK_KEY");
    static char note[64];
    const char *n = nullptr;
    InputMap m;
    input_map_snapshot(&m);
    for (int k = KEY_A; k < KEY_COUNT && !n; k++) {
        if (!input_key_name(k) || strcasecmp(input_key_name(k), cur)) continue;
        int a = key_action(&m, k);
        if (a >= 0 && input_action_name(a)) snprintf(note, sizeof note, menu_text(UI_MENU_WALK_KEY_TAKEN), input_action_name(a)), n = note;
    }
    opt_choice_list("TOTK_WALK_KEY", UI_MENU_WALK_KEY, keys, names, sizeof keys / sizeof *keys, UI_MENU_WALK_KEY_DESC,
                    -1, n);
}

// The note under the prompts. The settings name a folder of prompts (input_prompts_folder: the prompts and the pad
// layout, as a layout that puts A on the right button needs the face buttons named the other way round), but the game
// reads its fonts and UI archive once at start (romfs.c overlay_init). While the folder in use differs: "files
// missing" when the setup did not write the one asked for, why it was left out at start (damaged, a game resolution
// that is not 16:9, a RomFS mod with the same files, TOTK_ROMFS_PATCHES), else "prompts change after restart", and only then
// *restart is set: a restart would bring the prompts asked for. NULL when they match.
static const char *prompts_note(bool *restart) {
    *restart = false;
    const char *layout = cfg_value("TOTK_PAD_LAYOUT"), *used = romfs_prompts();
    const char *want = input_prompts_folder(input_prompts_pick(cfg_get("TOTK_PROMPTS"), layout), layout);
    if (romfs_prompts_shown(want)) return nullptr;   // changed in the running game
    if (want == used || (want && used && !strcmp(want, used))) return nullptr;
    // A game resolution of another shape needs a folder of its own for the PC prompts (romfs-patches-pc-<w>x<h>), which
    // the setup does not write: the shape is the reason then, not the setup.
    int off = romfs_prompts_off(want);
    if (want && !romfs_prompts_ready(want) && off != ROMFS_PROMPTS_SHAPE) return menu_text(UI_MENU_PROMPTS_MISSING);
    switch (off) {
    case ROMFS_PROMPTS_FAILED: return menu_text(UI_MENU_PROMPTS_FAILED);
    case ROMFS_PROMPTS_SHAPE: return menu_text(UI_MENU_PROMPTS_OFF_SHAPE);
    case ROMFS_PROMPTS_MODS: return menu_text(UI_MENU_PROMPTS_OFF_MODS);
    case ROMFS_PROMPTS_PATCHES: return menu_text(UI_MENU_PROMPTS_OFF_PATCHES);
    default: *restart = true; return menu_text(UI_MENU_PROMPTS_RESTART);
    }
}

// The confirmation of "Restart now", over the menu; Cancel has the cursor first. Restart writes totk.ini, then closes
// the game as Quit does and starts it again (gfx_window_request_restart). It waits while the game writes a save, and
// the game stays when totk.ini cannot be written, as the new prompts would not come back. From the Cheats tab
// (restart_rebuild) Restart also lets the next start rebuild for cheats whose last rebuild failed (cheats_rebuild_allow).
static bool restart_rebuild;
static void restart_confirm(void) {
    g_modal = false;
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("##restart_confirm", nullptr,
                                ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize))
        return;
    g_modal = true;
    ImGui::TextUnformatted(menu_text(UI_MENU_RESTART_CONFIRM));
    bool saving = fs_save_busy() != 0;
    if (saving || !cfg_last_save_ok())
        ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.35f, 1), "%s",
                           menu_text(saving ? UI_MENU_RESTART_SAVING : UI_MENU_SAVE_FAILED));
    ImGui::Spacing();
    const char *yes = menu_text(UI_MENU_RESTART_YES), *no = menu_text(UI_MENU_RESET_NO);
    const ImGuiStyle &st = ImGui::GetStyle();
    float bw = fmaxf(ImGui::CalcTextSize(yes).x, ImGui::CalcTextSize(no).x) + st.FramePadding.x * 4;
    bool close = ImGui::Button(no, ImVec2(bw, 0));
    if (ImGui::IsWindowAppearing()) ImGui::SetItemDefaultFocus();
    ImGui::SameLine();
    ImGui::BeginDisabled(saving);
    if (ImGui::Button(yes, ImVec2(bw, 0)) && cfg_save()) {
        if (restart_rebuild) cheats_rebuild_allow();
        gfx_window_request_restart();
    }
    ImGui::EndDisabled();
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))
        close = true;
    if (close) {
        ImGui::CloseCurrentPopup();
        restart_rebuild = false;
        opt_focus_key("##restart");   // back on the row it came from
    }
    ImGui::EndPopup();
}

static void tab_controls(float scale) {
    opt_begin("##controls", scale);
    static const UiTextId layout[] = {UI_MENU_PAD_LAYOUT_AUTO, UI_MENU_PAD_LAYOUT_NINTENDO, UI_MENU_PAD_LAYOUT_XBOX};
    opt_choice("TOTK_PAD_LAYOUT", UI_MENU_PAD_LAYOUT, layout, 3, UI_MENU_PAD_LAYOUT_DESC, pad_layout_note());
    static const UiTextId prompts[] = {UI_MENU_PROMPTS_NINTENDO, UI_MENU_PROMPTS_XBOX, UI_MENU_PROMPTS_PC, UI_MENU_PROMPTS_PS,
                                       UI_MENU_PROMPTS_RK};
    bool restart;
    const char *note = prompts_note(&restart);
#ifdef GAME_OOT
    {   // this game's sets change in the running game where prompts-live.bin is there
        static const char *const values[] = {"nintendo", "xbox", "playstation", "rekarina", "pc"};
        const char *names[] = {menu_text(UI_MENU_PROMPTS_NINTENDO), menu_text(UI_MENU_PROMPTS_XBOX),
                               menu_text(UI_MENU_PROMPTS_PS), menu_text(UI_MENU_PROMPTS_RK), menu_text(UI_MENU_PROMPTS_PC)};
        (void)prompts;
        // the keyboard set only where the setup wrote it (an older game folder has four sets)
        opt_choice_list("TOTK_PROMPTS", UI_MENU_PROMPTS, values, names, romfs_prompts_ready("romfs-patches-pc") ? 5 : 4,
                        romfs_prompts_can_change() ? UI_MENU_PROMPTS_DESC_LIVE : UI_MENU_PROMPTS_DESC, -1, note);
    }
#else
    opt_choice("TOTK_PROMPTS", UI_MENU_PROMPTS, prompts, 5, UI_MENU_PROMPTS_DESC, note);
#endif
    bool ask = restart && opt_button("##restart", UI_MENU_RESTART, UI_MENU_RESTART_DESC);
    opt_toggle("TOTK_MOTION", UI_MENU_MOTION, UI_MENU_MOTION_DESC);
    opt_slider("TOTK_RUMBLE_STRENGTH", UI_MENU_RUMBLE_STRENGTH, 0, 300, UI_MENU_RUMBLE_STRENGTH_DESC, 10, "%.0f %%");
    opt_slider("TOTK_RUMBLE_CURVE", UI_MENU_RUMBLE_CURVE, 0.2f, 1.5f, UI_MENU_RUMBLE_CURVE_DESC, 0.05f, "%.2f");
    opt_toggle("TOTK_RUMBLE_TRIGGERS", UI_MENU_RUMBLE_TRIGGERS, UI_MENU_RUMBLE_TRIGGERS_DESC);
    static const UiTextId mouse_modes[] = {UI_MENU_MOUSE_STICK, UI_MENU_MOUSE_DIRECT};
    opt_choice("TOTK_MOUSE_MODE", UI_MENU_MOUSE_MODE, mouse_modes, 2, UI_MENU_MOUSE_MODE_DESC);
    opt_slider("TOTK_MOUSE_SENS", UI_MENU_MOUSE_SENS, 0, 10, UI_MENU_MOUSE_SENS_DESC, 0.1f, "%.1f");
    opt_toggle("TOTK_MENU_CURSOR", UI_MENU_MENU_CURSOR, UI_MENU_MENU_CURSOR_DESC);
    row_walk_key();
    opt_slider("TOTK_WALK_SCALE", UI_MENU_WALK_SCALE, 0.2f, 0.9f, UI_MENU_WALK_SCALE_DESC, 0.01f, "%.2f");
    // Analog sticks: one dead zone row per stick, falling back to TOTK_STICK_DEADZONE (both sticks) while unset. With
    // a gamepad connected each row shows its stick's deflection now, before the dead zone: a drifting stick's resting
    // value, to set it above.
    opt_group(UI_MENU_GROUP_STICKS);
    double now[2];
    bool pad = input_pad_stick_percent(now) != 0;
    char stick_note[2][64];
    for (int s = 0; s < 2; s++)
        snprintf(stick_note[s], sizeof stick_note[s], menu_text(UI_MENU_STICK_DEADZONE_NOW), now[s]);
    opt_slider("TOTK_LSTICK_DEADZONE", UI_MENU_LSTICK_DEADZONE, 0, 50, UI_MENU_STICK_DEADZONE_DESC, 1, "%.0f %%",
               pad ? stick_note[0] : nullptr, "TOTK_STICK_DEADZONE");
    opt_slider("TOTK_RSTICK_DEADZONE", UI_MENU_RSTICK_DEADZONE, 0, 50, UI_MENU_STICK_DEADZONE_DESC, 1, "%.0f %%",
               pad ? stick_note[1] : nullptr, "TOTK_STICK_DEADZONE");
    opt_slider("TOTK_STICK_SENS", UI_MENU_STICK_SENS, 0.1f, 3, UI_MENU_STICK_SENS_DESC, 0.05f, "%.2f");
    binds_rows(scale);
    opt_end();
    binds_after_page();
    if (ask) ImGui::OpenPopup("##restart_confirm");   // at the level BeginPopupModal looks it up
    restart_confirm();
}

static void tab_menu(float scale) {
    opt_begin("##menu", scale);
    // the order of TOTK_MENU_LANG's values (cfg.c metas[])
    static const UiTextId langs[] = {UI_MENU_MENU_LANG_AUTO, UI_MENU_LANGNAME_EN, UI_MENU_LANGNAME_DE,
                                     UI_MENU_LANGNAME_ES, UI_MENU_LANGNAME_FR, UI_MENU_LANGNAME_IT,
                                     UI_MENU_LANGNAME_NL, UI_MENU_LANGNAME_PT, UI_MENU_LANGNAME_PT_BR,
                                     UI_MENU_LANGNAME_RU, UI_MENU_LANGNAME_JA, UI_MENU_LANGNAME_KO,
                                     UI_MENU_LANGNAME_ZH_HANS, UI_MENU_LANGNAME_ZH_HANT};
    opt_choice("TOTK_MENU_LANG", UI_MENU_MENU_LANG, langs, sizeof langs / sizeof *langs, UI_MENU_MENU_LANG_DESC);
    opt_toggle("TOTK_DISCORD_RPC", UI_MENU_DISCORD_RPC, UI_MENU_DISCORD_RPC_DESC);
#ifdef GAME_OOT
    opt_toggle("TOTK_UPDATE_CHECK", UI_MENU_UPDATE_CHECK, UI_MENU_UPDATE_CHECK_DESC);
#endif
    opt_end();
}

// ---- Advanced: the testing and troubleshooting keys, the bug report helpers and the reset of every setting

// A thread count with no fixed default: Automatic (unset), then the common counts.
static void row_threads(const char *key, UiTextId label, UiTextId desc) {
    static const char *const vals[] = {"", "1", "2", "4", "6", "8", "12", "16", "24", "32", "48", "64"};
    enum { N = sizeof vals / sizeof *vals };
    const char *names[N];
    names[0] = menu_text(UI_MENU_AUTO);
    for (int i = 1; i < N; i++) names[i] = vals[i];
    opt_choice_list(key, label, vals, names, N, desc);
}

// A free-text key shown as Off (unset) / On (value set): On writes on_value, any set value reads as On.
static void row_set_or_not(const char *key, UiTextId label, UiTextId desc, const char *on_value) {
    const char *vals[] = {"", on_value}, *names[] = {menu_text(UI_MENU_OFF), menu_text(UI_MENU_ON)};
    opt_choice_list(key, label, vals, names, 2, desc, *cfg_value(key) ? 1 : 0);
}

static bool validation_layer_present(void) {
    static int have = -1;
    if (have < 0) {
        uint32_t n = 0;
        vkEnumerateInstanceLayerProperties(&n, nullptr);
        VkLayerProperties *l = (VkLayerProperties *)calloc(n ? n : 1, sizeof *l);
        if (l && vkEnumerateInstanceLayerProperties(&n, l) >= 0)
            for (uint32_t i = 0; i < n && have <= 0; i++) have = !strcmp(l[i].layerName, "VK_LAYER_KHRONOS_validation");
        free(l);
        if (have < 0) have = 0;
    }
    return have != 0;
}

// Every setting the menu shows back to its default (the environment's keys stay), written to totk.ini.
static void reset_all(void) {
    const char *keys[128];
    int n = 0;
    for (const CfgMeta *m; n < 128 && (m = cfg_meta_at(n)); n++) keys[n] = m->key;
    int done = cfg_reset(keys, n);
    opt_save_soon();
    rt_log("menu: reset %d settings to their defaults", done);
}

// A button's result shown in its value for a few seconds: 1 done (done_text), -1 failed.
struct ActionNote { uint64_t until; int result; };

static const char *action_value(const ActionNote &a, UiTextId done_text) {
    if (!a.result || plat_now_ns() > a.until) return "";
    return menu_text(a.result > 0 ? done_text : UI_MENU_ACTION_FAILED);
}

static void action_done(ActionNote &a, bool ok) { a = {plat_now_ns() + 3000000000ull, ok ? 1 : -1}; }

static void tab_advanced(float scale) {
    static ActionNote copy_note, log_note;
    opt_begin("##advanced", scale);
    opt_text(UI_MENU_ADV_WARNING, true);

    opt_group(UI_MENU_GROUP_SCHED);
    static const UiTextId preempt[] = {UI_MENU_PREEMPT_OFF, UI_MENU_PREEMPT_ON, UI_MENU_PREEMPT_INHERIT};   // 0|1|2
    opt_choice("TOTK_ASYNC_PREEMPT", UI_MENU_ASYNC_PREEMPT, preempt, 3, UI_MENU_ASYNC_PREEMPT_DESC);
    opt_toggle("TOTK_REPLAY_ASYNC", UI_MENU_REPLAY_ASYNC, UI_MENU_REPLAY_ASYNC_DESC);
    opt_toggle("TOTK_TIMER_1MS", UI_MENU_TIMER_1MS, UI_MENU_TIMER_1MS_DESC);

    opt_group(UI_MENU_GROUP_SHADERS);
    static const UiTextId wait[] = {UI_MENU_PIPE_WAIT_DEFAULT, UI_MENU_PIPE_WAIT_DRAW};
    opt_choice("TOTK_PIPE_WAIT", UI_MENU_PIPE_WAIT, wait, 2, UI_MENU_PIPE_WAIT_DESC);
    static const UiTextId skip[] = {UI_MENU_PIPE_SKIP_STRICT, UI_MENU_PIPE_SKIP_ANY, UI_MENU_PIPE_SKIP_FULLSCREEN};
    opt_choice("TOTK_PIPE_SKIP", UI_MENU_PIPE_SKIP, skip, 3, UI_MENU_PIPE_SKIP_DESC);
    opt_toggle("TOTK_LIB_PREBUILD", UI_MENU_LIB_PREBUILD, UI_MENU_LIB_PREBUILD_DESC);
    row_threads("TOTK_LIB_PREBUILD_THREADS", UI_MENU_LIB_PREBUILD_THREADS, UI_MENU_LIB_PREBUILD_THREADS_DESC);
    opt_toggle("TOTK_PIPE_PREBUILD", UI_MENU_PIPE_PREBUILD, UI_MENU_PIPE_PREBUILD_DESC);
    row_threads("TOTK_PREWARM_XLATE", UI_MENU_PREWARM_XLATE, UI_MENU_PREWARM_XLATE_DESC);
    opt_toggle("TOTK_GPL", UI_MENU_GPL, UI_MENU_GPL_DESC);

    opt_group(UI_MENU_GROUP_TEXMEM);
    static const UiTextId astc[] = {UI_MENU_ASTC_AUTO, UI_MENU_ASTC_GPU, UI_MENU_ASTC_NATIVE, UI_MENU_ASTC_CPU};
    opt_choice("TOTK_ASTC", UI_MENU_ASTC, astc, 4, UI_MENU_ASTC_DESC);
    static const UiTextId texq[] = {UI_MENU_TEX_QUALITY_AUTO, UI_MENU_TEX_QUALITY_FULL, UI_MENU_TEX_QUALITY_HALF,
                                    UI_MENU_TEX_QUALITY_QUARTER};
    opt_choice("TOTK_TEX_QUALITY", UI_MENU_TEX_QUALITY, texq, 4, UI_MENU_TEX_QUALITY_DESC);
    static const UiTextId mem[] = {UI_MENU_MEMORY_AUTO, UI_MENU_MEMORY_IMPORT, UI_MENU_MEMORY_MIRROR,
                                   UI_MENU_MEMORY_VERIFY};
    opt_choice("TOTK_MEMORY", UI_MENU_MEMORY, mem, 4, UI_MENU_MEMORY_DESC);
    opt_toggle("TOTK_TEX_ALIAS", UI_MENU_TEX_ALIAS, UI_MENU_TEX_ALIAS_DESC);
    opt_slider("TOTK_TEX_VRAM_PRESSURE", UI_MENU_TEX_VRAM_PRESSURE, 0, 100, UI_MENU_TEX_VRAM_PRESSURE_DESC, 1, "%.0f%%");
    static const UiTextId astc_bc[] = {UI_MENU_AUTO, UI_MENU_OFF, UI_MENU_ON};   // unset, 0, 1
    opt_choice("TOTK_ASTC_BC", UI_MENU_ASTC_BC, astc_bc, 3, UI_MENU_ASTC_BC_DESC);

    opt_group(UI_MENU_GROUP_FIXES);
    opt_toggle("TOTK_LOD_GUARD", UI_MENU_LOD_GUARD, UI_MENU_LOD_GUARD_DESC);
    opt_toggle("TOTK_FIX_C0", UI_MENU_FIX_C0, UI_MENU_FIX_C0_DESC);
    opt_toggle("TOTK_FIX_ZERO", UI_MENU_FIX_ZERO, UI_MENU_FIX_ZERO_DESC);
    opt_toggle("TOTK_BIND_SKIP", UI_MENU_BIND_SKIP, UI_MENU_BIND_SKIP_DESC);
    opt_toggle("TOTK_SHADER_MEMO", UI_MENU_SHADER_MEMO, UI_MENU_SHADER_MEMO_DESC);
    opt_toggle("TOTK_PIPE_MEMO", UI_MENU_PIPE_MEMO, UI_MENU_PIPE_MEMO_DESC);
    opt_toggle("TOTK_TEX_MEMO", UI_MENU_TEX_MEMO, UI_MENU_TEX_MEMO_DESC);
    opt_toggle("TOTK_DESC_TEMPLATE", UI_MENU_DESC_TEMPLATE, UI_MENU_DESC_TEMPLATE_DESC);
    opt_toggle("TOTK_PUSH_SKIP", UI_MENU_PUSH_SKIP, UI_MENU_PUSH_SKIP_DESC);
    opt_slider("TOTK_SPLIT_DRAWS", UI_MENU_SPLIT_DRAWS, 0, 1000, UI_MENU_SPLIT_DRAWS_DESC, 8, "%.0f");

    opt_group(UI_MENU_GROUP_VULKAN);
    if (!validation_layer_present() && !cfg_bool("TOTK_VK_VALIDATION")) opt_disable_next(UI_MENU_VK_VALIDATION_MISSING);
    opt_toggle("TOTK_VK_VALIDATION", UI_MENU_VK_VALIDATION, UI_MENU_VK_VALIDATION_DESC);
    static const UiTextId dyn[] = {UI_MENU_AUTO, UI_MENU_OFF, UI_MENU_ON};   // unset, 0, 1
    opt_choice("TOTK_VK_DYN_VERTEX", UI_MENU_VK_DYN_VERTEX, dyn, 3, UI_MENU_VK_DYN_VERTEX_DESC);
    opt_choice("TOTK_VK_DYN_A2C", UI_MENU_VK_DYN_A2C, dyn, 3, UI_MENU_VK_DYN_A2C_DESC);
    opt_toggle("TOTK_VK_SUBGROUP_PIN", UI_MENU_VK_SUBGROUP_PIN, UI_MENU_VK_SUBGROUP_PIN_DESC);

    opt_group(UI_MENU_GROUP_DIAG);
    row_set_or_not("TOTK_TRACE", UI_MENU_TRACE, UI_MENU_TRACE_DESC, "perf");
    opt_toggle("TOTK_FLIGHT", UI_MENU_FLIGHT, UI_MENU_FLIGHT_DESC);
    opt_slider("TOTK_FLIGHT_SPIKE_MS", UI_MENU_FLIGHT_SPIKE_MS, 20, 1000, UI_MENU_FLIGHT_SPIKE_MS_DESC, 10, "%.0f");
    opt_choice("TOTK_PASS_CRUMBS", UI_MENU_PASS_CRUMBS, dyn, 3, UI_MENU_PASS_CRUMBS_DESC);   // vk_crumbs.c pass marks
    static char rec_path[600];   // the recording goes beside the log
    if (!rec_path[0]) {
        char dir[512];
        snprintf(rec_path, sizeof rec_path, "%s/input-record.txt", sysinfo_log_folder(dir, sizeof dir) ? dir : ".");
    }
    row_set_or_not("TOTK_INPUT_RECORD", UI_MENU_INPUT_RECORD, UI_MENU_INPUT_RECORD_DESC, rec_path);
    if (opt_button("##open_log", UI_MENU_OPEN_LOG, UI_MENU_OPEN_LOG_DESC, action_value(log_note, UI_MENU_OPEN_LOG))) {
        char dir[512];
        bool ok = sysinfo_log_folder(dir, sizeof dir) && plat_open_folder(dir);
        if (!ok) action_done(log_note, false);   // success shows as the opened folder
    }
    if (opt_button("##copy_info", UI_MENU_COPY_INFO, UI_MENU_COPY_INFO_DESC,
                   action_value(copy_note, UI_MENU_COPY_INFO_DONE))) {
        static char text[16384];
        action_done(copy_note, sysinfo_text(text, sizeof text) > 0 && plat_clipboard_set(text));
    }
    bool ask = opt_button("##reset", UI_MENU_RESET, UI_MENU_RESET_DESC);
    opt_end();
    if (ask) ImGui::OpenPopup("##reset_confirm");   // at the level BeginPopupModal looks it up

    // the confirmation, over the menu; Cancel has the cursor first
    g_modal = false;
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("##reset_confirm", nullptr,
                               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize)) {
        g_modal = true;
        ImGui::TextUnformatted(menu_text(UI_MENU_RESET_CONFIRM));
        ImGui::Spacing();
        const char *yes = menu_text(UI_MENU_RESET_YES), *no = menu_text(UI_MENU_RESET_NO);
        const ImGuiStyle &st = ImGui::GetStyle();
        float bw = fmaxf(ImGui::CalcTextSize(yes).x, ImGui::CalcTextSize(no).x) + st.FramePadding.x * 4;
        bool close = false;
        if (ImGui::Button(no, ImVec2(bw, 0))) close = true;
        if (ImGui::IsWindowAppearing()) ImGui::SetItemDefaultFocus();
        ImGui::SameLine();
        if (ImGui::Button(yes, ImVec2(bw, 0))) {
            reset_all();
            close = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))
            close = true;
        if (close) {
            ImGui::CloseCurrentPopup();
            opt_focus_key("##reset");   // back on the row it came from
        }
        ImGui::EndPopup();
    }
}

static const UiTextId kTabNames[TAB_COUNT] = {UI_MENU_TAB_SCREEN,   UI_MENU_TAB_GAME,   UI_MENU_TAB_SOUND,
                                              UI_MENU_TAB_CONTROLS, UI_MENU_TAB_AMIIBO, UI_MENU_TAB_MENU,
                                              UI_MENU_TAB_ADVANCED, UI_MENU_TAB_MODS,   UI_MENU_TAB_CHEATS};
static int g_tab;               // the tab shown
static int g_tab_request = -1;  // select this tab at the next layout; -1 none
static int g_focus_page = -1;   // this tab's first row takes the cursor once it is shown; -1 none

// A row of another tab: that tab opens with the row under the cursor.
static void go_to(int tab, const char *key) {
    g_tab_request = tab;
    opt_focus_key(key);
}

// ---- Mods: the mods folder (mods.c) with switches and the load order, then the port's own patches. The folder was
// read at start; the rows change the order and switches for the next start.
static void mods_builtin_rows(void) {
    opt_group(UI_MENU_MODS_BUILTIN);
    char fps[64], res[64];
    const char *lim = cfg_value("TOTK_FPS_LIMIT");
    if (!strcmp(lim, "display")) snprintf(fps, sizeof fps, "%s", menu_text(UI_MENU_FPS_DISPLAY));
    else snprintf(fps, sizeof fps, "%s FPS", lim);
    if (opt_button("##mods_fps60", UI_MENU_MODS_FPS60, UI_MENU_MODS_FPS60_DESC, fps)) go_to(TAB_GAME, "TOTK_FPS_LIMIT");
#ifdef GAME_OOT   // OoT: the real-time step is its only built-in patch with a switch
    (void)res;
    opt_toggle("TOTK_REALTIME", UI_MENU_REALTIME, UI_MENU_REALTIME_DESC);
#else
    int w = 0, h = 0;
    sscanf(cfg_value("TOTK_OUTPUT_RES"), "%dx%d", &w, &h);
    snprintf(res, sizeof res, "%d × %d", w, h);   // U+00D7, as row_output_res shows it
    if (opt_button("##mods_res", UI_MENU_MODS_RES, UI_MENU_MODS_RES_DESC, res)) go_to(TAB_SCREEN, "TOTK_OUTPUT_RES");
    opt_toggle("TOTK_REALTIME", UI_MENU_REALTIME, UI_MENU_REALTIME_DESC);
    opt_toggle("TOTK_CLOTH_REALTIME", UI_MENU_CLOTH_REALTIME, UI_MENU_CLOTH_REALTIME_DESC);
    opt_toggle("TOTK_HIGHFPS_FIX", UI_MENU_HIGHFPS_FIX, UI_MENU_HIGHFPS_FIX_DESC);
    opt_button("##mods_quickmenu", UI_MENU_MODS_QUICKMENU, UI_MENU_MODS_QUICKMENU_DESC,
               menu_text(romfs_patches_loaded() ? UI_MENU_ON : UI_MENU_OFF));
#endif
}

static void tab_mods(float scale) {
    opt_begin("##mods", scale);
    opt_group(UI_MENU_MODS_INSTALLED);
    int n = mods_count();
    if (!n) opt_text(UI_MENU_MODS_NONE);
    const char *after = mods_changed() ? menu_text(UI_MENU_AFTER_RESTART) : nullptr;
    for (int i = 0; i < n; i++) {
        const ModInfo *m = mods_at(i);
        char id[32], desc[1536];
        snprintf(id, sizeof id, "##mod%d", i);
        char over[512] = "", other[512] = "";
        if (m->overridden) snprintf(over, sizeof over, menu_text(UI_MENU_MODS_OVERRIDDEN), m->overridden, m->overridden_by);
        if (m->other_version) snprintf(other, sizeof other, menu_text(UI_MENU_MODS_OTHER_VERSION), m->other_version);
        char cheat[256] = "";
        if (m->kind & MOD_CHEATS) {   // its cheats as read at start, or the file they come from
            int count = 0;
            for (int j = 0; j < cheats_count(); j++)
                count += !strcmp(cheats_info(j)->mod, m->name) && cheats_info(j)->kind != CHEAT_HEADING;
            if (count) snprintf(cheat, sizeof cheat, menu_text(UI_MENU_MODS_CHEATS), count);
            else snprintf(cheat, sizeof cheat, menu_text(UI_MENU_MODS_CHEATS_FILE), cheats_file());
        }
        char code[512] = "", problem[512] = "";
        if (m->nsets && m->overrides[0])
            snprintf(code, sizeof code, menu_text(UI_MENU_MODS_REPLACES_BUILTIN), m->nsets, m->overrides);
        else if (m->nsets)
            snprintf(code, sizeof code, menu_text(UI_MENU_MODS_EXEFS_ON), m->nsets);
        switch (m->problem) {
        case MOD_EMPTY: snprintf(problem, sizeof problem, "%s", menu_text(UI_MENU_MODS_EMPTY)); break;
        case MOD_PLUGIN: snprintf(problem, sizeof problem, "%s", menu_text(UI_MENU_MODS_SUBSDK)); break;
        case MOD_EXEFS_LATER: snprintf(problem, sizeof problem, "%s", menu_text(UI_MENU_MODS_EXEFS_LATER)); break;
        case MOD_EXEFS_UNSUPPORTED:
            snprintf(problem, sizeof problem, menu_text(UI_MENU_MODS_EXEFS_UNSUPPORTED), m->note);
            break;
        case MOD_EXEFS_OTHER: snprintf(problem, sizeof problem, "%s", menu_text(UI_MENU_MODS_EXEFS_OTHER)); break;
        case MOD_BAD_NAME: snprintf(problem, sizeof problem, "%s", menu_text(UI_MENU_MODS_BAD_NAME)); break;
        default: break;
        }
        int k = snprintf(desc, sizeof desc, menu_text(UI_MENU_MODS_ROW_DESC), m->replaced, m->added);
        const char *parts[] = {over, other, cheat, code, problem, menu_text(UI_MENU_MODS_MOVE_HINT)};
        for (const char *part : parts)
            if (*part && k >= 0 && k < (int)sizeof desc) k += snprintf(desc + k, sizeof desc - k, "\n\n%s", part);
        const char *note = m->enabled && m->problem != MOD_OK ? menu_text(UI_MENU_MODS_UNUSED) : after;
        int s = opt_text_row(id, m->name, menu_text(m->enabled ? UI_MENU_ON : UI_MENU_OFF), note, desc, true);
        if (s == 2 || s == -1 || s == 1) {
            mods_set_enabled(i, !m->enabled);
            opt_save_soon();
        } else if ((s == 3 && i > 0) || (s == 4 && i + 1 < n)) {
            int to = s == 3 ? i - 1 : i + 1;
            mods_move(i, to - i);
            snprintf(id, sizeof id, "##mod%d", to);
            opt_focus_key(id);   // the cursor follows the mod
            opt_save_soon();
        }
    }
    bool rebuild =
        cheats_rebuild_pending() && opt_button("##restart", UI_MENU_CHEATS_REBUILD, UI_MENU_CHEATS_REBUILD_DESC);
    static uint64_t open_failed;   // plat_now_ns until which the button says it failed
    if (opt_button("##mods_open", UI_MENU_MODS_OPEN, UI_MENU_MODS_OPEN_DESC,
                   plat_now_ns() < open_failed ? menu_text(UI_MENU_ACTION_FAILED) : "")) {
        bool ok = mods_dir()[0] != 0;
        if (ok) plat_file_mkdir_p(mods_dir());
        if (!ok || !plat_open_folder(mods_dir())) open_failed = plat_now_ns() + 3000000000ull;
    }
    mods_builtin_rows();
    opt_end();
    if (rebuild) {
        restart_rebuild = true;
        ImGui::OpenPopup("##restart_confirm");
    }
    restart_confirm();
}

// ---- the screen: the band with the tabs (L / R or Q / E switch), the panel with the page's rows, the live picture and
// the selected row's description beside it, "Continue" and "Quit game" under the panel, the button hints at the bottom
// right. B on the gamepad and Esc close it.
static const ImU32 kMenuCream = IM_COL32(246, 239, 214, 255), kMenuDim = IM_COL32(217, 207, 174, 255);
static const ImU32 kMenuInset = IM_COL32(30, 26, 18, 255), kMenuEdge = IM_COL32(140, 118, 72, 255);
static const ImU32 kMenuNote = IM_COL32(255, 210, 122, 255);

// The live picture's place in the window, for the present (gfx_present.c copies the game picture into it under the
// menu): {x, y, w, h} in pixels while the menu shows it.
static int g_preview[4];
static std::atomic<int> g_preview_on;

extern "C" int menu_preview_rect(int out[4]) {
    if (!g_preview_on.load(std::memory_order_relaxed) || !g_open.load(std::memory_order_relaxed)) return 0;
    for (int i = 0; i < 4; i++) out[i] = g_preview[i];
    return 1;
}

// A key or button name in a small cap, as the game draws its button hints: a light disc (or a rounded plate for a
// longer name) with dark text. Returns its width.
static float key_cap(ImDrawList *dl, ImVec2 p, float h, const char *name, bool draw = true) {
    ImFont *font = ImGui::GetFont();
    float fs = h * 0.62f, tw = font->CalcTextSizeA(fs, FLT_MAX, 0, name).x, w = fmaxf(h, tw + h * 0.6f);
    bool left = !strcmp(name, "<"), right = !strcmp(name, ">");   // the D-pad's directions: drawn, the font has no arrows
    if (left || right) w = h;
    if (draw) {
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), kMenuCream, h * 0.5f);
        if (left || right) {
            float cx = p.x + w * 0.5f, cy = p.y + h * 0.5f, r = h * 0.24f, d = left ? -1.0f : 1.0f;
            dl->AddTriangleFilled(ImVec2(cx + d * r, cy), ImVec2(cx - d * r * 0.8f, cy - r), ImVec2(cx - d * r * 0.8f, cy + r),
                                  IM_COL32(27, 18, 11, 255));
        } else {
            dl->AddText(font, fs, ImVec2(p.x + (w - tw) * 0.5f, p.y + (h - fs) * 0.5f), IM_COL32(27, 18, 11, 255), name);
        }
    }
    return w;
}

// A plate button of the game's system menu ("Save", "Load"): khaki, its label centred; the cursor darkens it and frames
// it in white. Navigable.
static bool plate_button(const char *id, const char *label, ImVec2 size, float scale) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
    bool clicked = ImGui::InvisibleButton(id, size, ImGuiButtonFlags_EnableNav);
    ImGui::PopStyleColor();
    bool hot = ImGui::IsItemHovered() || (ImGui::IsItemFocused() && ImGui::GetCurrentContext()->NavCursorVisible);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 q(p.x + size.x, p.y + size.y);
    dl->AddRectFilled(p, q, hot ? kMenuInset : IM_COL32(111, 90, 53, 255));
    if (!hot) opt_grain(dl, p, q, 46);
    dl->AddRect(p, q, kMenuEdge, 0, 0, 1.0f);
    ImFont *font = ImGui::GetFont();
    float fs = ImGui::GetFontSize(), tw = font->CalcTextSizeA(fs, FLT_MAX, 0, label).x;
    dl->AddText(font, fs, ImVec2(p.x + (size.x - tw) * 0.5f, p.y + (size.y - fs) * 0.5f), kMenuCream, label);
    if (hot) opt_select_frame(dl, p, q, scale);
    return clicked;
}

static int tab_index(int tab) {
    for (int i = 0; i < TABS; i++)
        if (kTabs[i] == tab) return i;
    return 0;
}

extern "C" double gfx_output_fps(void);

static void layout_menu(const MenuTarget *t, float scale) {
    const float W = (float)t->width, H = (float)t->height;
    ImGuiIO &io = ImGui::GetIO();
    ImFont *font = ImGui::GetFont();
    const float fs = ImGui::GetFontSize();
    const bool pad = (io.BackendFlags & ImGuiBackendFlags_HasGamepad) != 0;
    if (tab_index(g_tab) == 0 && kTabs[0] != g_tab) g_tab = kTabs[0];   // a tab this game does not show

    // ---- the places
    const float band_h = floorf(H * 0.072f), top = floorf(H * 0.094f), bottom = floorf(H * 0.905f);
    const ImVec2 pa(floorf(W * 0.016f), top), pb(floorf(W * 0.530f), bottom);   // the panel
    const float sx0 = floorf(W * 0.548f), sx1 = floorf(W * 0.984f);            // the side column
    // the live picture: the game picture's shape, as wide as the column, at most 58 % of its height
    float aspect = t->image_h > 0 ? (float)t->image_w / (float)t->image_h : 16.0f / 9;
    float pvw = sx1 - sx0, pvh = floorf(pvw / aspect), pv_max = floorf((bottom - top) * 0.58f);
    if (pvh > pv_max) pvh = pv_max, pvw = floorf(pvh * aspect);
    const ImVec2 va(floorf(sx0 + (sx1 - sx0 - pvw) * 0.5f), top), vb(va.x + pvw, va.y + pvh);
    g_preview[0] = (int)va.x, g_preview[1] = (int)va.y, g_preview[2] = (int)pvw, g_preview[3] = (int)pvh;
    g_preview_on.store(1, std::memory_order_relaxed);

    // ---- behind everything: the game dimmed, but for the live picture
    ImDrawList *bg = ImGui::GetBackgroundDrawList();
    const ImU32 veil = IM_COL32(6, 3, 2, 224);
    bg->AddRectFilled(ImVec2(0, 0), ImVec2(W, va.y), veil);
    bg->AddRectFilled(ImVec2(0, vb.y), ImVec2(W, H), veil);
    bg->AddRectFilled(ImVec2(0, va.y), ImVec2(va.x, vb.y), veil);
    bg->AddRectFilled(ImVec2(vb.x, va.y), ImVec2(W, vb.y), veil);

    // ---- the band: darker at its top, warm at its bottom, a dark line and a soft shadow under it
    bg->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(W, band_h), IM_COL32(84, 66, 44, 255), IM_COL32(84, 66, 44, 255),
                                IM_COL32(122, 99, 62, 255), IM_COL32(122, 99, 62, 255));
    opt_grain(bg, ImVec2(0, 0), ImVec2(W, band_h), 70);
    bg->AddRectFilled(ImVec2(0, band_h), ImVec2(W, band_h + fmaxf(2, floorf(2 * scale))), IM_COL32(26, 17, 10, 255));
    bg->AddRectFilledMultiColor(ImVec2(0, band_h + 2 * scale), ImVec2(W, band_h + 14 * scale), IM_COL32(0, 0, 0, 120),
                                IM_COL32(0, 0, 0, 120), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0));

    // ---- the panel
    bg->AddRectFilled(pa, pb, IM_COL32(42, 29, 18, 255));
    opt_grain(bg, pa, pb, 90);
    bg->AddRect(pa, pb, IM_COL32(70, 52, 31, 255), 0, 0, 1.0f);

    // ---- the tabs, in a window of their own over the band (mouse only; L / R and Q / E switch them)
    int step = g_modal || binds_modal()                                                                  ? 0
             : ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) || ImGui::IsKeyPressed(ImGuiKey_E, false) ? 1
             : ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) || ImGui::IsKeyPressed(ImGuiKey_Q, false) ? -1
                                                                                                         : 0;
    if (step) g_tab_request = kTabs[(tab_index(g_tab) + step + TABS) % TABS];
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(W, band_h), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::Begin("##band", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground |
                                        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus);
    {
        ImDrawList *dl = ImGui::GetWindowDrawList();
        const float tfs = floorf(fs * 0.94f), tab_pad = floorf(fs * 1.05f), cap_h = floorf(fs * 0.86f), cap_gap = floorf(fs * 0.9f);
        const char *lk = pad ? "L" : "Q", *rk = pad ? "R" : "E";
        float widths[TABS], total = 0;
        for (int k = 0; k < TABS; k++) {
            widths[k] = floorf(font->CalcTextSizeA(tfs, FLT_MAX, 0, menu_text(kTabNames[kTabs[k]])).x + tab_pad * 2);
            total += widths[k];
        }
        float capw = key_cap(dl, ImVec2(0, 0), cap_h, lk, false);
        float x = floorf((W - total) * 0.5f);
        key_cap(dl, ImVec2(x - cap_gap - capw, (band_h - cap_h) * 0.5f), cap_h, lk);
        for (int k = 0; k < TABS; k++) {
            ImVec2 a(x, 0), q(x + widths[k], band_h);
            ImGui::SetCursorScreenPos(a);
            ImGui::PushID(k);
            if (ImGui::InvisibleButton("##tab", ImVec2(widths[k], band_h))) g_tab_request = kTabs[k];
            bool hov = ImGui::IsItemHovered();
            ImGui::PopID();
            bool on = kTabs[k] == g_tab;
            if (on) {   // the open tab: a lighter block the height of the band
                dl->AddRectFilledMultiColor(a, q, IM_COL32(150, 121, 66, 255), IM_COL32(150, 121, 66, 255),
                                            IM_COL32(132, 104, 56, 255), IM_COL32(132, 104, 56, 255));
                opt_grain(dl, a, q, 50);
            } else if (hov) {
                dl->AddRectFilled(a, q, IM_COL32(255, 240, 200, 26));
            }
            const char *name = menu_text(kTabNames[kTabs[k]]);
            float tw = font->CalcTextSizeA(tfs, FLT_MAX, 0, name).x;
            ImVec2 tp(floorf(x + (widths[k] - tw) * 0.5f), floorf((band_h - tfs) * 0.5f));
            dl->AddText(font, tfs, ImVec2(tp.x + 1, tp.y + 1), IM_COL32(30, 20, 10, 170), name);
            dl->AddText(font, tfs, tp, on ? IM_COL32(255, 255, 255, 255) : IM_COL32(239, 230, 200, 255), name);
            x += widths[k];
        }
        key_cap(dl, ImVec2(x + cap_gap, (band_h - cap_h) * 0.5f), cap_h, rk);
        // the port version and the game update at the left, what a bug report needs first; a newer release at the right
        char game[16], version[96];
        snprintf(game, sizeof game, "%s", rt_game_version_text());
        snprintf(version, sizeof version, menu_text(UI_MENU_VERSION), rt_port_version, game);
        float sfs = floorf(fs * 0.74f);
        float left_room = floorf((W - total) * 0.5f) - cap_gap * 2 - capw - W * 0.016f;
        ImVec4 lclip(W * 0.016f, 0, W * 0.016f + left_room, band_h);
        float vx = floorf(W * 0.016f);
        const char *vtext = version;
        // The logo stands for the port's name: the mark and the name in cream, the version small after it. Where the
        // tabs leave no room for both, or the atlas has no logo, the line of text alone as before.
        float lh = floorf(band_h * 0.5f), lw = floorf(lh * g_logo_aspect);
#ifdef GAME_OOT
        static const char port[] = "Rekarina ";   // the name the logo spells: this game's port alone shows it
#else
        static const char port[] = "";
#endif
        size_t name_len = strlen(port);
        if (g_logo_ok && name_len && lw + fs * 0.5f < left_room && !strncmp(version, port, name_len)) {
            float ly = floorf((band_h - lh) * 0.5f);
            dl->AddImage(ImGui::GetIO().Fonts->TexID, ImVec2(vx, ly), ImVec2(vx + lw, ly + lh), g_logo_uv[0], g_logo_uv[1],
                         IM_COL32(246, 239, 214, 255));
            vx += lw + floorf(fs * 0.6f);
            vtext = version + name_len;
            sfs = floorf(fs * 0.66f);
        }
        dl->AddText(font, sfs, ImVec2(vx, floorf((band_h - sfs) * 0.5f + (vtext != version ? band_h * 0.06f : 0))), kMenuDim, vtext,
                    NULL, 0, &lclip);
        char newer[32], update[96];
        if (update_note_latest(newer, sizeof newer)) {
            snprintf(update, sizeof update, menu_text(UI_UPDATE_TITLE), newer);
            float uw = font->CalcTextSizeA(sfs, FLT_MAX, 0, update).x;
            dl->AddText(font, sfs, ImVec2(floorf(W * 0.984f - uw), floorf((band_h - sfs) * 0.5f)), kMenuNote, update);
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    if (g_tab_request >= 0) g_focus_page = g_tab_request;   // the new page's first row takes the cursor
    if (g_tab_request >= 0 && g_tab_request != g_tab) opt_desc_clear();

    // ---- the page and the two buttons under the panel: one window, so the cursor moves between them
    const ImGuiStyle &st = ImGui::GetStyle();
    const float inner = floorf(10 * scale), btn_h = floorf(fs * 1.7f), btn_y = floorf(H * 0.925f);
    ImGui::SetNextWindowPos(ImVec2(pa.x, pa.y + inner), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(pb.x - pa.x, btn_y + btn_h + inner * 2 - pa.y - inner), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(inner, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::Begin("##menu", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground |
                                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
                                        ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(2);
    {
        int k = g_tab_request >= 0 ? g_tab_request : g_tab;
        g_tab = k;
        g_tab_request = -1;
        if (g_focus_page == k) {
            opt_focus_first();
            g_focus_page = -1;
        }
        // the page's rows are a NavFlattened child of this window itself: ImGui clips the items of a direct child
        // only, so behind a second child level the rows scrolled out above won D-pad moves from the buttons
        opt_reserve_bottom(ImGui::GetContentRegionAvail().y - (pb.y - pa.y - inner * 2));
        switch (k) {
        case TAB_SCREEN: tab_screen(scale); break;
        case TAB_GAME: tab_game(scale); break;
        case TAB_MODS: tab_mods(scale); break;
        case TAB_SOUND: tab_sound(scale); break;
        case TAB_CONTROLS: tab_controls(scale); break;
        case TAB_AMIIBO: tab_amiibo(scale); break;
        case TAB_MENU: tab_menu(scale); break;
        case TAB_CHEATS:
            if (tab_cheats(scale)) {
                restart_rebuild = true;
                ImGui::OpenPopup("##restart_confirm");
            }
            restart_confirm();
            break;
        case TAB_ADVANCED: tab_advanced(scale); break;
        }
    }
    // "Continue" and "Quit game", plates under the panel
    const char *cont = menu_text(UI_MENU_CONTINUE), *quit = menu_text(UI_MENU_QUIT);
    float bw = floorf(fmaxf(ImGui::CalcTextSize(cont).x, ImGui::CalcTextSize(quit).x) + fs * 2.4f);
    ImGui::SetCursorScreenPos(ImVec2(pa.x + inner, btn_y));
    if (plate_button("##continue", cont, ImVec2(bw, btn_h), scale)) set_open(0);
    ImGui::SameLine(0, floorf(fs * 0.7f));
    if (plate_button("##quit", quit, ImVec2(bw, btn_h), scale)) gfx_window_request_close();
    if (!cfg_last_save_ok()) {   // stays until a save succeeds; the settings hold for the session
        ImGui::SameLine(0, floorf(fs * 0.9f));
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (btn_h - fs) * 0.5f);
        ImGui::TextColored(ImVec4(1.0f, 0.62f, 0.45f, 1), "%s", menu_text(UI_MENU_SAVE_FAILED));
    }
    bool was_modal = g_modal || binds_modal();
    if (g_tab != TAB_ADVANCED && g_tab != TAB_CONTROLS) g_modal = false;
    if (!was_modal && (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)))
        set_open(0);
    ImGui::End();
    (void)st;

    // ---- the side column: the live picture's frame and tags, then the description
    ImDrawList *fg = bg;
    fg->AddRect(ImVec2(va.x - 1, va.y - 1), ImVec2(vb.x + 1, vb.y + 1), kMenuEdge, 0, 0, 1.0f);
    {
        float sfs = floorf(fs * 0.72f), tp = floorf(sfs * 0.45f);
        const char *live = menu_text(UI_MENU_PREVIEW);
        float lw = font->CalcTextSizeA(sfs, FLT_MAX, 0, live).x;
        fg->AddRectFilled(va, ImVec2(va.x + lw + tp * 2, va.y + sfs + tp * 2), kMenuInset);
        fg->AddText(font, sfs, ImVec2(va.x + tp, va.y + tp), kMenuDim, live);
        // the picture's size and the frame rate, bottom right
        char info[96];
        double fps = gfx_output_fps();
        if (fps <= 0 && io.Framerate > 1) fps = io.Framerate;
        snprintf(info, sizeof info, "%d \xc3\x97 %d  \xc2\xb7  %.0f fps", t->image_w, t->image_h, fps);
        float iw = font->CalcTextSizeA(sfs, FLT_MAX, 0, info).x;
        fg->AddRectFilled(ImVec2(vb.x - iw - tp * 2, vb.y - sfs - tp * 2), vb, kMenuInset);
        fg->AddText(font, sfs, ImVec2(vb.x - iw - tp, vb.y - sfs - tp), kMenuDim, info);
    }
    {
        ImVec2 da(sx0, vb.y + floorf(14 * scale)), db(sx1, bottom);
        fg->AddRectFilled(da, db, IM_COL32(49, 35, 24, 240));
        float px = floorf(22 * scale), py = floorf(18 * scale), wrap = db.x - da.x - px * 2;
        const char *label = opt_desc_label(), *text = opt_desc_text();
        float y = da.y + py;
        fg->PushClipRect(da, db, true);
        if (label[0]) {
            fg->AddText(font, fs, ImVec2(da.x + px, y), kMenuCream, label, NULL, wrap);
            y += font->CalcTextSizeA(fs, FLT_MAX, wrap, label).y + floorf(8 * scale);
            fg->AddLine(ImVec2(da.x + px, y), ImVec2(db.x - px, y), IM_COL32(92, 74, 46, 255), 1.0f);
            y += floorf(10 * scale);
        }
        // the prompts row shows the chosen set's buttons as the game will draw them
        if (!strcmp(opt_desc_key(), "TOTK_PROMPTS")) {
            float ph = prompts_preview(fg, da.x + px, y + floorf(6 * scale), wrap, floorf(fs * 2.5f));
            if (ph > 0) y += ph + floorf(22 * scale);
        }
        float dfs = floorf(fs * 0.86f);
        // a long description at a smaller size, down to 70 %, so it stays inside the box
        float room = db.y - py - y, need = font->CalcTextSizeA(dfs, FLT_MAX, wrap, text).y;
        if (need > room && room > 0) dfs = floorf(fmaxf(dfs * sqrtf(room / need), fs * 0.6f));
        fg->AddText(font, dfs, ImVec2(da.x + px, y), kMenuDim, text, NULL, wrap);
        fg->PopClipRect();
    }

    // ---- the button hints, bottom right
    {
        struct Hint { UiTextId text; const char *k0, *k1; };
        const Hint hints[] = {{UI_MENU_HINT_TAB, pad ? "L" : "Q", pad ? "R" : "E"},
                              {UI_MENU_HINT_CHANGE, "<", ">"},
                              {UI_MENU_HINT_DEFAULT, pad ? "Y" : "Del", nullptr},
                              {UI_MENU_HINT_BACK, pad ? "B" : "Esc", nullptr}};
        float hfs = floorf(fs * 0.8f), cap_h = floorf(hfs * 1.15f), gap = floorf(hfs * 0.45f), between = floorf(hfs * 1.6f);
        float x = sx1, y = floorf(H * 0.925f + (floorf(fs * 1.7f) - cap_h) * 0.5f);
        for (int i = (int)(sizeof hints / sizeof *hints) - 1; i >= 0; i--) {
            const Hint &hn = hints[i];
            float w1 = hn.k1 ? key_cap(fg, ImVec2(0, 0), cap_h, hn.k1, false) : 0;
            float w0 = key_cap(fg, ImVec2(0, 0), cap_h, hn.k0, false);
            const char *text = menu_text(hn.text);
            float tw = font->CalcTextSizeA(hfs, FLT_MAX, 0, text).x;
            if (hn.k1) x -= w1, key_cap(fg, ImVec2(x, y), cap_h, hn.k1), x -= gap * 0.6f;
            x -= w0;
            key_cap(fg, ImVec2(x, y), cap_h, hn.k0);
            x -= gap + tw;
            fg->AddText(font, hfs, ImVec2(x, y + (cap_h - hfs) * 0.5f), kMenuCream, text);
            x -= between;
        }
    }
}

static int g_save_pending;   // a changed setting still waits for its write to totk.ini (opt_save_tick)

// ---- the performance overlay (TOTK_PERF_OVERLAY), in the top right corner. "fps": the frame rate over the presents of
// the last half second. "full": a panel of perfmon.c's numbers, in the style of SavageWindRecomp's perf_overlay_draw
// (menu.mm): the machine's load and temperatures, the frame rate and frame times with a graph, the render thread's waits.
// Off, it costs a cfg_version load per present.
static unsigned g_perf_ver = ~0u;
static int g_perf_mode;                  // 0 off, 1 fps, 2 full
static std::atomic<int> g_perf_save;     // F10 changed the mode: totk.ini is written on the present thread
static uint64_t g_perf_last_ns, g_perf_win_ns, g_perf_max_ns;
static int g_perf_frames;
static char g_perf_text[96];

static int perf_mode_of(const char *s) { return !strcmp(s, "full") ? 2 : !strcmp(s, "fps") ? 1 : 0; }

extern "C" void menu_perf_cycle(void) {
    static const char *const modes[] = {"off", "fps", "full"};
    cfg_set("TOTK_PERF_OVERLAY", modes[(perf_mode_of(cfg_value("TOTK_PERF_OVERLAY")) + 1) % 3]);
    g_perf_save.store(1);
}

// The device the game renders on, for perfmon's GPU counters (by LUID), NVML (by name) and ADL (by PCI bus, else
// name); once, before full mode.
static void perf_gpu_once(const MenuTarget *t) {
    static bool done;
    if (done) return;
    done = true;
    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(t->phys, NULL, &ne, NULL);
    VkExtensionProperties *ep = (VkExtensionProperties *)calloc(ne ? ne : 1, sizeof *ep);
    bool has_bus = false;
    if (ep && vkEnumerateDeviceExtensionProperties(t->phys, NULL, &ne, ep) >= 0)
        for (uint32_t i = 0; i < ne && !has_bus; i++) has_bus = !strcmp(ep[i].extensionName, VK_EXT_PCI_BUS_INFO_EXTENSION_NAME);
    free(ep);
    VkPhysicalDevicePCIBusInfoPropertiesEXT bus = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT};
    VkPhysicalDeviceIDProperties id = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES, has_bus ? &bus : nullptr};
    VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id};
    vkGetPhysicalDeviceProperties2(t->phys, &p2);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(t->phys, &mp);
    uint64_t vram = 0;
    for (uint32_t h = 0; h < mp.memoryHeapCount; h++)
        if (mp.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) vram += mp.memoryHeaps[h].size;
    perfmon_set_gpu(p2.properties.deviceName, id.deviceLUIDValid ? id.deviceLUID : NULL, has_bus ? (int)bus.pciBus : -1,
                    (float)(vram >> 20));
}

static void perf_tick(uint64_t now, const MenuTarget *t) {
    if (cfg_version() != g_perf_ver) {
        g_perf_ver = cfg_version();
        int m = perf_mode_of(cfg_value("TOTK_PERF_OVERLAY"));
        if (m != g_perf_mode) {
            g_perf_last_ns = 0, g_perf_text[0] = 0;
            if (m == 2) perf_gpu_once(t);
            perfmon_mode(m);
        }
        g_perf_mode = m;
    }
    if (g_perf_mode != 1) return;
    uint64_t d = now - g_perf_last_ns;
    if (!g_perf_last_ns || d > 2000000000ull) {   // first frame, or a stall the window should not average in
        g_perf_win_ns = g_perf_max_ns = 0;
        g_perf_frames = 0;
    } else {
        g_perf_win_ns += d;
        g_perf_frames++;
        if (d > g_perf_max_ns) g_perf_max_ns = d;
        if (g_perf_win_ns >= 500000000ull) {
            // with frame generation: the frames on the display, marked FG
            double out_fps = gfx_output_fps();
            int n = snprintf(g_perf_text, sizeof g_perf_text, menu_text(UI_PERF_LINE_FPS),
                             out_fps > 0 ? out_fps : g_perf_frames * 1e9 / g_perf_win_ns);
            if (out_fps > 0 && n > 0 && n < (int)sizeof g_perf_text - 4) snprintf(g_perf_text + n, sizeof g_perf_text - n, " FG");
            g_perf_win_ns = g_perf_max_ns = 0;
            g_perf_frames = 0;
        }
    }
    g_perf_last_ns = now;
}

static float perf_gb(float mb) { return mb < 0 ? -1 : mb / 1024; }

// in use.
extern "C" double res_fixed_scene_share(void);
// Fills labels and v (room for 7 rows of 64 bytes) and returns the number of rows.
static int perf_picture(const MenuTarget *t, const UiTextId **labels_out, char v[7][64]) {
    unsigned out[2], scene[2];
    res_sizes(out, scene);
    snprintf(v[0], 64, menu_text(UI_MENU_OV_FMT_SIZE), t->width, t->height);
    snprintf(v[1], 64, menu_text(UI_MENU_OV_FMT_SIZE), (unsigned)t->image_w, (unsigned)t->image_h);
    double share = res_fixed_scene_share();
    if (share > 0)   // the scene is that share of the game image (frame_rate.c)
        snprintf(v[2], 64, menu_text(UI_MENU_OV_FMT_SCENE), (unsigned)(t->image_w * share + 0.5),
                 (unsigned)(t->image_h * share + 0.5), share * 100.0);
    else
        snprintf(v[2], 64, menu_text(UI_MENU_OV_FMT_SCENE), scene[0], scene[1], scene[0] * 100.0 / out[0]);
    if (t->scaler == 2) {
        int s = cfg_int("TOTK_SHARPNESS");
        snprintf(v[3], 64, menu_text(UI_MENU_OV_FMT_FSR), s < 0 ? 0 : s > 100 ? 100 : s);
    } else {
        // FSR enlarges only (vk_fsr_record): an image larger than the window is shrunk by the blit even with FSR selected
        int down = t->scaler == 1 && !strcmp(cfg_value("TOTK_UPSCALER"), "fsr") &&
                   (t->image_w > (int)t->width || t->image_h > (int)t->height);
        snprintf(v[3], 64, "%s",
                 menu_text(down ? UI_MENU_OV_SCALE_DOWN : t->scaler ? UI_MENU_OV_SCALE_BILINEAR : UI_MENU_OV_SCALE_NONE));
    }
    double hz = frame_step_limit_hz();
    snprintf(v[4], 64, "%s", menu_text(UI_MENU_OV_NONE));
    if (hz > 0) snprintf(v[4], 64, menu_text(UI_MENU_OV_FPS), hz);
    const char *pm = present_name(gfx_window_present_mode());
    snprintf(v[5], 64, "%s", pm ? pm : menu_text(UI_MENU_OV_NONE));
    static const UiTextId labels[7] = {UI_MENU_OV_WINDOW, UI_MENU_OV_OUTPUT, UI_MENU_OV_SCENE,
                                       UI_MENU_OV_SCALING, UI_MENU_OV_LIMIT, UI_MENU_OV_PRESENT, UI_MENU_OV_FG_RATE};
    double fg = gfx_output_fps();   // with frame generation: what the display gets, generated frames included
    int rows = fg > 0 ? 7 : 6;
    if (fg > 0) snprintf(v[6], 64, menu_text(UI_MENU_OV_FPS), fg);
    else v[6][0] = 0;
    *labels_out = labels;
    return rows;
}

// The full panel, drawn in the menu's own colours (the description pane's dark wood behind a thin khaki line, cream
// numbers, dim labels, amber for what is measured): the frame rate large with the lows beside it, the frame times of
// the last PERF_HISTORY frames as a thin line with a guide at 16.7 ms (60 frames a second), the load
// of the GPU, its memory, the CPU and the RAM as bars, then the per-frame times and the picture lines. The plate is
// see-through so the game stays visible behind it; the panel is as wide as its widest line.
static void perf_full_draw(const MenuTarget *t, float scale) {
    PerfStats s;
    perfmon_snapshot(&s);
    // with the menu open: under its window (the settings stay readable), else over everything
    ImDrawList *dl = g_open.load(std::memory_order_relaxed) ? ImGui::GetBackgroundDrawList() : ImGui::GetForegroundDrawList();
    ImFont *font = ImGui::GetFont();
    ImFont *bigf = g_font_big ? g_font_big : font;   // the frame rate's own digits
    const float fs = floorf(ImGui::GetFontSize() * 0.7f), sfs = floorf(fs * 0.86f);
    const float big = g_font_big ? g_font_big->FontSize : floorf(fs * 2.5f);
    auto big_w = [&](const char *text) { return bigf->CalcTextSizeA(big, FLT_MAX, 0, text).x; };
    const ImU32 cream = IM_COL32(246, 239, 214, 255), dim = IM_COL32(201, 192, 165, 255), label = IM_COL32(176, 162, 128, 255),
                amber = IM_COL32(217, 149, 31, 255), inset = IM_COL32(24, 20, 14, 150), line = IM_COL32(110, 90, 58, 170);
    auto width = [&](float size, const char *text) { return font->CalcTextSizeA(size, FLT_MAX, 0, text).x; };
    auto fmt = [&](char *out, size_t n, UiTextId id, float v) {
        if (v < 0) snprintf(out, n, "%s", menu_text(UI_MENU_OV_NONE));
        else snprintf(out, n, menu_text(id), v);
    };

    // ---- the lines of text, so the panel can be measured before it is drawn
    char fps[16], low[2][48], frame[2][64];
    // With frame generation the large number is what the display gets, marked FG, and the frames the game renders
    // stand beside it with the factor between the two.
    const double shown = gfx_output_fps();
    const bool generated = shown > 0 && s.fps > 0;
    char rendered[48] = "";
    fmt(fps, sizeof fps, UI_MENU_OV_FMT_FPS, generated ? (float)shown : s.fps);
    if (generated) snprintf(rendered, sizeof rendered, "%.0f FPS × %.0f", s.fps, floor(shown / s.fps + 0.5));
    fmt(low[0], sizeof low[0], UI_MENU_OV_LOW1, s.low1_fps);
    fmt(low[1], sizeof low[1], UI_MENU_OV_LOW01, s.low01_fps);
    {
        char ms[32];
        fmt(ms, sizeof ms, UI_MENU_OV_FRAME_MS, s.frame_ms_avg);
        snprintf(frame[0], sizeof frame[0], "%s %s", menu_text(UI_MENU_OV_FRAME_LABEL), ms);
        fmt(frame[1], sizeof frame[1], UI_MENU_OV_MAX_MS, s.frame_ms_max);
    }
    struct Meter { UiTextId name; float share; char value[32], extra[32]; } meters[4] = {
        {UI_MENU_OV_GPU, s.gpu_pct / 100, "", ""},
        {UI_MENU_OV_VRAM, s.vram_total_mb > 0 ? s.vram_used_mb / s.vram_total_mb : -1, "", ""},
        {UI_MENU_OV_CPU, s.cpu_total_pct / 100, "", ""},
        {UI_MENU_OV_RAM, s.ram_total_mb > 0 ? s.ram_used_mb / s.ram_total_mb : -1, "", ""}};
    fmt(meters[0].value, 32, UI_MENU_OV_FMT_PCT, s.gpu_pct);
    fmt(meters[0].extra, 32, UI_MENU_OV_FMT_TEMP, s.gpu_temp_c);
    fmt(meters[1].value, 32, UI_MENU_OV_FMT_GB, perf_gb(s.vram_used_mb));
    fmt(meters[1].extra, 32, UI_MENU_OV_FMT_GB_OF, perf_gb(s.vram_total_mb));
    fmt(meters[2].value, 32, UI_MENU_OV_FMT_PCT, s.cpu_total_pct);
    fmt(meters[2].extra, 32, UI_MENU_OV_GAME_PCT, s.cpu_game_pct);
    fmt(meters[3].value, 32, UI_MENU_OV_FMT_GB, perf_gb(s.ram_used_mb));
    fmt(meters[3].extra, 32, UI_MENU_OV_FMT_GB_OF, perf_gb(s.ram_total_mb));
    enum { TIMES = 5, ROWS = TIMES + 7 + 1 };   // the times, the picture's rows (7 with frame generation), the version
    static const UiTextId time_labels[TIMES] = {UI_MENU_OV_REPLAY, UI_MENU_OV_GPU_TIME, UI_MENU_OV_PIPE_WAIT,
                                                UI_MENU_OV_SHADER_WAIT, UI_MENU_OV_TEX_UPLOADS};
    const char *row_label[ROWS];
    char row_value[ROWS][64];
    const float times[TIMES] = {s.replay_ms, s.gpu_ms, s.pipe_wait_ms, s.xlate_ms, s.tex_uploads};
    for (int i = 0; i < TIMES; i++) {
        row_label[i] = menu_text(time_labels[i]);
        fmt(row_value[i], 64, i == TIMES - 1 ? UI_MENU_OV_PER_FRAME : UI_MENU_OV_FMT_MS, times[i]);
    }
    const UiTextId *pic_labels;
    int pics = perf_picture(t, &pic_labels, row_value + TIMES);
    for (int i = 0; i < pics; i++) row_label[TIMES + i] = menu_text(pic_labels[i]);
    int rows = TIMES + pics;
    char foot[192], game[16], power[32] = "";
    snprintf(game, sizeof game, "%s", rt_game_version_text());
    if (s.gpu_power_w >= 0) snprintf(power, sizeof power, menu_text(UI_MENU_OV_FMT_POWER), s.gpu_power_w);
    {
        char ver[96];
        snprintf(ver, sizeof ver, menu_text(UI_MENU_VERSION), rt_port_version, game);
        snprintf(foot, sizeof foot, "%s%s%s", s.gpu_name, s.gpu_name[0] && power[0] ? "  \xc2\xb7  " : "", power);
        // the port version and the game update, so a screenshot in a bug report carries both
        row_label[rows] = ver;   // measured with the rest; drawn as the last, dim line
        snprintf(row_value[rows], 64, "%s", "");
        // the panel's width: the widest row (label, a gap, value), the meters with a bar at least 5 em wide, the header
        const float pad = floorf(16 * scale), gap = floorf(fs * 1.4f), lh = floorf(fs * 1.34f);
        float inner = fmaxf(big_w(fps) + width(sfs, " FPS FG  ") + gap +
                                fmaxf(fmaxf(width(sfs, low[0]), width(sfs, low[1])), generated ? width(sfs, rendered) : 0),
                            fmaxf(width(sfs, foot), width(sfs, ver)));
        for (int i = 0; i < rows; i++) inner = fmaxf(inner, width(fs, row_label[i]) + gap + width(fs, row_value[i]));
        float name_w = 0, value_w = 0, extra_w = 0;
        for (const Meter &m : meters) {
            name_w = fmaxf(name_w, width(fs, menu_text(m.name)));
            value_w = fmaxf(value_w, width(fs, m.value));
            extra_w = fmaxf(extra_w, width(sfs, m.extra));
        }
        const float mgap = floorf(fs * 0.6f);
        inner = fmaxf(inner, name_w + value_w + extra_w + mgap * 3 + fs * 5);
        inner = floorf(inner);
        const float graph_h = floorf(64 * scale), sep = floorf(9 * scale);
        float h = pad + big + sep + graph_h + floorf(4 * scale) + lh + sep * 0.5f + 4 * lh + sep + TIMES * lh + sep * 2 + pics * lh +
                  sep + floorf(sfs * 1.3f) * 2 + pad;
        ImVec2 b((float)t->width - floorf(12 * scale), floorf(12 * scale) + floorf(h)), a(b.x - inner - pad * 2, floorf(12 * scale));
        dl->AddRectFilled(a, b, IM_COL32(49, 35, 24, 150));
        dl->AddRect(a, b, IM_COL32(140, 118, 72, 190), 0, 0, 1.0f);
        const float x0 = a.x + pad, x1 = b.x - pad;
        float y = a.y + pad;
        auto text = [&](float size, float x, float ty, ImU32 col, const char *str) { dl->AddText(font, size, ImVec2(floorf(x), floorf(ty)), col, str); };
        auto right = [&](float size, float xr, float ty, ImU32 col, const char *str) { text(size, xr - width(size, str), ty, col, str); };

        // ---- the frame rate, the lows at its right
        dl->AddText(bigf, big, ImVec2(x0, floorf(y - big * 0.08f)), cream, fps);
        float after = x0 + big_w(fps) + fs * 0.35f;
        text(sfs, after, y + big - sfs * 1.32f, label, "FPS");
        if (generated) {   // the FG mark: an amber plate with dark letters, as a switch's lit half
            float tw = width(sfs, "FG"), bx = after + width(sfs, "FPS") + fs * 0.4f, by = floorf(y + big - sfs * 1.42f);
            dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + tw + sfs * 0.7f, by + sfs * 1.25f), amber);
            text(sfs, bx + sfs * 0.35f, by + sfs * 0.1f, IM_COL32(30, 22, 12, 255), "FG");
            right(sfs, x1, y + big * 0.5f - sfs * 1.7f, cream, rendered);
            right(sfs, x1, y + big * 0.5f - sfs * 0.55f, dim, low[0]);
            right(sfs, x1, y + big * 0.5f + sfs * 0.6f, dim, low[1]);
        } else {
            right(sfs, x1, y + big * 0.5f - sfs * 1.12f, dim, low[0]);
            right(sfs, x1, y + big * 0.5f + sfs * 0.06f, dim, low[1]);
        }
        y += big + sep;

        // ---- the frame times of the last PERF_HISTORY frames, one thin line, on the scale the panel always had: 50 ms
        // at the top, or the slowest frame held when that is more. On so tall a scale the line lies calm and a hitch
        // stands out as a spike.
        {
            ImVec2 ga(x0, y), gb(x1, y + graph_h);
            dl->AddRectFilled(ga, gb, inset);
            const float top = s.frame_ms_max > 50 ? s.frame_ms_max : 50;
            auto gy = [&](float ms) { return gb.y - fminf(ms / top, 1) * (graph_h - 4) - 2; };
            float ly = floorf(gy(16.7f)) + 0.5f;   // 60 frames a second, the game's own rate
            dl->AddLine(ImVec2(ga.x, ly), ImVec2(gb.x, ly), line, 1.0f);
            {
                char g[24];
                snprintf(g, sizeof g, menu_text(UI_MENU_OV_FRAME_MS), 16.7f);
                float gfs = floorf(sfs * 0.82f);
                right(gfs, gb.x - 4 * scale, ly - gfs - 2, IM_COL32(176, 162, 128, 210), g);
            }
            const float step = (gb.x - ga.x - 2) / (PERF_HISTORY - 1);
            ImVec2 pts[PERF_HISTORY];
            for (int i = 0; i < PERF_HISTORY; i++) pts[i] = ImVec2(ga.x + 1 + i * step, gy(s.frame_ms[i]));
            dl->AddPolyline(pts, PERF_HISTORY, amber, 0, fmaxf(1.0f, 1.2f * scale));
            y += graph_h + floorf(4 * scale);
        }
        text(sfs, x0, y, dim, frame[0]);
        right(sfs, x1, y, dim, frame[1]);
        y += lh + sep * 0.5f;

        // ---- the machine: a bar each
        {
            const float bx0 = x0 + name_w + mgap, bx1 = x1 - value_w - extra_w - mgap * 2, bh = floorf(fs * 0.62f);
            for (const Meter &m : meters) {
                text(fs, x0, y, label, menu_text(m.name));
                float by = floorf(y + (fs - bh) * 0.5f) + 1;
                dl->AddRectFilled(ImVec2(bx0, by), ImVec2(bx1, by + bh), inset);
                if (m.share > 0)
                    dl->AddRectFilledMultiColor(ImVec2(bx0 + 1, by + 1), ImVec2(bx0 + 1 + (bx1 - bx0 - 2) * fminf(m.share, 1), by + bh - 1),
                                                amber, amber, IM_COL32(196, 130, 27, 255), IM_COL32(196, 130, 27, 255));
                right(fs, x1 - extra_w - mgap, y, cream, m.value);
                right(sfs, x1, y + (fs - sfs) * 0.6f, dim, m.extra);
                y += lh;
            }
        }
        // ---- per frame, then the picture, a hairline before each
        for (int i = 0; i < rows; i++) {
            if (i == 0 || i == TIMES) {
                y += sep * (i ? 0.5f : 0.4f);
                dl->AddLine(ImVec2(x0, floorf(y) + 0.5f), ImVec2(x1, floorf(y) + 0.5f), line, 1.0f);
                y += sep * (i ? 0.5f : 0.6f);
            }
            text(fs, x0, y, label, row_label[i]);
            right(fs, x1, y, cream, row_value[i]);
            y += lh;
        }
        y += sep;
        text(sfs, x0, y, label, foot);
        text(sfs, x0, y + floorf(sfs * 1.3f), label, ver);
    }
}

// The notice at the top of the picture while the game holds more video memory than the system leaves it (vk_memory.c
// vk_mem_vram_short): two lines on a dark plate, whatever the overlay setting.
extern "C" int vk_mem_vram_short(double *used_gb, double *budget_gb);
static bool g_vram_short;

static void vram_notice_draw(const MenuTarget *t, float scale) {
    double used, budget;
    if (!vk_mem_vram_short(&used, &budget)) return;
    char line[2][256];
    snprintf(line[0], sizeof line[0], menu_text(UI_VRAM_SHORT), used, budget);
    snprintf(line[1], sizeof line[1], "%s", menu_text(UI_VRAM_SHORT_HINT));
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    ImVec2 a = ImGui::CalcTextSize(line[0]), b = ImGui::CalcTextSize(line[1]);
    float pad = 10 * scale, w = (a.x > b.x ? a.x : b.x) + pad * 2, h = a.y + b.y + pad * 2 + 4 * scale;
    ImVec2 p(((float)t->width - w) / 2, 14 * scale);
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32(20, 12, 0, 215), 6 * scale);
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), IM_COL32(240, 190, 84, 230), 6 * scale, 0, 1.5f * scale);
    dl->AddText(ImVec2(p.x + (w - a.x) / 2, p.y + pad), IM_COL32(255, 214, 120, 255), line[0]);
    dl->AddText(ImVec2(p.x + (w - b.x) / 2, p.y + pad + a.y + 4 * scale), IM_COL32(240, 236, 216, 255), line[1]);
}

// The update notice (update_note.c): two lines on a plate at the top left for the first seconds after the check found a
// newer release, whatever the overlay setting; the menu's title line keeps saying so afterwards.
enum { UPDATE_NOTICE_NS_S = 12 };
static uint64_t g_update_since;   // plat_now_ns of the notice's first frame; 0 before

static bool update_notice_active(void) {
    if (!update_note_latest(nullptr, 0)) return false;
    uint64_t now = plat_now_ns();
    if (!g_update_since) g_update_since = now;
    return now - g_update_since < UPDATE_NOTICE_NS_S * 1000000000ull;
}

static void update_notice_draw(float scale) {
    char version[32], line[2][256];
    if (!update_note_latest(version, sizeof version)) return;
    snprintf(line[0], sizeof line[0], menu_text(UI_UPDATE_AVAILABLE), version);
    snprintf(line[1], sizeof line[1], "%s", menu_text(UI_UPDATE_HINT));
    // fades out over the last second
    float left = UPDATE_NOTICE_NS_S - (float)((plat_now_ns() - g_update_since) / 1e9), a = left < 1 ? (left < 0 ? 0 : left) : 1;
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    ImVec2 s0 = ImGui::CalcTextSize(line[0]), s1 = ImGui::CalcTextSize(line[1]);
    float pad = 12 * scale, w = (s0.x > s1.x ? s0.x : s1.x) + pad * 2, h = s0.y + s1.y + pad * 2 + 4 * scale;
    ImVec2 p(18 * scale, 18 * scale);
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32(49, 35, 24, (int)(240 * a)));
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), IM_COL32(140, 118, 72, (int)(255 * a)), 0, 0, 1.0f);
    dl->AddText(ImVec2(p.x + pad, p.y + pad), IM_COL32(255, 210, 122, (int)(255 * a)), line[0]);
    dl->AddText(ImVec2(p.x + pad, p.y + pad + s0.y + 4 * scale), IM_COL32(246, 239, 214, (int)(255 * a)), line[1]);
}

static void perf_draw(const MenuTarget *t, float scale) {
    if (g_perf_mode == 2) return perf_full_draw(t, scale);
    float w = (float)t->width;
    if (!g_perf_mode || !g_perf_text[0]) return;
    // with the menu open: under its window, over the dimming layout_menu drew before
    ImDrawList *dl = g_open.load(std::memory_order_relaxed) ? ImGui::GetBackgroundDrawList() : ImGui::GetForegroundDrawList();
    // no plate: cream text over the picture with a dark edge around it, so it reads on a bright sky too
    ImFont *font = ImGui::GetFont();
    float fs = floorf(ImGui::GetFontSize() * 0.92f), e = fmaxf(1, floorf(1.5f * scale));
    ImVec2 sz = font->CalcTextSizeA(fs, FLT_MAX, 0, g_perf_text);
    ImVec2 p(floorf(w - 16 * scale - sz.x), floorf(12 * scale));   // the top right corner: the game's HUD leaves it empty
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++)
            if (dx || dy) dl->AddText(font, fs, ImVec2(p.x + dx * e, p.y + dy * e), IM_COL32(20, 13, 6, 150), g_perf_text);
    dl->AddText(font, fs, p, IM_COL32(246, 239, 214, 255), g_perf_text);
}

extern "C" int menu_overlay_frame(uint64_t frame, const MenuTarget *t, const char **shot) {
    *shot = NULL;
    cheats_frame(frame);   // the data cheats and the buttons of the cheats that need them held
    if (!g_test.parsed) test_parse();
    if (g_test.next_set < g_test.n_sets && frame >= g_test.sets[g_test.next_set].frame) test_apply_sets(frame);
    if (g_perf_save.exchange(0)) cfg_save();
    perf_tick(plat_now_ns(), t);
    amiibo_tick();
    {
        double used, budget;
        g_vram_short = vk_mem_vram_short(&used, &budget) != 0;
    }
    bool open = g_open.load(std::memory_order_relaxed);
    if (!open) {
        if (g_save_pending) g_save_pending = opt_save_tick(1);   // closed: the last change goes to totk.ini now
        if (g_test.at && !g_test.opened && frame >= g_test.at) {
            g_test.opened = frame;
            g_test.page = 0;
            g_tab_request = kTabs[0];
            set_open(1);
            open = true;
        } else if (!g_perf_mode && !g_vram_short && !update_notice_active()) {
            return 0;
        }
    }
    if (g_failed_flag()) {
        set_open(0);
        return 0;
    }
    uint64_t t0 = plat_now_ns();
    if (!g_ctx_ok) make_context();
    float scale = menu_scale(t->height);
    if (!vk_up(t) || ((g_font_scale != scale || strcmp(g_font_lang, menu_text_lang())) && !fonts_up(scale))) {
        rt_log("menu: Dear ImGui Vulkan back end set-up failed; the menu stays closed");
        g_failed.store(1);
        set_open(0);
        return 0;
    }
    if (g_style_scale != scale) {
        ImGui::GetStyle() = g_base_style;
        ImGui::GetStyle().ScaleAllSizes(scale);
        g_style_scale = scale;
    }
    list_gpus(t->instance);
    ImGuiIO &io = ImGui::GetIO();
    int reset = g_reset.exchange(0);
    if (reset) {
        io.ClearInputKeys();
        io.ClearInputMouse();
        g_last_ns = 0;
        g_focus_page = g_tab_request >= 0 ? g_tab_request : g_tab;   // opening: the first row has the cursor
        if (GImGui->OpenPopupStack.Size) ImGui::ClosePopupToLevel(0, false);   // a confirmation left open on closing
        g_modal = false;
    }
    io.DisplaySize = ImVec2((float)t->width, (float)t->height);
    double dt = g_last_ns ? (t0 - g_last_ns) / 1e9 : 1 / 60.0;
    io.DeltaTime = (float)(dt < 1e-4 ? 1e-4 : dt > 0.1 ? 0.1 : dt);
    g_last_ns = t0;
    if (!open) {   // the overlay alone: no input reaches ImGui
    } else if (g_test.page < 0) {
        drain_events(io);
        pad_events(io, reset);
    } else {   // the test hook's pages: the user's input in a background test window must not change settings
        plat_mutex_lock(&ev_lock);
        ev_n = 0;
        plat_mutex_unlock(&ev_lock);
    }
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    if (open) layout_menu(t, scale);
    else g_preview_on.store(0, std::memory_order_relaxed);
    perf_draw(t, scale);
    if (g_vram_short) vram_notice_draw(t, scale);
    if (update_notice_active()) update_notice_draw(scale);
    ImGui::Render();
    if (open) g_save_pending = opt_save_tick(0);
    g_cost_ns += plat_now_ns() - t0;
    if (g_test.page >= 0 && frame >= g_test.opened + g_test.per_page * (uint64_t)(g_test.page + 1)) {
        static char path[512];   // this frame's image goes to a file; then the next tab, or the menu closes
        const char *dir = cfg_get("TOTK_MENU_TEST_DIR");
        snprintf(path, sizeof path, "%s/menu-test-%llu-%d.png", dir ? dir : ".", (unsigned long long)frame, g_test.page);
        *shot = path;
        if (++g_test.page == TABS) {
            g_test.page = -1;
            set_open(0);   // the next frame lays out nothing
        } else {
            g_tab_request = kTabs[g_test.page];
        }
    }
    return ImGui::GetDrawData()->TotalVtxCount > 0;
}

extern "C" void menu_overlay_draw(VkCommandBuffer cb) {
    uint64_t t0 = plat_now_ns();
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cb);
    g_cost_ns += plat_now_ns() - t0;
    if (++g_cost_frames == 600) {
        rt_log("menu: %.3f ms CPU per open frame (layout and recording, mean of 600)", g_cost_ns / 600 / 1e6);
        g_cost_ns = g_cost_frames = 0;
    }
}
