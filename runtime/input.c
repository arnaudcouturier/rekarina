// Host input for nn::hid (runtime/ov_input.c): an SDL3 gamepad (no SDL window needed), the keyboard and the mouse,
// merged into Switch npad button bits and stick axes. Button order follows libnx HidNpadButton (ISC). The key and pad
// bindings are the tables in input_map.c (rekarina.ini overrides them). The keyboard and the mouse come from the window
// system's back end (input_wsys.h: input_win.c with Win32, input_sdl.c with SDL), which also owns the cursor capture.
// Keys and their map: docs/controls.md.
// The gamepad also rumbles (input_rumble) and, when it has a gyro and an accelerometer, feeds the six-axis sensor from a
// thread of its own (input_motion_start). Every SDL call on the gamepad is made under `lock`.
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <SDL3/SDL.h>

#include "cfg.h"
#include "fs.h"
#include "host.h"
#include "input.h"
#include "input_map.h"
#include "input_wsys.h"
#include "nvn_api.h"
#include "ui/menu.h"
#include "vk.h"
#include "platform/platform.h"

enum { STICK_MAX = 32767, STICK_DIR = 16384, TRIGGER_ON = 16384, RESCAN_MS = 1000 };

static PlatMutex lock = PLAT_MUTEX_INIT;
static int state;                    // 0 not started, 1 running, -1 disabled or SDL failed
static volatile int32_t g_block, g_menu;   // text entry / host menu: 0 off, 1 open, 2 waiting for release
// The device the player used last (input_kbm_last): 1 keyboard and mouse, 0 the gamepad. A pad counts once a button is
// down or a stick is past half way (a drifting stick never switches), the mouse once it moved MOUSE_USED raw counts
// within one poll (a slow deliberate move does; a desk bump does not), the keyboard with any key or mouse button.
enum { PAD_USED = 16384, MOUSE_USED = 6 };
static volatile int32_t g_kbm_last = 1;
int input_kbm_last(void) { return __atomic_load_n(&g_kbm_last, __ATOMIC_RELAXED); }
static SDL_Gamepad *pad;
static uint64_t next_scan;
static int pad_rumble;               // the open gamepad has rumble motors
static volatile int32_t pad_labels[4];  // ... and the labels of its south, east, west, north buttons (PAD_LABEL_*), any thread
static int pad_east_a;               // ... and labels its east face button A (Nintendo layout under REKARINA_PAD_LAYOUT=auto)
static volatile int32_t pad_family = -1;   // ... and the kind of its labels (input_pad_family), -1 none; any thread
static int pad_layout = -1;          // PAD_LAYOUT_* in effect for the open gamepad, -1 to pick again; live (cfg_version)
static unsigned pad_layout_seen;
static volatile int32_t pad_layout_shown = -1;   // pad_layout for the menu (any thread), -1 without a gamepad
static volatile int32_t pad_hd;      // ... is a Switch Pro Controller or Joy-Con that takes HD rumble packets; any thread
static uint64_t hd_until;            // gamepad thread: when the last HD packet's duration runs out (ms), 0 none running
// Gamepad thread: the rumble written to the open gamepad (pad_main): when (ms, 0 none yet), the two motors' values, and
// the writes in the current second and the one before, for the log line of a gamepad that went away (scan_pads).
static uint64_t rumble_at, rumble_sec;
static uint16_t rumble_low, rumble_high;
static uint32_t rumble_n, rumble_n_prev;
static void rumble_wrote(uint64_t now, uint16_t low, uint16_t high) {
    if (now / 1000 != rumble_sec) {
        rumble_n_prev = now / 1000 == rumble_sec + 1 ? rumble_n : 0;
        rumble_n = 0, rumble_sec = now / 1000;
    }
    rumble_n++;
    rumble_at = now, rumble_low = low, rumble_high = high;
}
static SDL_JoystickID sensor_pad;    // the gamepad whose sensors the motion thread set up, 0 none
static int sensor_ok;                // ... and it has both sensors, enabled
static int motion_thread;            // 0 not started, 1 running, -1 off (REKARINA_MOTION=0, no SDL)
static volatile int32_t motion_on;   // input_motion_start ran: the gamepad thread feeds the six-axis sensor
// Once the game runs, the gamepad thread (pad_main) alone calls SDL's gamepad functions. SDL holds its joystick lock while
// a device is enumerated and opened, 280-320 ms a frame each time a pad came or went when the game's poll made those calls.
// The poll reads this snapshot of the last update instead, and a rumble is left here for that thread to send.
static PlatMutex snap_lock = PLAT_MUTEX_INIT;
static struct {
    int on, rumble;                       // a gamepad is connected, and it has rumble motors
    int guide;                            // the Guide button (Xbox, PS button) is down: it opens the menu, input_menu_guide
    uint8_t pd[PAD_COUNT];
    int16_t ax[SDL_GAMEPAD_AXIS_COUNT];
    int want_rumble, want_triggers;       // a rumble waits for the gamepad thread
    uint16_t low, high;
    uint32_t ms;
    int want_hd;                          // ... and it came with the game's two sides (hd)
    InputVibration hd[2];
} snap;
static volatile int32_t pad_layout_stale;   // the gamepad changed: pad_layout_get picks the layout again

// The device the button prompts follow under REKARINA_PROMPTS = auto (input_pad_in_use, input_prompts_wanted): the one the
// player pressed something on last. Only new presses count, so nothing held or resting decides: a gamepad button that
// went down, or a stick that crossed PROMPT_STICK_ON after the dead zone (it counts again once back under
// PROMPT_STICK_OFF, so a stick resting near the line does not count twice and a drifting one, inside its dead zone,
// never); a key or mouse button the game binds (input_map.keys, mouse buttons only while captured) or Esc that went
// down while the game window is in front and no text entry is open. Keys held when the window comes to the front are
// taken as the start (the Tab of an Alt+Tab back). Mouse motion moves the camera and never shows a prompt, so it does
// not count: a Steam Deck's trackpad set up as a mouse keeps the controller's prompts. The other device takes over
// once its press has stayed the newest for PROMPT_SETTLE_NS, so a key brushed while playing on the pad (or a
// handheld's extra button that sends one) does not flip the prompts back and forth. The prompts' own switch
// (romfs.c romfs_prompts_live) then writes the new set into the game's font, each present.
enum { PROMPT_STICK_ON = 19661, PROMPT_STICK_OFF = 8192 };   // 60 % and 25 % of full deflection
#define PROMPT_SETTLE_NS 400000000ull
enum { DEV_NONE = -1, DEV_KBM, DEV_PAD };
static volatile int32_t prompt_dev = DEV_NONE;   // the device the prompts follow, DEV_NONE before the first press
static int prompt_newest = DEV_NONE;             // under lock: the device of the newest press, and when it was
static uint64_t prompt_newest_ns;

// SDL buttons of the gamepad inputs (input_map.h PAD_*), by position; PAD_ZL and PAD_ZR are the triggers (axes).
static const SDL_GamepadButton pad_sdl[PAD_COUNT] = {
    [PAD_SOUTH] = SDL_GAMEPAD_BUTTON_SOUTH, [PAD_EAST] = SDL_GAMEPAD_BUTTON_EAST, [PAD_WEST] = SDL_GAMEPAD_BUTTON_WEST,
    [PAD_NORTH] = SDL_GAMEPAD_BUTTON_NORTH, [PAD_LSTICK] = SDL_GAMEPAD_BUTTON_LEFT_STICK,
    [PAD_RSTICK] = SDL_GAMEPAD_BUTTON_RIGHT_STICK, [PAD_L] = SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,
    [PAD_R] = SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, [PAD_START] = SDL_GAMEPAD_BUTTON_START, [PAD_BACK] = SDL_GAMEPAD_BUTTON_BACK,
    [PAD_DPAD_LEFT] = SDL_GAMEPAD_BUTTON_DPAD_LEFT, [PAD_DPAD_UP] = SDL_GAMEPAD_BUTTON_DPAD_UP,
    [PAD_DPAD_RIGHT] = SDL_GAMEPAD_BUTTON_DPAD_RIGHT, [PAD_DPAD_DOWN] = SDL_GAMEPAD_BUTTON_DPAD_DOWN,
    [PAD_LPADDLE1] = SDL_GAMEPAD_BUTTON_LEFT_PADDLE1, [PAD_RPADDLE1] = SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1,
    [PAD_LPADDLE2] = SDL_GAMEPAD_BUTTON_LEFT_PADDLE2, [PAD_RPADDLE2] = SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2,
};

// REKARINA_INPUT_SCRIPT=<file>: lines "<frame> <button> [hold]" or "<frame> LSTICK|RSTICK <x> <y> [hold]", ORed into the real input.
// <frame> and its hold are 60 Hz ticks of game time since the first present (frame_step_ticks: presents at 60 fps), so
// the title menus get the presses at the same game time at any frame limit. A line "@<ms> ..." is timed instead: <ms>
// and its hold are milliseconds of the world clock (frame_step_world_us, game time since the loaded save's first
// frame), so it lands at the same game time at any frame rate and load length. A timed event shorter than a frame is
// applied once. In a script with timed lines (a recording) the untimed ones belong before the world clock: they stop
// when it starts, as the load ends at another tick in each run. A line "+<ticks> ..." counts from the arrival after the
// latest map change a warp asked for (REKARINA_WARP_AT, oot_warp.c): the first frame that draws a scene after the
// loading screen that followed the warp, so a test route starts at the same place and moment of its map whatever the
// title and the load took (a route timed from the start ran a different path in each arm of a pair).
// at, hold: ticks, or microseconds of the world clock when timed.
typedef struct { uint64_t at, hold; int bit, stick, timed, rel; int32_t x, y; int logged; char text[48]; } ScriptEv;
static const struct { const char *name; int bit; } script_buttons[] = {
    {"A", B_A}, {"B", B_B}, {"X", B_X}, {"Y", B_Y}, {"LCLICK", B_STICK_L}, {"RCLICK", B_STICK_R}, {"L", B_L}, {"R", B_R},
    {"ZL", B_ZL}, {"ZR", B_ZR}, {"PLUS", B_PLUS}, {"MINUS", B_MINUS}, {"LEFT", B_LEFT}, {"UP", B_UP}, {"RIGHT", B_RIGHT}, {"DOWN", B_DOWN},
    {"DASH", 62},   // the dash key (dash_apply, SCRIPT_DASH_BIT); after the 16 the recorder writes
};
static ScriptEv *script_ev;
static int script_n, script_state;   // 0 not loaded, 1 loaded
static int script_mixed;             // the script has timed lines: its untimed ones end where the world clock starts
static int32_t script_mouse[2];      // one-shot raw counts, consumed by the camera hook; under lock

static void script_load(void) {
    script_state = 1;
    const char *path = cfg_get("REKARINA_INPUT_SCRIPT");
    if (!path || !*path) return;
    FILE *f = fopen(path, "r");
    if (!f) { rt_log("input: cannot open REKARINA_INPUT_SCRIPT %s", path); return; }
    char line[256];
    int cap = 0, ln = 0;
    while (fgets(line, sizeof line, f)) {
        ln++;
        char *h = strchr(line, '#');
        if (h) *h = 0;
        char name[32];
        double at;
        int off = 0, timed = line[0] == '@', rel = line[0] == '+', skip = timed || rel;
        if (sscanf(line + skip, "%lf %31s%n", &at, name, &off) < 2 || at < 0) continue;
        off += skip;
        double unit = timed ? 1000.0 : 1.0;   // ms to us, or presents
        ScriptEv e = {.at = (uint64_t)(at * unit + 0.5), .hold = timed ? 66667 : 4, .bit = -1, .timed = timed, .rel = rel};
        long a, b;
        double c;
        if (!strcmp(name, "MOUSE") || !strcmp(name, "LSTICK") || !strcmp(name, "RSTICK")) {
            int n = sscanf(line + off, "%ld %ld %lf", &a, &b, &c);
            if (n < 2) { rt_log("input: script line %d: %s needs x y [hold]", ln, name); continue; }
            e.stick = name[0] == 'M' ? 3 : name[0] == 'L' ? 1 : 2; e.x = (int32_t)a; e.y = (int32_t)b;
            if (e.stick == 3) e.hold = 1;   // a displacement, never a held speed
            if (n >= 3 && c > 0) e.hold = (uint64_t)(c * unit + 0.5);
            snprintf(e.text, sizeof e.text, "%s %d %d", name, e.x, e.y);
        } else {
            for (size_t i = 0; i < sizeof script_buttons / sizeof *script_buttons; i++)
                if (!strcmp(name, script_buttons[i].name)) e.bit = script_buttons[i].bit;
            if (e.bit < 0) { rt_log("input: script line %d: unknown button %s", ln, name); continue; }
            if (sscanf(line + off, "%lf", &c) == 1 && c > 0) e.hold = (uint64_t)(c * unit + 0.5);
            snprintf(e.text, sizeof e.text, "%s", name);
        }
        if (script_n == cap) {
            cap = cap ? cap * 2 : 32;
            script_ev = realloc(script_ev, (size_t)cap * sizeof *script_ev);
            if (!script_ev) { script_n = 0; fclose(f); return; }
        }
        script_ev[script_n++] = e;
        script_mixed |= timed;
    }
    fclose(f);
    rt_log("input: script %s, %d events", path, script_n);
}

// The arrival after the latest warp, for the "+" lines: 0 none yet; ticks + 1 of its first scene frame.
__attribute__((weak)) int oot_warps_made(void) { return 0; }
int perf_scene_frames_ago(void);   // perf.c (a weak 0), vk_texture.c
static uint64_t rel_base;
static int rel_warps, rel_state;   // the warps seen; 1 a warp was made, 2 its loading screen is up

static void script_rel_track(uint64_t fr, uint64_t tick) {
    int w = oot_warps_made();
    if (w != rel_warps) rel_warps = w, rel_state = 1, rel_base = 0;
    int scene = perf_scene_frames_ago() <= 2;
    if (rel_state == 1 && !scene) rel_state = 2;
    else if (rel_state == 2 && scene) {
        rel_state = 0, rel_base = tick + 1;
        rt_log("input: script frame %llu tick %llu: arrival after warp %d: \"+\" lines count from here",
               (unsigned long long)fr, (unsigned long long)tick, w);
    }
}

static void script_apply(uint64_t *b, int32_t sl[2], int32_t sr[2]) {
    if (!script_state) script_load();
    if (!script_n) return;
    uint64_t fr = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED), tick = frame_step_ticks();
    int64_t w = frame_step_world_us();
    script_rel_track(fr, tick);
    for (int i = 0; i < script_n; i++) {
        ScriptEv *e = &script_ev[i];
        if (e->rel && !rel_base) continue;
        if (!e->rel && (e->timed ? w < 0 : script_mixed && w >= 0)) continue;
        uint64_t now = e->timed ? (uint64_t)w : e->rel ? tick + 1 - rel_base : tick;
        if (now < e->at || (now >= e->at + e->hold && (e->logged || !e->timed))) continue;
        if (e->stick == 3 && e->logged) continue;
        if (!e->logged) {
            e->logged = 1;
            if (e->timed) rt_log("input: script frame %llu at %.1f ms: %s", (unsigned long long)fr, w / 1000.0, e->text);
            else rt_log("input: script frame %llu tick %llu: %s", (unsigned long long)fr, (unsigned long long)tick, e->text);
        }
        if (e->stick == 3) {
            int64_t x = (int64_t)script_mouse[0] + e->x, y = (int64_t)script_mouse[1] + e->y;
            script_mouse[0] = (int32_t)(x < INT32_MIN ? INT32_MIN : x > INT32_MAX ? INT32_MAX : x);
            script_mouse[1] = (int32_t)(y < INT32_MIN ? INT32_MIN : y > INT32_MAX ? INT32_MAX : y);
        } else if (e->stick) { int32_t *s = e->stick == 1 ? sl : sr; s[0] = e->x; s[1] = e->y; }
        else *b |= 1ull << e->bit;
    }
}

// REKARINA_INPUT_RECORD=<file>: writes the merged input as REKARINA_INPUT_SCRIPT lines, one per button press and per stick
// segment, so a played session replays as a script. A stick segment ends when an axis moves by more than REC_STICK_STEP.
// An event that begins on the world clock is written timed ("@<ms>", hold in ms), one before it in ticks
// (frame_step_ticks). When the world clock starts, held buttons and sticks are cut there and go on as timed events.
enum { REC_STICK_STEP = 2048 };
static FILE *rec;
static int rec_state;                // 0 not opened, 1 recording, -1 off
static uint64_t rec_down[16];        // tick + 1 at which each button went down, 0 while up
static int64_t rec_down_w[16];       // the world clock then, -1 before it started
static uint64_t rec_stick_from[2];   // tick + 1 at which the current non-zero stick segment began
static int64_t rec_stick_w[2];
static int32_t rec_stick[2][2];
static int64_t rec_last_w = -1;      // the world clock at the previous apply

// "<tick> <what> <hold>" or "@<ms> <what> <hold ms>": from tick from and world clock from_w to now.
static void rec_line(uint64_t from, int64_t from_w, uint64_t fr, int64_t w, const char *what) {
    if (from_w >= 0 && w >= from_w)
        fprintf(rec, "@%.1f %s %.1f\n", from_w / 1000.0, what, (w > from_w ? w - from_w : 1000) / 1000.0);
    else
        fprintf(rec, "%llu %s %llu\n", (unsigned long long)from, what, (unsigned long long)(fr > from ? fr - from : 1));
}

static void rec_close_stick(int s, uint64_t fr, int64_t w) {
    if (!rec_stick_from[s]) return;
    char what[48];
    snprintf(what, sizeof what, "%s %d %d", s ? "RSTICK" : "LSTICK", rec_stick[s][0], rec_stick[s][1]);
    rec_line(rec_stick_from[s] - 1, rec_stick_w[s], fr, w, what);
    rec_stick_from[s] = 0;
}

static void rec_flush_all(void) {
    if (rec_state != 1) return;
    uint64_t fr = frame_step_ticks() + 1;
    int64_t w = frame_step_world_us();
    for (int i = 0; i < 16; i++)
        if (rec_down[i]) { rec_line(rec_down[i] - 1, rec_down_w[i], fr, w, script_buttons[i].name); rec_down[i] = 0; }
    rec_close_stick(0, fr, w);
    rec_close_stick(1, fr, w);
    fflush(rec);
}

static void rec_apply(uint64_t b, const int32_t sl[2], const int32_t sr[2]) {
    if (!rec_state) {
        const char *path = cfg_get("REKARINA_INPUT_RECORD");   // no default: not recording when unset
        rec = path && *path ? fopen(path, "w") : NULL;
        rec_state = rec ? 1 : -1;
        if (path && *path && !rec) rt_log("input: cannot open REKARINA_INPUT_RECORD %s", path);
        if (rec) { fprintf(rec, "# Recorded input (REKARINA_INPUT_RECORD); frame = 60 Hz ticks of game time, @ms = world clock.\n"); atexit(rec_flush_all);
                   rt_log("input: recording to %s", path); }
    }
    if (rec_state != 1) return;
    uint64_t fr = frame_step_ticks();
    int64_t w = frame_step_world_us();
    int wrote = 0;
    if (w >= 0 && rec_last_w < 0) {   // the world clock starts: what is held goes on as timed events from @0
        for (int i = 0; i < 16; i++)
            if (rec_down[i]) {
                rec_line(rec_down[i] - 1, -1, fr, -1, script_buttons[i].name);
                rec_down[i] = fr + 1, rec_down_w[i] = w;
                wrote = 1;
            }
        for (int s = 0; s < 2; s++)
            if (rec_stick_from[s]) {
                rec_close_stick(s, fr, -1);
                rec_stick_from[s] = fr + 1, rec_stick_w[s] = w;
                wrote = 1;
            }
    }
    rec_last_w = w;
    for (int i = 0; i < 16; i++) {
        int bit = script_buttons[i].bit, down = (b >> bit & 1) != 0;
        if (down && !rec_down[i]) rec_down[i] = fr + 1, rec_down_w[i] = w;
        else if (!down && rec_down[i]) {
            rec_line(rec_down[i] - 1, rec_down_w[i], fr, w, script_buttons[i].name);
            rec_down[i] = 0;
            wrote = 1;
        }
    }
    const int32_t *st[2] = {sl, sr};
    for (int s = 0; s < 2; s++) {
        int moved = abs(st[s][0] - rec_stick[s][0]) > REC_STICK_STEP || abs(st[s][1] - rec_stick[s][1]) > REC_STICK_STEP ||
                    ((st[s][0] || st[s][1]) != (rec_stick[s][0] || rec_stick[s][1]));
        if (!moved) continue;
        if (rec_stick_from[s]) { rec_close_stick(s, fr, w); wrote = 1; }
        rec_stick[s][0] = st[s][0]; rec_stick[s][1] = st[s][1];
        if (st[s][0] || st[s][1]) rec_stick_from[s] = fr + 1, rec_stick_w[s] = w;
    }
    if (wrote) fflush(rec);
}

static void mouse_take_camera(int32_t *dx, int32_t *dy);

// DEBUG-TESTMOUSE: REKARINA_TEST_MOUSE=<counts a second>[,<frame>]: a steady horizontal mouse from that frame on (gfx
// frames, as the shots count them), counts by the time since the last call with the fraction carried, as a real
// mouse's reports add up; they join the script's counts, so the direct path or the stick path (mouse_camera) takes
// them, whichever turns the camera. Under lock.
static void test_mouse(void) {
    static double rate = -1, carry;
    static uint64_t last, from;
    if (rate < 0) {
        const char *e = cfg_get("REKARINA_TEST_MOUSE");
        rate = e && *e ? atof(e) : 0;
        const char *c = e ? strchr(e, ',') : NULL;
        from = c ? strtoull(c + 1, NULL, 10) : 0;
    }
    if (!(rate > 0) || __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) < from) return;
    uint64_t now = plat_now_ns();
    if (last) {
        double c = rate * (double)(now - last) / 1e9 + carry, whole = floor(c);
        carry = c - whole, script_mouse[0] += (int32_t)whole;
    }
    last = now;
}

void input_direct_mouse_sample(int enabled, int32_t *dx, int32_t *dy) {
    plat_mutex_lock(&lock);
    enabled = enabled && !g_block && !g_menu;
    test_mouse();
    *dx = *dy = 0;
    if (enabled) {   // otherwise they stay for the stick path (mouse_camera)
        *dx = script_mouse[0]; *dy = script_mouse[1];
        script_mouse[0] = script_mouse[1] = 0;
    }
    if (!enabled) { *dx = *dy = 0; }
    else if (ikb_captured() && ikb_foreground() && input_cursor_allowed()) {
        int32_t x, y;
        mouse_take_camera(&x, &y);   // the script's counts stay as written; a recording keeps what the camera got
        int64_t sx = (int64_t)*dx + x, sy = (int64_t)*dy + y;
        *dx = (int32_t)(sx < INT32_MIN ? INT32_MIN : sx > INT32_MAX ? INT32_MAX : sx);
        *dy = (int32_t)(sy < INT32_MIN ? INT32_MIN : sy > INT32_MAX ? INT32_MAX : sy);
    }
    if (enabled && (*dx || *dy) && rec_state == 1) {
        char what[80];
        snprintf(what, sizeof what, "MOUSE %d %d", *dx, *dy);
        int64_t w = frame_step_world_us();
        uint64_t tick = frame_step_ticks();
        rec_line(tick, w, tick + 1, w, what);
        fflush(rec);
    }
    plat_mutex_unlock(&lock);
}

static float mouse_sens = -1;              // REKARINA_MOUSE_SENS, 0 = mouse camera off; live (cfg_version)
static float mouse_vert = 1;               // REKARINA_MOUSE_SENS_Y, negative with REKARINA_MOUSE_INVERT_Y; read with mouse_sens
static unsigned mouse_sens_seen;

static float mouse_sens_get(void) {
    unsigned ver = cfg_version();
    if (mouse_sens >= 0 && ver == mouse_sens_seen) return mouse_sens;
    mouse_sens_seen = ver;
    double v = cfg_double("REKARINA_MOUSE_SENS"), y = cfg_double("REKARINA_MOUSE_SENS_Y");
    mouse_vert = (float)((y < 0.1 ? 0.1 : y > 3 ? 3 : y) * (cfg_bool("REKARINA_MOUSE_INVERT_Y") ? -1 : 1));
    mouse_sens = (float)(v < 0 ? 0 : v > 10 ? 10 : v);
    return mouse_sens;
}

// The counts that turn the camera: ikb_mouse_take's, the vertical ones times REKARINA_MOUSE_SENS_Y and turned over with
// REKARINA_MOUSE_INVERT_Y (both live; 1 and off leave the counts as they are). The fraction a scaled count leaves is carried
// into the next sample, so a slow vertical move at 0.5 still turns the camera by every second count rather than by none.
// The selectors' cursor (mouse_camera) takes the counts as they are. Under lock.
static void mouse_take_camera(int32_t *dx, int32_t *dy) {
    static double carry;
    ikb_mouse_take(dx, dy);
    mouse_sens_get();
    if (mouse_vert == 1) { carry = 0; return; }
    double y = *dy * (double)mouse_vert + carry, whole = trunc(y);
    carry = y - whole;
    *dy = (int32_t)(whole < INT32_MIN ? INT32_MIN : whole > INT32_MAX ? INT32_MAX : whole);
}


// ---- walk key: the keyboard's stick samples scaled down while a key is held (REKARINA_WALK_KEY, REKARINA_WALK_SCALE)
// A keyboard key is all or nothing, so every keyboard-driven stick sample is +/-32767 and the game only ever sees a
// full tilt. Holding this key scales those two samples, which gives the half tilt a gamepad gets from its analog range,
// so the keyboard can walk as well as run. yuzu and eden do the same with their modifier and modifier_scale settings.
// Only the keyboard is scaled: the gamepad's axes and the mouse camera sample their own values.
static int walk_key = KEY_NONE;        // REKARINA_WALK_KEY as a KEY_*, KEY_NONE = off; live (cfg_version)
static float walk_scale = 0.51f;       // REKARINA_WALK_SCALE, 0.2 to 0.9; live (cfg_version)
static unsigned walk_seen = ~0u;

// The ini takes the key names the Controls tab writes; a hand-written line may spell them in any case.
static int name_matches(const char *name, const char *key) {
    for (; *name && *key; name++, key++) {
        unsigned char a = (unsigned char)*name, b = (unsigned char)*key;
        if (a >= 'A' && a <= 'Z') a = (unsigned char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (unsigned char)(b - 'A' + 'a');
        if (a != b) return 0;
    }
    return *name == *key;
}

static void walk_get(void) {
    unsigned ver = cfg_version();
    if (ver == walk_seen) return;
    walk_seen = ver;
    const char *name = cfg_value("REKARINA_WALK_KEY");   // "none" (the default) turns the key off
    int key = KEY_NONE;
    if (*name && !name_matches(name, "none")) {
        for (int k = KEY_A; k < KEY_COUNT && key == KEY_NONE; k++)
            if (input_key_name(k) && name_matches(name, input_key_name(k))) key = k;
        if (key == KEY_NONE) rt_log("input: walk_key: unknown key '%s', the keyboard sticks keep their full tilt", name);
    }
    // 0.51 rather than 0.5: a keyboard sample is 32767, and 32767 * 0.5 = 16383 falls one short of stick_bits'
    // STICK_DIR (16384), which would leave the game's stick-direction bits clear while the walk key is held. The value
    // in the settings table carries that default; this code only rejects what the table's range does not allow.
    const char *sv = cfg_value("REKARINA_WALK_SCALE");
    char *end;
    double d = strtod(sv, &end);
    double s = end == sv || *end || !(d >= 0.2 && d <= 0.9) ? strtod(cfg_meta("REKARINA_WALK_SCALE")->def, NULL) : d;
    if (key != walk_key || (float)s != walk_scale) {
        walk_key = key;
        walk_scale = (float)s;
        rt_log("input: walk key %s, keyboard sticks at %.0f%% of their tilt (mouse and gamepad unchanged)",
               key != KEY_NONE ? input_key_name(key) : "off", s * 100);
    }
}

// ---- dash on an input of its own (REKARINA_STICK_DASH, off by default; REKARINA_DASH_KEY; both live)
// OoT dashes while its A button is held as the hero moves, and a press of A does the action at hand (the pause menu's
// controller guide names A "Actions" and "Dash (Hold)"; E on the keyboard). With the option on, a press of the
// gamepad's left stick while the stick is tilted, or the dash key held while a stick is tilted, holds A for the game:
// L3 latches until the stick comes back near the middle, the key dashes while held. The press of the stick that starts
// a dash does not reach the game as its own button. A hold ends no sooner than DASH_MIN_NS after it began, so a quick
// tap of the key cannot become the game's tap (an action). The button itself keeps working as before: a press or a
// release of it while a dash holds it lets go of it for DASH_GAP_NS first, so the game sees that press and release of
// its own. A script's LCLICK counts as the stick's press and its DASH as the key (input scripts drive no physical
// button). Until 1.2.2 the hold went to B, which does not dash: the hero kept a run's 4.1-4.5 m/s with B held, and
// 5.0-5.6 m/s with the stick's press or the key on A, at the same height (run dash3, 2026-11-19).
#define DASH_BUTTON B_A
#define DASH_TILT_ON 16384           // a press of the stick counts while the stick is past half way
#define DASH_TILT_OFF 8192           // a latched dash ends when the stick comes back under a quarter
#define DASH_MIN_NS 400000000ull
#define DASH_GAP_NS 34000000ull      // two frames at 60 fps: the game reads its pad once a frame
#define SCRIPT_DASH_BIT 62           // a script's DASH, carried in the buttons word until dash_apply takes it out
static int dash_opt, dash_key = KEY_NONE;
static unsigned dash_seen = ~0u;

static void dash_get(void) {
    unsigned ver = cfg_version();
    if (ver == dash_seen) return;
    dash_seen = ver;
    int on = cfg_bool("REKARINA_STICK_DASH") != 0, key = KEY_NONE;
    const char *name = cfg_value("REKARINA_DASH_KEY");   // "none" turns the key off
    if (*name && !name_matches(name, "none")) {
        for (int k = KEY_A; k < KEY_COUNT && key == KEY_NONE; k++)
            if (input_key_name(k) && name_matches(name, input_key_name(k))) key = k;
        if (key == KEY_NONE) rt_log("input: dash_key: unknown key '%s'", name);
    }
    if (on != dash_opt || key != dash_key)
        rt_log("input: dash on its own input %s (the left stick's press while it is tilted, key %s)", on ? "on" : "off",
               key != KEY_NONE ? input_key_name(key) : "none");
    dash_opt = on, dash_key = key;
}

// Under lock, after every source of input is merged: the dash's hold of DASH_BUTTON in *b. l3: the gamepad's left stick
// pressed (or a script's LCLICK); key: the dash key (or a script's DASH) held; sl: the left stick as the game gets it.
static void dash_apply(uint64_t *b, const int32_t sl[2], int l3, int key) {
    static int latched, l3_was, real_was, holding, told;
    static uint64_t since, gap_until;
    if (!dash_opt) {
        latched = holding = 0;
        return;
    }
    uint64_t now = plat_now_ns();
    int32_t m = abs(sl[0]) > abs(sl[1]) ? abs(sl[0]) : abs(sl[1]);
    if (l3 && !l3_was && m >= DASH_TILT_ON) latched = 1;   // a press while tilted: a dash until the stick comes back
    l3_was = l3;
    if (m < DASH_TILT_OFF) latched = 0;
    if (latched && l3) *b &= ~(1ull << B_STICK_L);   // the press that dashes is not the game's stick press
    int want = latched || (key && m >= DASH_TILT_OFF);
    int real = (int)(*b >> DASH_BUTTON & 1);
    if (want && !holding) {
        holding = 1, since = now, gap_until = 0;
        if (told++ < 8) rt_log("input: dash: %s holds the game's dash button", latched ? "the left stick's press" : "the dash key");
    } else if (!want && holding && now - since >= DASH_MIN_NS) {
        holding = 0;
    }
    if (holding && real != real_was) gap_until = now + DASH_GAP_NS;   // the button's own press or release, seen alone
    real_was = real;
    if (holding) *b = now < gap_until ? *b & ~(1ull << DASH_BUTTON) : *b | 1ull << DASH_BUTTON;
}

static int mouse_selector;
int input_direct_mouse_selector_active(void) { return __atomic_load_n(&mouse_selector, __ATOMIC_RELAXED) != 0; }

// Selector 1 is a radial wheel, 2..4 are distinct horizontal quick lists; only a wheel retains a virtual cursor.
// Under `lock`.
static void mouse_camera(int32_t out[2], int selector) {
    static uint64_t frame = ~0ull, t0;
    static int32_t held[2];
    static int was_selector;
    static float cursor[2];
    static InputMouseRow row;
    uint64_t now = plat_now_ns();
    int32_t dx, dy;
    if (!input_direct_mouse_enabled() || !ikb_captured()) selector = 0;
    __atomic_store_n(&mouse_selector, selector, __ATOMIC_RELAXED);
    if (selector) {
        if (was_selector != selector) cursor[0] = cursor[1] = 0, row = (InputMouseRow){0};
        was_selector = selector;
        ikb_mouse_take(&dx, &dy);
        if (selector == 1) input_direct_mouse_radial(dx, dy, mouse_sens_get(), cursor, out);
        else input_direct_mouse_row(dx, mouse_sens_get(), now, &row, out);
        held[0] = held[1] = 0; frame = ~0ull;
        return;
    }
    if (was_selector) {
        was_selector = 0; cursor[0] = cursor[1] = 0; row = (InputMouseRow){0};
        ikb_mouse_take(&dx, &dy);   // release the wheel without sending its last movement to the camera
        held[0] = held[1] = out[0] = out[1] = 0; frame = ~0ull;
        return;
    }
    if (input_direct_mouse_active()) {
        held[0] = held[1] = out[0] = out[1] = 0;
        frame = ~0ull;
        return;   // the camera update takes the raw counts itself, once, outside the HID stick path
    }
    test_mouse();
    if (!ikb_captured()) {
        ikb_mouse_take(&dx, &dy);
        held[0] = held[1] = 0;
        frame = ~0ull;
    } else {
        uint64_t fr = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
        double age = frame == ~0ull ? 0 : (double)(now - t0) / 1e9;
        if (fr != frame) {
            double dt = age < 0.004 ? 0.004 : age > 0.1 ? 0.1 : age;
            mouse_take_camera(&dx, &dy);
            dx += script_mouse[0], dy += script_mouse[1];   // an input script's counts the direct path left
            script_mouse[0] = script_mouse[1] = 0;
            double per_count, full;
            if (input_direct_mouse_stick_curve(&per_count, &full))
                input_mouse_stick_curved(dx, dy, dt, mouse_sens_get(), per_count, full, held);
            else
                input_mouse_stick(dx, dy, dt, mouse_sens_get(), held);
            frame = fr;
            t0 = now;
        } else if (age > 0.1) {
            held[0] = held[1] = 0;   // frames stopped: do not hold a deflection
        }
    }
    double k = cam_speed_applied(), x = held[0] / k, y = held[1] / k, len = sqrt(x * x + y * y);
    if (len > STICK_MAX) x *= STICK_MAX / len, y *= STICK_MAX / len;
    out[0] = (int32_t)lround(x);
    out[1] = (int32_t)lround(y);
}

static float stick_dead[2] = {-1, -1};     // input_pad_deadzone of the left and right stick; live (cfg_version)
static unsigned stick_dead_seen;

static float stick_dead_get(int s) {
    unsigned ver = cfg_version();
    if (stick_dead[0] < 0 || ver != stick_dead_seen) {
        stick_dead_seen = ver;
        stick_dead[0] = input_pad_deadzone(0);
        stick_dead[1] = input_pad_deadzone(1);
    }
    return stick_dead[s];
}

// One gamepad stick through the dead zone; SDL's Y grows downwards, the Switch's upwards.
static void stick(const int16_t *axes, SDL_GamepadAxis ax, SDL_GamepadAxis ay, float dead, int32_t out[2]) {
    input_pad_stick(axes[ax], -(int32_t)axes[ay], dead, out);
}

// REKARINA_PAD_FOLLOW (default 1, live): with several gamepads connected, the one pressed last is the one the game reads
// (its labels, rumble and gyro go with it). Each of them stays open; a controller that SDL lists but that sends nothing
// (a second driver's copy of it, a receiver with no pad paired, Steam's virtual pad beside the real one) then never
// keeps the game from the one in the player's hands. 0: the first gamepad only, as before.
enum { MAX_OTHER_PADS = 7 };
static SDL_Gamepad *others[MAX_OTHER_PADS];   // gamepad thread: the other open gamepads
static int n_others;
static char pad_name_shown[64];               // the open gamepad's name for the menu, under snap_lock

static int pad_follow(void) {
    static int on = -1;
    static unsigned seen;
    unsigned v = cfg_version();
    if (on < 0 || v != seen) seen = v, on = cfg_bool("REKARINA_PAD_FOLLOW") != 0;
    return on;
}

static void pad_adopt(SDL_Gamepad *g);

// The gamepads SDL lists now against the open ones: the read one and the others that went away are closed, new ones
// opened (the first becomes the read one when none is), and with REKARINA_PAD_FOLLOW=0 only the first is kept. Logs
// once per change. Gamepad thread (start() calls it once before that thread exists).
static void scan_pads(void) {
    for (int i = 0; i < n_others;)
        if (!SDL_GamepadConnected(others[i]) || !pad_follow()) {
            SDL_CloseGamepad(others[i]);
            others[i] = others[--n_others];
        } else {
            i++;
        }
    if (pad && !SDL_GamepadConnected(pad)) {
        SDL_CloseGamepad(pad); pad = NULL; sensor_pad = 0; sensor_ok = 0; hd_until = 0;
        plat_xchg32(&pad_hd, 0);
        plat_xchg32(&pad_layout_shown, -1);
        plat_xchg32(&pad_family, -1);
        for (int i = 0; i < 4; i++) plat_xchg32(&pad_labels[i], PAD_LABEL_NONE);
        uint64_t now = plat_now_ns() / 1000000u;
        if (rumble_at && now - rumble_at < 2000 && now / 1000 <= rumble_sec + 1)
            rt_log("input: gamepad disconnected (%llu ms after its last rumble write, motors %u and %u; %u writes in that "
                   "second, %u in the one before)", (unsigned long long)(now - rumble_at), rumble_low, rumble_high,
                   rumble_n, rumble_n_prev);
        else
            rt_log("input: gamepad disconnected");
        rumble_at = 0, rumble_low = rumble_high = 0;
        if (n_others) {   // another one is still there: it is read from now on
            SDL_Gamepad *g = others[--n_others];
            pad_adopt(g);
        }
    }
    if (pad && !pad_follow()) return;
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    for (int i = 0; ids && i < n; i++) {
        int open = pad && SDL_GetGamepadID(pad) == ids[i];
        for (int k = 0; k < n_others && !open; k++) open = SDL_GetGamepadID(others[k]) == ids[i];
        if (open) continue;
        // one that could not be opened is not tried again each second: an open holds SDL's joystick lock for a long
        // moment, and this thread updates the pad the game reads
        static SDL_JoystickID refused[8];
        static int n_refused;
        for (int k = 0; k < n_refused && k < 8 && !open; k++) open = refused[k] == ids[i];
        if (open) continue;
        if (pad && n_others >= MAX_OTHER_PADS) break;
        SDL_Gamepad *g = SDL_OpenGamepad(ids[i]);
        if (!g) {
            rt_log("input: gamepad %u cannot be opened (%s)", (unsigned)ids[i], SDL_GetError());
            refused[n_refused++ % 8] = ids[i];
            continue;
        }
        if (!pad) {
            pad_adopt(g);
            if (!pad_follow()) break;
        } else {
            others[n_others++] = g;
            rt_log("input: another gamepad \"%s\" (vendor %04x product %04x): the game reads it once it is pressed",
                   SDL_GetGamepadName(g), SDL_GetGamepadVendor(g), SDL_GetGamepadProduct(g));
        }
    }
    SDL_free(ids);
}

// A press on gamepad g: a button down, a trigger past half way or a stick past PAD_USED. Gamepad thread.
static int pad_pressed_now(SDL_Gamepad *g) {
    for (int k = 0; k < SDL_GAMEPAD_BUTTON_COUNT; k++)
        if (SDL_GetGamepadButton(g, (SDL_GamepadButton)k)) return 1;
    for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT; a++) {
        int v = SDL_GetGamepadAxis(g, (SDL_GamepadAxis)a);
        if (a == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || a == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER ? v > TRIGGER_ON : abs(v) > PAD_USED)
            return 1;
    }
    return 0;
}

// REKARINA_PAD_FOLLOW: another open gamepad that was pressed becomes the one read; the one read so far joins the others.
static void pad_follow_press(void) {
    if (!pad || !n_others || !pad_follow()) return;
    // Only while the one read is at rest: two copies of one controller (Steam's virtual pad and the real one, two
    // drivers) press together, and switching on every such poll would flap between them.
    if (pad_pressed_now(pad)) return;
    for (int i = 0; i < n_others; i++)
        if (pad_pressed_now(others[i])) {
            SDL_Gamepad *g = others[i];
            if (sensor_ok) {   // the old one stops streaming its sensors
                SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_GYRO, false);
                SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_ACCEL, false);
            }
            others[i] = pad;
            hd_until = 0;
            plat_xchg32(&pad_hd, 0);
            rt_log("input: gamepad \"%s\" pressed: the game reads it from now on", SDL_GetGamepadName(g));
            pad_adopt(g);
            return;
        }
}

// g becomes the gamepad the game reads: its labels, the prompts' kind, rumble, HD rumble and sensors are taken from it.
static void pad_adopt(SDL_Gamepad *g) {
    pad = g;
    rumble_low = rumble_high = 0;   // nothing written to this one yet: its first value is a change (pad_main)
    plat_mutex_lock(&snap_lock);
    snprintf(pad_name_shown, sizeof pad_name_shown, "%s", SDL_GetGamepadName(pad) ? SDL_GetGamepadName(pad) : "");
    plat_mutex_unlock(&snap_lock);
    pad_rumble = SDL_GetBooleanProperty(SDL_GetGamepadProperties(pad), SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false);
    pad_east_a = SDL_GetGamepadButtonLabel(pad, SDL_GAMEPAD_BUTTON_EAST) == SDL_GAMEPAD_BUTTON_LABEL_A;
    int shapes = 0;
    for (int i = 0; i < 4; i++) {
        SDL_GamepadButtonLabel lb = SDL_GetGamepadButtonLabel(pad, pad_sdl[PAD_SOUTH + i]);
        shapes |= lb >= SDL_GAMEPAD_BUTTON_LABEL_CROSS && lb <= SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE;
        plat_xchg32(&pad_labels[i], lb == SDL_GAMEPAD_BUTTON_LABEL_A          ? PAD_LABEL_A
                                            : lb == SDL_GAMEPAD_BUTTON_LABEL_B        ? PAD_LABEL_B
                                            : lb == SDL_GAMEPAD_BUTTON_LABEL_X        ? PAD_LABEL_X
                                            : lb == SDL_GAMEPAD_BUTTON_LABEL_Y        ? PAD_LABEL_Y
                                            : lb == SDL_GAMEPAD_BUTTON_LABEL_CROSS    ? PAD_LABEL_CROSS
                                            : lb == SDL_GAMEPAD_BUTTON_LABEL_CIRCLE   ? PAD_LABEL_CIRCLE
                                            : lb == SDL_GAMEPAD_BUTTON_LABEL_SQUARE   ? PAD_LABEL_SQUARE
                                            : lb == SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE ? PAD_LABEL_TRIANGLE
                                                                                      : PAD_LABEL_NONE);
    }
    // The prompts' kind goes by the labels SDL reports, which it takes from the pad's type (and, for Steam's virtual
    // pad, from the controller behind it): DualShock 4, DualSense and Edge have shapes; Switch Pro and Joy-Con have A on
    // the east button; Xbox and XInput pads, the Steam Deck's own controls (Valve, 0x28de) and Windows handhelds (ROG
    // Ally, Legion Go: XInput) have A at the bottom, as does a pad whose labels SDL does not know.
    int family = shapes ? PROMPTS_PS : pad_east_a ? PROMPTS_NINTENDO : PROMPTS_XBOX;
    plat_xchg32(&pad_family, family);
    SDL_GamepadType gt = SDL_GetGamepadType(pad);
    const char *type = SDL_GetGamepadStringForType(gt);
    rt_log("input: gamepad type %s, vendor %04x product %04x: %s labels", type && *type ? type : "unknown",
           SDL_GetGamepadVendor(pad), SDL_GetGamepadProduct(pad),
           family == PROMPTS_PS ? "PlayStation" : family == PROMPTS_NINTENDO ? "Nintendo" : "Xbox");
    __atomic_store_n(&pad_layout_stale, 1, __ATOMIC_RELEASE);
    // A Switch Pro Controller or a Joy-Con with rumble gets the game's two bands per side (hd_send) until it refuses them.
    // Only Nintendo's own (vendor 057e: Joy-Con L 2006, R 2007, the pair SDL combines 2008, Pro Controller 2009,
    // charging grip 200e, which clones in Switch mode copy): the Switch 2 controllers have no gamepad type of their own,
    // and SDL drives them with another driver (SDL_HINT_JOYSTICK_HIDAPI_SWITCH2), not the one hd_send's packet suits.
    uint16_t pv = SDL_GetGamepadVendor(pad), pp = SDL_GetGamepadProduct(pad);
    int nintendo_hd = pv == 0x057e && ((pp >= 0x2006 && pp <= 0x2009) || pp == 0x200e);
    plat_xchg32(&pad_hd, pad_rumble && nintendo_hd &&
                             (gt == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO || gt == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT ||
                              gt == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT ||
                              gt == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR));
    int motion = SDL_GamepadHasSensor(pad, SDL_SENSOR_GYRO) && SDL_GamepadHasSensor(pad, SDL_SENSOR_ACCEL);
    rt_log("input: gamepad \"%s\" (%s, %s, east button %s)", SDL_GetGamepadName(pad), pad_rumble ? "rumble" : "no rumble",
           motion ? "gyro and accelerometer" : "no motion sensors", pad_east_a ? "labelled A" : "not labelled A");
    // An XInput pad has no motion sensors, and a program that stands in for another controller shows up as one
    if (!motion && (gt == SDL_GAMEPAD_TYPE_XBOX360 || gt == SDL_GAMEPAD_TYPE_XBOXONE))
        rt_log("input: an Xbox-type (XInput) gamepad: no gyro reaches the game. A PlayStation or Switch controller looks "
               "like this when Steam Input, DS4Windows or BetterJoy stands in for it; SDL must open the controller itself "
               "for its gyro and its own rumble");
}

// Under lock: the layout of the open gamepad, picked again when a setting or the gamepad changed; logs each change.
static int pad_layout_get(void) {
    if (__atomic_exchange_n(&pad_layout_stale, 0, __ATOMIC_ACQUIRE)) pad_layout = -1;
    unsigned ver = cfg_version();
    if (pad_layout >= 0 && ver == pad_layout_seen) return pad_layout;
    pad_layout_seen = ver;
    int was = pad_layout;
    const char *set = cfg_value("REKARINA_PAD_LAYOUT");
    pad_layout = input_pad_layout_pick(set, pad_east_a);
    if (pad_layout != was)
        rt_log("input: gamepad layout %s (pad_layout = %s)", pad_layout == PAD_LAYOUT_XBOX ? "xbox" : "nintendo", set);
    plat_xchg32(&pad_layout_shown, pad_layout);
    if (pad) romfs_hint_pad_layout(pad_layout);   // the action hint's places follow it at the next start
    return pad_layout;
}

// The REKARINA_BIND_* values last applied, one per line; the tables are built again when one changes (the menu's remapping rows write
// them while the game runs). Under lock.
static unsigned binds_seen = ~0u;
static char binds_applied[4096];

static void binds_refresh(void) {
    unsigned v = cfg_version();
    if (v == binds_seen) return;
    binds_seen = v;
    char now[sizeof binds_applied];
    size_t len = 0;
    for (int a = 0; a < ACTION_COUNT; a++) {
        char var[32];
        snprintf(var, sizeof var, "REKARINA_BIND_%s", input_action_name(a));
        const char *s = cfg_get(var);
        len += (size_t)snprintf(now + len, len < sizeof now ? sizeof now - len : 0, "%s\n", s ? s : "");
    }
    if (!strcmp(now, binds_applied)) return;
    memcpy(binds_applied, now, sizeof now);
    input_map_configure();
}

// REKARINA_TEST_PAD=<lx>,<ly>,<rx>,<ry>[,<kind>] (test runs): an SDL virtual gamepad whose sticks rest at these raw SDL axis
// values (-32768..32767, down is +y), attached before the first scan so it is the gamepad the game reads: drift and full
// deflection go through the same mapping and dead zone as a real pad. kind (ps5, ps4, switch, xbox) gives it that
// controller's USB ids, so SDL reports its type and the automatic prompts follow it (a check of the prompt sets without
// the pads).
static void test_pad_attach(void) {
    static SDL_Joystick *joy;   // kept open: a virtual joystick's axes are set through an open handle
    const char *e = cfg_get("REKARINA_TEST_PAD");
    int v[4];
    if (!e || !*e) return;
    char kind[16] = "";
    if (sscanf(e, "%d,%d,%d,%d,%15s", &v[0], &v[1], &v[2], &v[3], kind) < 4) {
        rt_log("input: REKARINA_TEST_PAD=%s is not <lx>,<ly>,<rx>,<ry>; no test gamepad", e);
        return;
    }
    SDL_VirtualJoystickDesc d;
    SDL_INIT_INTERFACE(&d);
    d.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    d.naxes = SDL_GAMEPAD_AXIS_COUNT;
    d.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    d.name = "TotK test gamepad";
    static const struct { const char *kind; Uint16 vendor, product; const char *name; } ids[] = {
        {"ps5", 0x054c, 0x0ce6, "DualSense Wireless Controller"}, {"ps4", 0x054c, 0x09cc, "PS4 Controller"},
        {"switch", 0x057e, 0x2009, "Pro Controller"}, {"xbox", 0x045e, 0x0b13, "Xbox Wireless Controller"},
    };
    for (size_t i = 0; i < sizeof ids / sizeof *ids; i++)
        if (!strcmp(kind, ids[i].kind)) d.vendor_id = ids[i].vendor, d.product_id = ids[i].product, d.name = ids[i].name;
    SDL_JoystickID id = SDL_AttachVirtualJoystick(&d);
    joy = id ? SDL_OpenJoystick(id) : NULL;
    if (!joy) {
        rt_log("input: test gamepad not attached (%s)", SDL_GetError());
        return;
    }
    static const SDL_GamepadAxis axes[4] = {SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY, SDL_GAMEPAD_AXIS_RIGHTX,
                                            SDL_GAMEPAD_AXIS_RIGHTY};
    for (int i = 0; i < 4; i++)
        SDL_SetJoystickVirtualAxis(joy, axes[i], (Sint16)(v[i] < -32768 ? -32768 : v[i] > 32767 ? 32767 : v[i]));
    rt_log("input: test gamepad attached (%s), sticks at %d,%d %d,%d", d.name, v[0], v[1], v[2], v[3]);
}

static void pad_main(void *arg);

static void start(void) {
    binds_refresh();
    const char *e = cfg_get("REKARINA_INPUT");
    if (e && !strcmp(e, "none")) { state = -1; rt_log("input: disabled (REKARINA_INPUT=none)"); return; }
    // REKARINA_PAD_DRIVER=compat (read at start): SDL's own drivers (HIDAPI, and on Windows Raw Input and Windows.Gaming.Input)
    // stay off, so a controller is read through the system's XInput or DirectInput (Linux: the kernel's evdev). Some
    // controllers are listed by those drivers but send nothing through them (8BitDo pads in some modes, an Xbox One S pad
    // over Bluetooth); the system's path reads them. Gyro, HD rumble and the PlayStation labels need the default (auto).
    if (!strcmp(cfg_value("REKARINA_PAD_DRIVER"), "compat")) {
        SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0");
        SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "0");
        SDL_SetHint(SDL_HINT_JOYSTICK_WGI, "0");
        rt_log("input: gamepad driver compat: the system's XInput / DirectInput (evdev on Linux), not SDL's own drivers");
    }
    if (!SDL_Init(SDL_INIT_GAMEPAD)) { state = -1; rt_log("input: SDL gamepad init failed (%s), keyboard only", SDL_GetError()); }
    else state = 1;
    if (state == 1) test_pad_attach();
    if (state == 1) {
        scan_pads();
        if (!pad) rt_log("input: no gamepad (keyboard only until one is connected)");
        if (!plat_thread_spawn(pad_main, NULL, "gamepad", 0)) rt_fatal("input: cannot start the gamepad thread");
    }
    if (state == -1) state = 2;   // keyboard only
}

static void stick_bits(uint64_t *b, int32_t x, int32_t y, int first) {
    if (x < -STICK_DIR) *b |= 1ull << (first + 0);
    if (y > STICK_DIR) *b |= 1ull << (first + 1);
    if (x > STICK_DIR) *b |= 1ull << (first + 2);
    if (y < -STICK_DIR) *b |= 1ull << (first + 3);
}

static void pick(int32_t *x, int32_t *y, int32_t kx, int32_t ky) {
    int64_t a = (int64_t)*x * *x + (int64_t)*y * *y, b = (int64_t)kx * kx + (int64_t)ky * ky;
    if (b > a) { *x = kx; *y = ky; }
}

// The shake key (B_SHAKE): the game detaches a part fused to what Ultrahand holds when the right stick swings left and
// right a few times. A press swings it for SHAKE_NS, holding keeps it swinging; each side lasts SHAKE_SIDE_NS, two
// frames at the game's 30 fps, so every side is read even at 20 fps. Returns the stick x, 0 while not shaking.
// Under lock.
#define SHAKE_NS 800000000ull
#define SHAKE_SIDE_NS 66666667ull
static int32_t shake_stick(int held) {
    static int was;
    static uint64_t from, until;
    uint64_t now = plat_now_ns();
    if (held && !was && now >= until) from = now;
    if (held && !was) until = now + SHAKE_NS;
    if (held && until < now) until = now;
    was = held;
    if (now >= until && !held) return 0;
    return (now - from) / SHAKE_SIDE_NS % 2 ? STICK_MAX : -STICK_MAX;
}

// State 1: the gamepad's inputs by position (pd[PAD_*] != 0 held) and its axes (ax may be NULL) as the gamepad thread
// last read them; 0 without a connected gamepad.
static int pad_physical(uint8_t pd[PAD_COUNT], int16_t *ax) {
    plat_mutex_lock(&snap_lock);
    int on = snap.on;
    if (on) {
        memcpy(pd, snap.pd, sizeof snap.pd);
        if (ax) memcpy(ax, snap.ax, sizeof snap.ax);
    }
    plat_mutex_unlock(&snap_lock);
    return on;
}

// Under lock, state 1: the open gamepad's buttons (Switch bits after REKARINA_PAD_LAYOUT and the REKARINA_BIND_* lines) and
// sticks; 0 without a connected gamepad.
// The Guide button as the gamepad thread last read it. It is no Switch button and no binding: only the menu uses it.
static int pad_guide(void) {
    plat_mutex_lock(&snap_lock);
    int g = snap.on && snap.guide;
    plat_mutex_unlock(&snap_lock);
    return g;
}

static int pad_read(uint64_t *b, int32_t sl[2], int32_t sr[2], int *l3) {
    uint8_t pd[PAD_COUNT] = {0};
    int16_t ax[SDL_GAMEPAD_AXIS_COUNT];
    if (!pad_physical(pd, ax)) return 0;
    *b |= input_map_pad_buttons(pd, pad_layout_get());
    *l3 = pd[PAD_LSTICK] != 0;   // the stick's own press, whatever it is bound to (dash_apply)
    stick(ax, SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY, stick_dead_get(0), sl);
    stick(ax, SDL_GAMEPAD_AXIS_RIGHTX, SDL_GAMEPAD_AXIS_RIGHTY, stick_dead_get(1), sr);
    return 1;
}

int input_pad_snapshot(uint8_t pd[PAD_COUNT], int16_t ax[6], char *name, size_t n) {
    plat_mutex_lock(&snap_lock);
    int on = snap.on;
    if (on) {
        memcpy(pd, snap.pd, sizeof snap.pd);
        for (int i = 0; i < 6 && i < SDL_GAMEPAD_AXIS_COUNT; i++) ax[i] = snap.ax[i];
    }
    if (name && n) snprintf(name, n, "%s", on ? pad_name_shown : "");
    plat_mutex_unlock(&snap_lock);
    return on;
}

int input_pad_stick_percent(double pct[2]) {
    int ok = 0;
    pct[0] = pct[1] = 0;
    plat_mutex_lock(&snap_lock);
    if (snap.on) {
        pct[0] = input_stick_percent(snap.ax[SDL_GAMEPAD_AXIS_LEFTX], snap.ax[SDL_GAMEPAD_AXIS_LEFTY]);
        pct[1] = input_stick_percent(snap.ax[SDL_GAMEPAD_AXIS_RIGHTX], snap.ax[SDL_GAMEPAD_AXIS_RIGHTY]);
        ok = 1;
    }
    plat_mutex_unlock(&snap_lock);
    return ok;
}

// Under lock: a new press on the gamepad for prompt_dev, from its Switch buttons b and sticks (after the dead zone): a
// button that was up at the last poll, or a stick past PROMPT_STICK_ON that fell under PROMPT_STICK_OFF since it last
// counted.
static int pad_pressed(uint64_t b, const int32_t sl[2], const int32_t sr[2]) {
    static uint64_t was;
    static int armed[2] = {1, 1};
    int press = (b & ~was) != 0;
    was = b;
    const int32_t *st[2] = {sl, sr};
    for (int s = 0; s < 2; s++) {
        int32_t x = abs(st[s][0]), y = abs(st[s][1]), m = x > y ? x : y;
        if (armed[s] && m > PROMPT_STICK_ON) press = 1, armed[s] = 0;
        else if (!armed[s] && m < PROMPT_STICK_OFF) armed[s] = 1;
    }
    return press;
}

// Under lock, the game window in front: a new press for prompt_dev of a key or mouse button the game binds (down[]
// after the capture rules) or of Esc. fresh: the window just came to the front, what is held is taken as the start.
static int keys_pressed(const uint8_t down[KEY_COUNT], int esc, int fresh) {
    static uint8_t was[KEY_COUNT];
    static int esc_was;
    int press = 0;
    if (!fresh) {
        for (int i = 0; i < input_map.nkeys; i++)
            if (down[input_map.keys[i].key] && !was[input_map.keys[i].key]) press = 1;
        press |= esc && !esc_was;
    }
    memcpy(was, down, sizeof was);
    esc_was = esc;
    return press;
}

// Under lock: dev (DEV_PAD, DEV_KBM; DEV_NONE: no press in this poll) pressed now; prompt_dev follows once the newest
// press has kept its device for PROMPT_SETTLE_NS (the first press at once).
static void prompt_track(int dev) {
    static int logged;
    uint64_t now = plat_now_ns();
    if (dev != DEV_NONE && dev != prompt_newest) prompt_newest = dev, prompt_newest_ns = now;
    int cur = (int)__atomic_load_n(&prompt_dev, __ATOMIC_RELAXED);
    if (prompt_newest == DEV_NONE || prompt_newest == cur) return;
    if (cur != DEV_NONE && now - prompt_newest_ns < PROMPT_SETTLE_NS) return;
    __atomic_store_n(&prompt_dev, prompt_newest, __ATOMIC_RELAXED);
    if (logged < 64)
        rt_log("input: device in use: %s%s", prompt_newest == DEV_PAD ? "the gamepad" : "the keyboard and mouse",
               ++logged == 64 ? " (further changes are not logged)" : "");
}

// REKARINA_KBM_OFF (default 0, live): while a gamepad is connected the game takes nothing from the keyboard and the mouse
// (a pad player whose desk or cat leans on a key); the settings menu, F11 and the screenshot key still work.
static int kbm_off(void) {
    static int on = -1;
    static unsigned seen;
    unsigned v = cfg_version();
    if (on < 0 || v != seen) seen = v, on = cfg_bool("REKARINA_KBM_OFF") != 0;
    return on;
}

void gfx_screenshot_request(void);   // gfx_present.c

// The screenshot key (REKARINA_SCREENSHOT_KEY, a key name of the bind_ lines; default f12, none: off): its press asks
// the present for a picture (gfx_present.c gfx_screenshot_request). Under lock, the window in front.
static void screenshot_key(void) {
    static unsigned seen = ~0u;
    static int key = KEY_NONE, was;
    unsigned v = cfg_version();
    if (v != seen) {
        seen = v, key = KEY_NONE;
        const char *name = cfg_value("REKARINA_SCREENSHOT_KEY");
        for (int k = KEY_A; k < KEY_COUNT && *name && !name_matches(name, "none") && key == KEY_NONE; k++)
            if (input_key_name(k) && name_matches(name, input_key_name(k))) key = k;
    }
    int down = key != KEY_NONE && ikb_down(key);
    if (down && !was) gfx_screenshot_request();
    was = down;
}

static void poll_merged(uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) {
    uint64_t b = 0;
    int32_t sl[2] = {0, 0}, sr[2] = {0, 0};
    plat_mutex_lock(&lock);
    if (!state) start();
    int l3 = 0, dash_down = 0;
    dash_get();
    if (state > 0) {
        binds_refresh();
        int pad_on = state == 1 && pad_read(&b, sl, sr, &l3);
        int pad_used = b || abs(sl[0]) > PAD_USED || abs(sl[1]) > PAD_USED || abs(sr[0]) > PAD_USED ||
                       abs(sr[1]) > PAD_USED, kbm_used = 0;
        int pad_press = pad_pressed(b, sl, sr), key_press = 0;
        static int was_front;   // the game window was in front at the last poll
        int front = ikb_foreground();
        if (front) screenshot_key();
        if (front && !(pad_on && kbm_off())) {
            uint8_t down[KEY_COUNT] = {0};
            // Mouse buttons count only while the cursor is captured, and a button already held when the capture began
            // (the click that refocused the window) is ignored until it is released.
            static int was_captured;
            static uint8_t mouse_ignore[KEY_COUNT];
            int cap = ikb_captured();
            for (int k = 1; k < KEY_COUNT; k++) {
                down[k] = ikb_down(k);
                kbm_used |= down[k];
                if (k < KEY_MOUSE_L || k > KEY_MOUSE_X2) continue;
                if (cap && !was_captured) mouse_ignore[k] = down[k];
                if (!down[k]) mouse_ignore[k] = 0;
                if (!cap || mouse_ignore[k]) down[k] = 0;
            }
            was_captured = cap;
            // (keys typed into a text entry are text: the Steam Deck's on-screen keyboard keeps the pad's prompts)
            key_press = keys_pressed(down, ikb_escape(), !was_front) && g_block != 1;
            b |= input_map_buttons(down);
            dash_down = dash_key != KEY_NONE && down[dash_key];
            int32_t k[2];
            walk_get();
            int walk = walk_key != KEY_NONE && down[walk_key];
            input_map_stick(down, 0, k);
            if (walk) k[0] = (int32_t)(k[0] * walk_scale), k[1] = (int32_t)(k[1] * walk_scale);
            pick(&sl[0], &sl[1], k[0], k[1]);
            input_map_stick(down, 1, k);
            if (walk) k[0] = (int32_t)(k[0] * walk_scale), k[1] = (int32_t)(k[1] * walk_scale);
            pick(&sr[0], &sr[1], k[0], k[1]);
            int selector = b & (1ull << B_L) ? 1 : b & (1ull << B_UP) ? 2 :
                           b & (1ull << B_LEFT) ? 3 : b & (1ull << B_RIGHT) ? 4 : 0;
            mouse_camera(k, selector);
            pick(&sr[0], &sr[1], k[0], k[1]);
            int32_t shake = shake_stick(input_map_held(down, B_SHAKE));
            if (shake) sr[0] = shake, sr[1] = 0;
            static uint32_t travel_seen;
            uint32_t travel = ikb_mouse_travel();
            if (travel - travel_seen >= MOUSE_USED) kbm_used = 1;
            travel_seen = travel;
        }
        // A press or a full-way stick on the pad wins over keys held in the same poll: the player picked it up
        if (pad_used) __atomic_store_n(&g_kbm_last, 0, __ATOMIC_RELAXED);
        else if (kbm_used) __atomic_store_n(&g_kbm_last, 1, __ATOMIC_RELAXED);
        was_front = front;
        prompt_track(pad_press ? DEV_PAD : key_press ? DEV_KBM : DEV_NONE);
    }
    uint64_t before_script = b;
    script_apply(&b, sl, sr);
    rec_apply(b, sl, sr);
    l3 |= (int)((b & ~before_script) >> B_STICK_L & 1);   // a script's LCLICK is the stick's press
    dash_down |= (int)(b >> SCRIPT_DASH_BIT & 1);
    b &= ~(1ull << SCRIPT_DASH_BIT);
    dash_apply(&b, sl, l3, dash_down);
    // Minus and Plus held together (gamepad or keys) open or close the menu and never reach the game
    static int combo_held;
    int32_t st[4] = {sl[0], sl[1], sr[0], sr[1]};
    int toggle = input_menu_gate(&combo_held, menu_is_open(), &b, st);
    // The Guide button (REKARINA_MENU_GUIDE, on by default) opens and closes the menu on the press
    static int guide_was;
    int guide = cfg_bool("REKARINA_MENU_GUIDE") && state == 1 && pad_guide();
    if (guide && !guide_was) toggle = 1;
    guide_was = guide;
    static int overlay_held;   // L3+R3 step the performance overlay (F10)
    int overlay = input_overlay_gate(&overlay_held, &b);
    sl[0] = st[0], sl[1] = st[1], sr[0] = st[2], sr[1] = st[3];
    plat_mutex_unlock(&lock);
    if (toggle) menu_toggle();
    if (overlay) menu_perf_cycle();
    stick_bits(&b, sl[0], sl[1], B_SL_LEFT);
    stick_bits(&b, sr[0], sr[1], B_SR_LEFT);
    *buttons = b; *lx = sl[0]; *ly = sl[1]; *rx = sr[0]; *ry = sr[1];
}

// Text entry (swkbd_*.c): while it is open the game reads no input, as behind the keyboard applet; after it closes
// the game reads none until every button and stick direction is released, so the press that closed it stays there.
// g_block: 0 off, 1 text entry open, 2 closed, waiting for the release. The cursor is released while it is open.
// When g_block and g_menu last went to 2 (plat_now_ns). The wait for the release ends after RELEASE_WAIT_NS even with
// something held: an input that never reads as released (a button another program holds down, a key whose release
// went to another window, a stick that rests outside its dead zone) would otherwise keep the game deaf until restart
// (#462, #494). A press that closed the menu is let go well within the wait; one still held after it is meant.
static volatile uint64_t g_block_closed, g_menu_closed;
#define RELEASE_WAIT_NS 1000000000ull

int input_cursor_allowed(void) {
    const char *in = cfg_get("REKARINA_INPUT");
    return g_block != 1 && g_menu != 1 && mouse_sens_get() > 0 && !(in && !strcmp(in, "none")) && !menu_cursor_pointer();
}

void input_block(int on) {
    if (!on) __atomic_store_n(&g_block_closed, plat_now_ns(), __ATOMIC_RELEASE);
    plat_xchg32(&g_block, on ? 1 : 2);
    ikb_cursor_update();
}

// The in-game menu (ui/menu.cpp) blocks the game's input the same way: g_menu 0 closed, 1 open, 2 closed, waiting for
// the release. The cursor is released while it is open, for the menu's mouse.
void input_menu(int open) {
    if (!open) __atomic_store_n(&g_menu_closed, plat_now_ns(), __ATOMIC_RELEASE);
    plat_xchg32(&g_menu, open ? 1 : 2);
    if (!open) ikb_menu_closed();   // the capture before Esc comes back
    ikb_cursor_update();
}

// 1 while flag (g_block, g_menu) blocks the game's input; state 2 ends at the first poll with nothing held, or with
// something still held RELEASE_WAIT_NS after closed. A stick axis counts as held as well: with the walk key the
// keyboard's sticks move part way without setting a direction bit, so a gate that watched the buttons alone would let
// that movement through the moment the menu or the text entry closes.
static int blocked(volatile int32_t *flag, volatile uint64_t *closed, const char *what, uint64_t buttons,
                   const int32_t *axes) {
    int32_t k = *flag;
    if (k != 2) return k != 0;
    int held = buttons != 0;
    for (int i = 0; i < 4; i++) held |= axes[i] != 0;
    if (held && plat_now_ns() - __atomic_load_n(closed, __ATOMIC_ACQUIRE) < RELEASE_WAIT_NS) return 1;
    if (plat_cas32(flag, 0, 2) != 2) return *flag != 0;   // reopened meanwhile
    if (held)
        rt_log("input: the %s closed 1 s ago and input is still held (buttons %#llx, sticks %d %d %d %d); the game "
               "gets it from now on", what, (unsigned long long)buttons, axes[0], axes[1], axes[2], axes[3]);
    return 0;
}

void input_poll(uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) {
    poll_merged(buttons, lx, ly, rx, ry);
    if (!g_block && !g_menu) menu_cursor_poll(buttons);
    const int32_t axes[4] = {*lx, *ly, *rx, *ry};
    int text = blocked(&g_block, &g_block_closed, "text entry", *buttons, axes),
        menu = blocked(&g_menu, &g_menu_closed, "menu", *buttons, axes);
    if (!text && !menu) return;
    *buttons = 0; *lx = *ly = *rx = *ry = 0;
}

void input_poll_ui(uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) {
    poll_merged(buttons, lx, ly, rx, ry);
}

int input_pad_read(uint64_t *buttons, int32_t *lx, int32_t *ly) {
    uint64_t b = 0;
    int32_t sl[2] = {0, 0}, sr[2] = {0, 0};
    plat_mutex_lock(&lock);
    if (!state) start();
    if (state > 0) binds_refresh();
    int l3;
    int ok = state == 1 && pad_read(&b, sl, sr, &l3);
    plat_mutex_unlock(&lock);
    *buttons = b;
    *lx = sl[0];
    *ly = sl[1];
    return ok;
}

uint16_t rumble_motor(float a, float curve, float strength) {
    if (!(a > 0) || !(strength > 0)) return 0;
    float v = powf(a > 1 ? 1 : a, curve) * strength * 65535.0f + 0.5f;
    return v >= 65535.0f ? 65535 : (uint16_t)v;
}

// ---- Switch HD rumble: the rumble data of the Switch Pro Controller and the Joy-Con, as the public reverse-engineering
// notes on the Switch's controllers describe it (Nintendo_Switch_Reverse_Engineering: the rumble data section of its
// HID notes and its rumble data table). Four bytes per side, a high and a low band each:
//   frequency: the code c = round(32 * log2(f / 10 Hz)). The high band keeps (c - 0x60) * 4, nine bits from byte 0 into
//     bit 0 of byte 1 (codes 0x61..0xdf: 81.75 to 1252.57 Hz); the low band keeps c - 0x40 in the low seven bits of
//     byte 2 (codes 0x41..0xbf: 40.87 to 626.28 Hz). A frequency outside a band takes the band's nearest end.
//   amplitude: a step e of 0 to 100 on the table's logarithmic scale, step 100 being amplitude 1.0, the most the notes
//     call safe (more is clamped). The high band keeps 2e in the upper seven bits of byte 1; the low band keeps
//     0x40 + e / 2 in byte 3 and the odd half step in bit 7 of byte 2.
// Neutral, nothing at 160 and 320 Hz, is 00 01 40 40. cfg_probe.c checks the encoding against the notes' values.

// The frequency code of f Hz, clamped to lo..hi; a frequency that is not a positive number counts as neutral Hz.
static int hd_freq_code(float f, float neutral, int lo, int hi) {
    double c = round(32 * log2((f > 0 && isfinite(f) ? f : neutral) / 10.0));
    return c < lo ? lo : c > hi ? hi : (int)c;
}

// The amplitude step of a. The table's amplitude grows by 2^(1/32) a step from about 0.23 up (step 32) to 1.0 (step
// 100), by 2^(1/16) a step from about 0.115 (step 16 is 0.1175) and by 2^(1/4) a step below (step 8 is 0.0334); its
// first step is 0.0078, so anything from 0.004 is at least step 1. NaN and values <= 0 are step 0.
static int hd_amp_step(float a) {
    if (!(a > 0)) return 0;
    double l = log2(a < 1 ? a : 1);
    double e = round(a >= 0.227f ? 100 + 32 * l : a >= 0.115f ? 16 + 16 * (l - log2(0.1175)) : 8 + 4 * (l - log2(0.0334)));
    if (e < 1 && a >= 0.004f) e = 1;
    return e < 0 ? 0 : e > 100 ? 100 : (int)e;
}

void input_hd_rumble_encode(const InputVibration *v, uint8_t out[4]) {
    int hf = (hd_freq_code(v->freq_high, 320, 0x61, 0xdf) - 0x60) * 4, lf = hd_freq_code(v->freq_low, 160, 0x41, 0xbf) - 0x40;
    int eh = hd_amp_step(v->amp_high), el = hd_amp_step(v->amp_low);
    out[0] = (uint8_t)(hf & 0xff);
    out[1] = (uint8_t)(eh * 2 | hf >> 8);
    out[2] = (uint8_t)(lf | (el & 1) << 7);
    out[3] = (uint8_t)(0x40 + el / 2);
}

int input_rumble_hd(void) { return __atomic_load_n(&pad_hd, __ATOMIC_RELAXED) != 0; }

// On the gamepad thread: the game's two sides as one Switch rumble output packet, sent through SDL's HIDAPI Switch
// driver. SDL 3.4.18 (toolchain/deps-mingw) takes, in SDL_SendGamepadEffect, exactly ten bytes whose first is 0x10 (the
// rumble-only output report): it keeps bytes 2..5 as the left side and 6..9 as the right, numbers the packet itself and
// writes it, replacing any rumble of its own ("Unknown Nintendo Switch Pro effect type" for another first byte). A packet
// has no duration: hd_until keeps the ms of the request, and pad_main sends the neutral packet when they run out, as
// SDL_RumbleGamepad's duration would stop the pad when the game stops streaming. 0 when this pad does not take it (not a
// Switch Pro Controller or Joy-Con, or SDL refused the packet: logged once, and the pad keeps SDL_RumbleGamepad).
static int hd_send(const InputVibration hd[2], uint32_t ms, uint64_t now_ms) {
    if (!__atomic_load_n(&pad_hd, __ATOMIC_RELAXED)) return 0;
    uint8_t p[10] = {0x10, 0};
    input_hd_rumble_encode(&hd[0], p + 2);
    input_hd_rumble_encode(&hd[1], p + 6);
    if (!SDL_SendGamepadEffect(pad, p, sizeof p)) {
        rt_log("input: \"%s\" does not take HD rumble packets (%s): it gets the two-motor rumble", SDL_GetGamepadName(pad),
               SDL_GetError());
        plat_xchg32(&pad_hd, 0);
        hd_until = 0;
        return 0;
    }
    static int logged;
    if (!logged) logged = 1, rt_log("input: HD rumble: the game's two bands per side go to \"%s\"", SDL_GetGamepadName(pad));
    hd_until = ms ? now_ms + ms : 0;
    return 1;
}

int input_pad_layout(void) {
    return (int)plat_cas32(&pad_layout_shown, 0, 0);
}

int input_pad_label(int pad_id) {
    if (pad_id < PAD_SOUTH || pad_id > PAD_NORTH) return PAD_LABEL_NONE;
    return (int)plat_cas32(&pad_labels[pad_id - PAD_SOUTH], 0, 0);
}

int input_pad_family(void) { return (int)__atomic_load_n(&pad_family, __ATOMIC_RELAXED); }

int input_pad_in_use(void) {
    return input_pad_family() >= 0 && __atomic_load_n(&prompt_dev, __ATOMIC_RELAXED) != DEV_KBM;
}

// The prompts REKARINA_PROMPTS asks for (input_prompts_pick). Unset is the settings table's default (auto for OoT), except
// that 0.1.9's pad_layout = xbox_prompts still means the Xbox prompts then. Read again when cfg_version changes: the
// prompts' switch asks every present (romfs.c romfs_prompts_live). The memo is the version and the pick in one word,
// so a thread that read an older version leaves a memo the next call reads again.
static int prompts_pick_now(void) {
    static volatile uint64_t memo = ~0ull;   // cfg_version << 32 | the pick
    unsigned v = cfg_version();
    uint64_t m = __atomic_load_n(&memo, __ATOMIC_ACQUIRE);
    if (m != ~0ull && (unsigned)(m >> 32) == v) return (int)(uint32_t)m;
    const char *set = cfg_get("REKARINA_PROMPTS"), *layout = cfg_value("REKARINA_PAD_LAYOUT");
    if ((!set || !*set) && strcasecmp(layout, "xbox_prompts")) set = cfg_value("REKARINA_PROMPTS");
    int pick = input_prompts_pick(set, layout);
    __atomic_store_n(&memo, (uint64_t)v << 32 | (uint32_t)pick, __ATOMIC_RELEASE);
    return pick;
}

int input_prompts_auto(void) { return prompts_pick_now() == PROMPTS_AUTO; }

const char *input_prompts_wanted(void) {
    int pick = prompts_pick_now();
    if (pick != PROMPTS_AUTO || !__atomic_load_n(&state, __ATOMIC_RELAXED))
        return input_prompts_folder(pick, cfg_value("REKARINA_PAD_LAYOUT"));
    int fam = input_pad_family(), dev = (int)__atomic_load_n(&prompt_dev, __ATOMIC_RELAXED);
    // Right after SDL was started a connected pad may not be listed yet (scan_pads at start found none, the gamepad
    // thread scans again after RESCAN_MS): the keyboard's set would show, then the pad's a second later, each change a
    // rewrite of the font's table and the action hint's images. Before the first press, for 1.5 s from the first ask
    // under auto, the set loaded at start stays and the thread scans every 50 ms. Nothing waits: romfs.c asks at each
    // present.
    if (fam < 0 && dev == DEV_NONE) {
        static uint64_t first_ns, forced_ns;
        static int told;
        uint64_t now = plat_now_ns(), t0 = __atomic_load_n(&first_ns, __ATOMIC_ACQUIRE);
        if (!t0) {   // the first ask starts the window; a racing first ask keeps the other's start
            uint64_t none = 0;
            t0 = __atomic_compare_exchange_n(&first_ns, &none, now, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) ? now : none;
        }
        const char *loaded = romfs_prompts();
        if (now - t0 < 1500000000ull && loaded) {
            if (now - __atomic_load_n(&forced_ns, __ATOMIC_RELAXED) >= 50000000ull) {
                __atomic_store_n(&forced_ns, now, __ATOMIC_RELAXED);
                __atomic_store_n(&next_scan, 0, __ATOMIC_RELAXED);
            }
            return loaded;
        }
        if (!__atomic_exchange_n(&told, 1, __ATOMIC_ACQ_REL))
            rt_log("input: start: no gamepad %.0f ms after the first ask, the prompts follow the keyboard",
                   (now - t0) / 1e6);
    }
    int kind = fam >= 0 && dev != DEV_KBM ? fam : PROMPTS_PC;
    // A game folder from an older setup has no keyboard set (the menu does not offer it): the pad's set stays.
    if (kind == PROMPTS_PC && !romfs_prompts_ready("romfs-patches-pc")) kind = fam >= 0 ? fam : PROMPTS_XBOX;
    if (kind == PROMPTS_PC) return input_prompts_device_folder(kind, PAD_LAYOUT_XBOX);   // no layout to follow
    // The layout in effect; a pad opened since the last poll gets the one pad_layout_get will pick for its labels.
    int l = input_pad_layout();
    if (l < 0) l = input_pad_layout_pick(cfg_value("REKARINA_PAD_LAYOUT"), fam == PROMPTS_NINTENDO);
    return input_prompts_device_folder(kind, l);
}

void input_map_snapshot(InputMap *m) {
    plat_mutex_lock(&lock);
    if (!state) start();
    if (state > 0) binds_refresh();
    *m = input_map;
    plat_mutex_unlock(&lock);
}

int input_capture(int reset, int *id) {
    static uint8_t prev_k[KEY_COUNT], prev_p[PAD_COUNT];
    static int prev_esc;
    uint8_t k[KEY_COUNT] = {0}, p[PAD_COUNT] = {0};
    int fg = ikb_foreground();
    for (int i = 1; i < KEY_COUNT; i++) k[i] = fg && ikb_down(i);
    int esc = fg && ikb_escape();
    plat_mutex_lock(&lock);
    if (!state) start();
    if (state == 1) pad_physical(p, NULL);
    plat_mutex_unlock(&lock);
    int got = INPUT_CAPTURE_NONE;
    if (!reset) {
        if (esc && !prev_esc) got = INPUT_CAPTURE_CANCEL;
        for (int i = 1; i < KEY_COUNT && !got; i++)
            if (k[i] && !prev_k[i]) got = INPUT_CAPTURE_KEY, *id = i;
        for (int i = 1; i < PAD_COUNT && !got; i++)
            if (p[i] && !prev_p[i]) got = INPUT_CAPTURE_PAD, *id = i;
    }
    memcpy(prev_k, k, sizeof k);
    memcpy(prev_p, p, sizeof p);
    prev_esc = esc;
    return got;
}

int input_rumble_triggers_enabled(void) {
    return cfg_bool("REKARINA_RUMBLE_TRIGGERS");
}

int input_rumble(uint16_t low, uint16_t high, const InputVibration hd[2], uint32_t ms) {
    int ok = 0;
    static int triggers = -1;
    static unsigned seen;
    if (triggers < 0 || seen != cfg_version()) seen = cfg_version(), triggers = input_rumble_triggers_enabled();
    plat_mutex_lock(&snap_lock);
    if (snap.on && snap.rumble) {   // the gamepad thread sends the newest one: a rumble can block on a wireless pad
        snap.want_rumble = 1, snap.want_triggers = triggers, snap.low = low, snap.high = high, snap.ms = ms;
        snap.want_hd = hd != NULL;
        if (hd) memcpy(snap.hd, hd, sizeof snap.hd);
        ok = 1;
    }
    plat_mutex_unlock(&snap_lock);
    return ok;
}

// SDL gamepad sensor axes (SDL_sensor.h, a gamepad held in front of you): x to the right, y up out of the face (lying
// flat face up, +y points at the ceiling), z toward the player (out of the bottom edge). The accelerometer reads the
// push against gravity in m/s^2 (lying flat: y = +9.81); the gyro reads rad/s, counter-clockwise positive (right hand).
// Switch six-axis axes (nn::hid, as MacNativeGames' GCMotion feed and its Pro Controller HID path use them): x to the
// right, y toward the top edge (away from the player), z out of the face; acceleration as gravity reads (lying flat:
// z = -1 G). So Switch x = SDL x, Switch y = -SDL z, Switch z = SDL y: a rotation (right-handed to right-handed), so the
// gyro keeps its sense, while the acceleration is also negated (push -> gravity) and scaled to G.
void input_motion_axes(const float a[3], const float g[3], double acc[3], double gyro[3]) {
    acc[0] = -a[0] / SDL_STANDARD_GRAVITY;
    acc[1] = a[2] / SDL_STANDARD_GRAVITY;
    acc[2] = -a[1] / SDL_STANDARD_GRAVITY;
    gyro[0] = g[0];
    gyro[1] = -g[2];
    gyro[2] = g[1];
}

// On the gamepad thread: (re)enable the sensors when the open gamepad changed; 1 if it has both and they are on.
static int sensors_ready(void) {
    SDL_JoystickID id = SDL_GetGamepadID(pad);
    if (id == sensor_pad) return sensor_ok;
    sensor_pad = id;
    // Nothing drains the SDL event queue: no event per sample (the data still updates, SDL_SendJoystickSensor)
    SDL_SetEventEnabled(SDL_EVENT_GAMEPAD_SENSOR_UPDATE, false);
    sensor_ok =SDL_GamepadHasSensor(pad, SDL_SENSOR_GYRO) && SDL_GamepadHasSensor(pad, SDL_SENSOR_ACCEL) &&
                SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_GYRO, true) &&
                SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_ACCEL, true);
    if (sensor_ok) rt_log("input: motion sensors on (gyro %.0f Hz, accelerometer %.0f Hz)",
                          (double)SDL_GetGamepadSensorDataRate(pad, SDL_SENSOR_GYRO),
                          (double)SDL_GetGamepadSensorDataRate(pad, SDL_SENSOR_ACCEL));
    else rt_log("input: \"%s\" has no gyro and accelerometer: the six-axis sensor reads a controller at rest",
                SDL_GetGamepadName(pad));
    return sensor_ok;
}

int input_motion_enabled(void) {
    static int on = -1;
    static unsigned seen;
    unsigned v = cfg_version();
    if (on < 0 || v != seen) {
        seen = v;
        on = cfg_bool("REKARINA_MOTION");
    }
    return on;
}

// 250 Hz: a gamepad that came or went is picked up, the gamepads update (reports read, rumble expiry), their state goes
// into the snapshot the game's poll reads, a waiting rumble is sent, and the newest gyro and accelerometer sample goes into
// the fusion. Without a gamepad with sensors, or with motion off (REKARINA_MOTION=0, live), nothing is fed and the six-axis
// sensor reads at rest.
static void pad_main(void *arg) {
    (void)arg;
    for (;;) {
        plat_wait_ns(4000000);
        uint64_t now = plat_now_ns() / 1000000u;
        if (now >= next_scan) next_scan = now + RESCAN_MS, scan_pads();
        SDL_UpdateGamepads();
        pad_follow_press();
        int on = pad && SDL_GamepadConnected(pad);
        uint8_t pd[PAD_COUNT] = {0};
        int16_t ax[SDL_GAMEPAD_AXIS_COUNT] = {0};
        if (on) {
            for (int p = PAD_SOUTH; p < PAD_ZL; p++) pd[p] = SDL_GetGamepadButton(pad, pad_sdl[p]);
            pd[PAD_ZL] = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > TRIGGER_ON;
            pd[PAD_ZR] = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > TRIGGER_ON;
            for (int p = PAD_ZR + 1; p < PAD_COUNT; p++) pd[p] = SDL_GetGamepadButton(pad, pad_sdl[p]);   // false without paddles
            for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT; a++) ax[a] = SDL_GetGamepadAxis(pad, (SDL_GamepadAxis)a);
        }
        plat_mutex_lock(&snap_lock);
        snap.on = on, snap.rumble = on && pad_rumble;
        snap.guide = on && SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_GUIDE);
        memcpy(snap.pd, pd, sizeof pd), memcpy(snap.ax, ax, sizeof ax);
        int rumble = snap.want_rumble, triggers = snap.want_triggers, has_hd = snap.want_hd;
        uint16_t low = snap.low, high = snap.high;
        uint32_t ms = snap.ms;
        InputVibration hd[2];
        memcpy(hd, snap.hd, sizeof hd);
        snap.want_rumble = 0;
        plat_mutex_unlock(&snap_lock);
        // Rumble goes out at most once per RUMBLE_GAP ms (REKARINA_RUMBLE_GAP_MS, default 30, 0 = every request), the newest
        // request waiting for its turn. 30 ms is the gap SDL's own Switch driver keeps between its rumble writes (its notes:
        // writing more often can turn the controller off over Bluetooth). SDL_SendGamepadEffect writes each HD packet at
        // once, so the HD path needs this pacing; the two-motor path keeps it too, for drivers that write each change at
        // once. The game sends a changed value at most every 10 ms and an unchanged one every 50 ms (ov_input.c
        // rumble_update): a two-motor value equal to the last one written only renews SDL's duration, and SDL writes
        // nothing to the pad for it, so it goes at once. A player's left Joy-Con of a pair dropped while a held note
        // vibrated: the pair's left half plays the heavy motor alone (SDL's Switch driver), and rumble_wrote's counts go
        // into the log line of a gamepad that went away.
        static int r_wait, r_hd, r_gap = -1;
        static InputVibration r_next[2];
        static uint32_t r_ms;
        static uint16_t r_low, r_high;
        if (r_gap < 0) {
            const char *g = cfg_get("REKARINA_RUMBLE_GAP_MS");
            r_gap = g && *g ? atoi(g) : 30;
            if (r_gap < 0 || r_gap > 200) r_gap = 30;
        }
        if (on && !rumble && !r_wait && hd_until && now >= hd_until) {   // the game stopped streaming: stop as a duration would
            static const InputVibration rest[2] = {{0, 160, 0, 320}, {0, 160, 0, 320}};
            if (hd_send(rest, 0, now)) rumble_wrote(now, 0, 0);
        }
        if (on && rumble) {
            r_hd = has_hd && input_rumble_hd();
            if (r_hd) memcpy(r_next, hd, sizeof r_next);
            r_low = low, r_high = high, r_ms = ms, r_wait = 1;
            if (!r_hd && low == rumble_low && high == rumble_high) {   // a renewal: SDL keeps the value, writes nothing
                SDL_RumbleGamepad(pad, low, high, ms);
                r_wait = 0;
            }
            if (triggers) SDL_RumbleGamepadTriggers(pad, high, high, ms);   // unsupported pads return false: ignored
        }
        if (on && r_wait && (!rumble_at || now - rumble_at >= (uint64_t)r_gap)) {
            r_wait = 0;
            if (!r_hd || !hd_send(r_next, r_ms, now)) SDL_RumbleGamepad(pad, r_low, r_high, r_ms);
            rumble_wrote(now, r_low, r_high);
        }
        if (!on) r_wait = 0;
        float a[3], g[3];
        int have = on && __atomic_load_n(&motion_on, __ATOMIC_RELAXED) && sensors_ready() &&
                   SDL_GetGamepadSensorData(pad, SDL_SENSOR_ACCEL, a, 3) && SDL_GetGamepadSensorData(pad, SDL_SENSOR_GYRO, g, 3);
        if (!have || !input_motion_enabled()) continue;
        double acc[3], gyro[3];
        input_motion_axes(a, g, acc, gyro);
        motion_feed(motion_global(), acc, gyro, kern_now_ns());
    }
}

void input_motion_start(void) {
    plat_mutex_lock(&lock);
    if (!state) start();
    if (!motion_thread) {
        if (!input_motion_enabled()) rt_log("input: motion off (REKARINA_MOTION=0); it can be turned on while the game runs");
        motion_thread = state == 1 ? 1 : -1;
        __atomic_store_n(&motion_on, state == 1, __ATOMIC_RELAXED);
    }
    plat_mutex_unlock(&lock);
}
