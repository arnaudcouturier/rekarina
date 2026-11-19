// Direct mouse input for TotK 1.4.3. The camera controller's input update (main+0x874c1c) turns HID stick floats
// into a radial dead zone, clamps the radius to one and applies StickExponent. The mouse enters after all three,
// at main+0x874cb4, before the game's X/Y inversion and input-activity flags. It never becomes a HID stick value.
// Free orbit removes that sample again before the camera's additional vertical square/velocity filters and adds
// raw-count angular deltas at its spherical-angle output instead. Ultrahand has a separate post-curve hook.
// The ordinary stick's result stays, so a controller still uses the game's own response.
//
// This is deliberately not a render-camera override: the game remains responsible for aiming, camera collision,
// lock-on and scripted cameras. Which of its other modes consume this camera input needs in-game verification.
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "input.h"
#include "input_wsys.h"
#include "platform/platform.h"

#if __has_include("patches.h")
#include "layout.h"
#include "patches.h"
#endif

static int direct_mode(void) {
    // cfg_value: unset is the settings table's default (direct), as the menu shows it
    return !strcmp(cfg_value("REKARINA_MOUSE_MODE"), "direct") && rt_patch_enabled(rt_patch_set("mouse_direct"));
}

static uint64_t last_update;
static uint64_t hand_update;
static int accepts_mouse;

static int hand_active(uint64_t now) {
    uint64_t last = __atomic_load_n(&hand_update, __ATOMIC_ACQUIRE);
    return last && now - last < 150000000ull;
}

int input_direct_mouse_active(void) {
    uint64_t last = __atomic_load_n(&last_update, __ATOMIC_ACQUIRE);
    return direct_mode() && (__atomic_load_n(&accepts_mouse, __ATOMIC_RELAXED) || hand_active(plat_now_ns())) && last &&
           plat_now_ns() - last < 150000000ull;
}

int input_direct_mouse_enabled(void) { return direct_mode(); }

#ifdef GAME_OOT
static int oot_stick_mode;   // the OoT hook leaves the mouse to the stick (oot_camera_input)
#endif
int input_direct_mouse_stick_curve(double *deg_per_count, double *full_deg_s) {
#ifdef GAME_OOT
    uint64_t last = __atomic_load_n(&last_update, __ATOMIC_ACQUIRE);
    if (direct_mode() && __atomic_load_n(&oot_stick_mode, __ATOMIC_RELAXED) && last && plat_now_ns() - last < 150000000ull) {
        // 1.78 degrees a frame at full deflection on land at 60 fps, measured with scripted stick steps; the swimming
        // camera turned 0.61 degrees a frame at 0.61 (0.66 by the same curve). REKARINA_MOUSE_STICK_RATE: degrees a second.
        static double rate = -1;
        if (rate < 0) { const char *e = cfg_get("REKARINA_MOUSE_STICK_RATE"); rate = e && *e ? atof(e) : 107.0; }
        *deg_per_count = 0.08, *full_deg_s = rate;
        return 1;
    }
#endif
    (void)deg_per_count, (void)full_deg_s;
    return 0;
}

void input_direct_mouse_radial(int32_t dx, int32_t dy, float sensitivity, float cursor[2], int32_t out[2]) {
    if (!(sensitivity > 0)) { cursor[0] = cursor[1] = 0; out[0] = out[1] = 0; return; }
    cursor[0] += dx * (double)sensitivity / 200;
    cursor[1] -= dy * (double)sensitivity / 200;
    double length = hypot(cursor[0], cursor[1]);
    if (length > 1) { cursor[0] /= length; cursor[1] /= length; }
    out[0] = (int32_t)lrint(cursor[0] * 32767);
    out[1] = (int32_t)lrint(cursor[1] * 32767);
}

void input_direct_mouse_row(int32_t dx, float sensitivity, uint64_t now, InputMouseRow *row, int32_t out[2]) {
    const float step_counts = 32;
    out[0] = out[1] = 0;
    if (!(sensitivity > 0)) { *row = (InputMouseRow){0}; return; }
    if (dx) {
        // Reversing direction cancels any remainder from the previous drag. Bound a fast flick to one pending step;
        // a stopped mouse never starts another pulse, regardless of how often HID is polled.
        if ((dx < 0 && row->distance > 0) || (dx > 0 && row->distance < 0)) row->distance = 0;
        row->distance = fmaxf(-2 * step_counts, fminf(2 * step_counts, row->distance + dx * sensitivity));
    }
    if (row->direction) {
        if (now < row->pulse_until && (!dx || (dx > 0 ? 1 : -1) == row->direction)) {
            out[0] = row->direction * 32767;
            return;
        }
        row->direction = 0;
        row->neutral_until = now + 32000000ull;
        return;   // publish neutral even when a slow frame skipped the pulse's deadline
    }
    if (!dx || now < row->neutral_until || fabsf(row->distance) < step_counts) return;
    row->direction = row->distance > 0 ? 1 : -1;
    row->distance = fmodf(row->distance, step_counts);
    row->pulse_until = now + 32000000ull;
    out[0] = row->direction * 32767;
}

#ifdef RT_PATCH_PAGE
#define HOST_MOUSE_DIRECT (RT_PATCH_PAGE + 0xfd0)
#define HOST_MOUSE_ACTIVITY (RT_PATCH_PAGE + 0xfc0)
#define HOST_MOUSE_ORBIT_BEGIN (RT_PATCH_PAGE + 0xfb0)
#define HOST_MOUSE_ORBIT_ANGLES (RT_PATCH_PAGE + 0xfa0)
#define HOST_MOUSE_HAND (RT_PATCH_PAGE + 0xf90)
static _Thread_local uint64_t mouse_input_object;

typedef struct {
    uint64_t object, generation, when;
    float pad[2], motion[2], angle[2];   // post-inversion controller input, mouse input, mouse yaw/pitch in degrees
    int camera_enabled;
} MouseSample;
static MouseSample sample;
static PlatMutex sample_lock = PLAT_MUTEX_INIT;
static _Thread_local struct { uint64_t object; MouseSample mouse; } orbit;
typedef struct {
    uint64_t object, generation, input;
    float pitch, original_target;
    int hold_pitch, target_saved;
} OrbitTrack;
static OrbitTrack orbit_seen[16];

// sample_lock held. Camera objects can coexist while the game blends between their outputs.
static OrbitTrack *orbit_track(uint64_t object) {
    int oldest = 0;
    for (int i = 0; i < 16; i++) {
        if (orbit_seen[i].object == object) return &orbit_seen[i];
        if (orbit_seen[i].generation < orbit_seen[oldest].generation) oldest = i;
    }
    orbit_seen[oldest] = (OrbitTrack){.object = object};
    return &orbit_seen[oldest];
}

static int orbit_sample_once(uint64_t object, uint64_t generation) {
    plat_mutex_lock(&sample_lock);
    OrbitTrack *track = orbit_track(object);
    int fresh = track->generation != generation;
    if (fresh) track->generation = generation;
    plat_mutex_unlock(&sample_lock);
    return fresh;
}

static float guest_float(uint64_t at) { float f; memcpy(&f, (void *)(uintptr_t)at, sizeof f); return f; }
static void guest_float_store(uint64_t at, float f) { memcpy((void *)(uintptr_t)at, &f, sizeof f); }
static uint64_t guest_pointer(uint64_t at) { uint64_t p; memcpy(&p, (void *)(uintptr_t)at, sizeof p); return p; }
static MouseSample sample_read(void) {
    plat_mutex_lock(&sample_lock);
    MouseSample m = sample;
    plat_mutex_unlock(&sample_lock);
    return m;
}
static float wrap_degrees(float a) { return remainderf(a, 360.0f); }

// main+0x191d78 builds the pitch target from +0x15c and +0x11c; the orbit update filters +0x108 toward it and adds
// (1 - +0x88) * +0x10c at output. Updating only +0x108 makes the camera spring back to the unchanged target.
static float orbit_pitch_offset(uint64_t object) {
    return (1 - guest_float(object + 0x88)) * guest_float(object + 0x10c);
}

static void orbit_pitch_target(uint64_t object, float pitch) {
    guest_float_store(object + 0x15c, pitch - orbit_pitch_offset(object) - guest_float(object + 0x11c));
}

static void orbit_pitch_release(OrbitTrack *track) {
    if (track->target_saved) guest_float_store(track->object + 0x15c, track->original_target);
    track->hold_pitch = track->target_saved = 0;
}

// A host call at an instruction site, not an AAPCS64 function replacement. Preserve every guest register except
// s9/s10, which carry the processed camera input, and perform the displaced store of s9 at [x20 + 0x420].
static void camera_input(Cpu *c) {
    mouse_input_object = 0;
    uint32_t flags = (uint32_t)c->x[21];
    int accept = !(flags & 1) && !input_direct_mouse_selector_active();
    __atomic_store_n(&accepts_mouse, accept, __ATOMIC_RELAXED);
    __atomic_store_n(&last_update, plat_now_ns(), __ATOMIC_RELEASE);
    MouseSample next = {.object = c->x[20], .when = plat_now_ns(), .camera_enabled = accept};
    memcpy(&next.pad[0], &c->v[9], sizeof(float));
    memcpy(&next.pad[1], &c->v[10], sizeof(float));
    if (flags & 2) next.pad[0] = -next.pad[0];
    if (flags & 4) next.pad[1] = -next.pad[1];
    if (direct_mode()) {
        int32_t dx, dy;
        input_direct_mouse_sample(!input_direct_mouse_selector_active() && (accept || hand_active(plat_now_ns())), &dx, &dy);
        float sensitivity = (float)cfg_double("REKARINA_MOUSE_SENS");
        sensitivity = fminf(10, fmaxf(0, sensitivity));
        // The game multiplies camera input by its 30 Hz frame ratio later. Divide by that exact ratio, rather
        // than wall-clock time: an equal distance of mouse motion then gives equal input at every frame rate.
        float ratio = 1;
        if (frame_step_realtime()) memcpy(&ratio, rt_patch_page(), sizeof ratio);
        else {
            double hz = frame_step_limit_hz();
            if (hz > 0) ratio = (float)(30 / hz);
        }
        if (isfinite(ratio) && ratio > 0 && sensitivity > 0 && (dx || dy)) {
            if (accept) mouse_input_object = c->x[20];
            float x, y;
            memcpy(&x, &c->v[9], sizeof x);
            memcpy(&y, &c->v[10], sizeof y);
            float scale = 0.01f * sensitivity / ratio;
            next.motion[0] = dx * scale * (flags & 2 ? -1 : 1);
            next.motion[1] = -dy * scale * (flags & 4 ? -1 : 1);
            next.angle[0] = -dx * 0.08f * sensitivity * (flags & 2 ? -1 : 1);
            next.angle[1] = dy * 0.08f * sensitivity * (flags & 4 ? -1 : 1);
            if (accept) { x += dx * scale; y -= dy * scale; }
            uint32_t xb, yb;
            memcpy(&xb, &x, sizeof xb);
            memcpy(&yb, &y, sizeof yb);
            c->v[9] = xb;
            c->v[10] = yb;
            static int logged;
            if (!logged) {
                logged = 1;
                rt_log("mouse: direct camera input after the stick dead zone, clamp and exponent");
            }
        }
    }
    plat_mutex_lock(&sample_lock);
    next.generation = sample.generation + 1;
    sample = next;
    plat_mutex_unlock(&sample_lock);
    uint32_t original = (uint32_t)c->v[9];
    memcpy((void *)(uintptr_t)(c->x[20] + 0x420), &original, sizeof original);
}

// Free camera has another signed-square curve for its vertical axis, plus mode-dependent velocity filters.
// Remove only this mouse sample before that controller runs. At its final spherical-angle output, add degrees
// directly and move its persistent pitch/yaw by the same amounts so the next update starts at the new orientation.
// The controller still handles its gamepad input, following, collision and focus normally.
static void orbit_begin(Cpu *c) {
    orbit.object = 0;
    uint64_t object = c->x[0];
    if (direct_mode()) {
        MouseSample m = sample_read();
        uint64_t input = guest_pointer(object + 0x20);
        if (m.object == input && m.camera_enabled) {
            // The hooks take a step when the mouse moved in it, or while the mouse holds the pitch. A step of the
            // gamepad alone (no mouse motion, no hold: a player who never moved the mouse) keeps the game's input.
            int moved = m.motion[0] != 0 || m.motion[1] != 0, take;
            uint64_t held = guest_pointer(input + 0x148);
            uint8_t override;
            memcpy(&override, (void *)(uintptr_t)(input + 0x15c), 1);
            plat_mutex_lock(&sample_lock);
            OrbitTrack *track = orbit_track(object);
            if (track->input != input) track->hold_pitch = track->target_saved = 0;
            if (m.pad[1] != 0 || (held & 0x20) || (override & 0x40)) orbit_pitch_release(track);
            take = moved || track->hold_pitch;
            if (take) track->input = input;
            if (track->hold_pitch) orbit_pitch_target(object, track->pitch);
            plat_mutex_unlock(&sample_lock);
            if (moved) {   // the mouse's part of this step's stick goes to the final angles instead
                guest_float_store(input + 0x420, m.pad[0]);
                guest_float_store(input + 0x424, m.pad[1]);
            }
            if (take) orbit.object = object, orbit.mouse = m;
        }
    } else {
        plat_mutex_lock(&sample_lock);
        orbit_pitch_release(orbit_track(object));
        plat_mutex_unlock(&sample_lock);
    }
    c->x[8] = guest_pointer(object + 8);   // displaced ldr x8, [x0, #8]
}

static void orbit_angles(Cpu *c) {
    uint64_t object = c->x[20];
    if (orbit.object == object) {
        float yaw_delta = orbit.mouse.angle[0], pitch_delta = orbit.mouse.angle[1];
        int fresh = plat_now_ns() - orbit.mouse.when < 150000000ull &&
                    orbit_sample_once(object, orbit.mouse.generation);
        if (!fresh) yaw_delta = pitch_delta = 0;
        plat_mutex_lock(&sample_lock);
        OrbitTrack *track = orbit_track(object);
        if (track->hold_pitch || yaw_delta || pitch_delta) {
            float pitch = guest_float(c->sp + 0x44), yaw = guest_float(c->sp + 0x48);
            float low = guest_float(object + 0xf8), high = guest_float(object + 0xfc);
            if (!isfinite(low) || !isfinite(high) || low >= high) low = -89.9f, high = 89.9f;
            low = fmaxf(-89.9f, low); high = fminf(89.9f, high);
            // The game can recalculate its pitch after orbit_begin (following, mode changes and another filter).
            // Integrate from our last requested angle, then correct the final output even on stationary frames.
            float base = track->hold_pitch ? track->pitch : pitch;
            float new_pitch = fminf(high, fmaxf(low, base + pitch_delta));
            guest_float_store(c->sp + 0x44, new_pitch);
            guest_float_store(c->sp + 0x48, wrap_degrees(yaw + yaw_delta));
            guest_float_store(object + 0x108, wrap_degrees(new_pitch - orbit_pitch_offset(object)));
            guest_float_store(object + 0x120, wrap_degrees(guest_float(object + 0x120) + yaw_delta));
            if (!track->target_saved) track->original_target = guest_float(object + 0x15c), track->target_saved = 1;
            track->pitch = new_pitch; track->input = orbit.mouse.object; track->hold_pitch = 1;
            orbit_pitch_target(object, new_pitch);
            static int logged;
            if (!logged) {
                logged = 1;
                rt_log("mouse: free-camera angles directly, 0.08 degrees per count at sensitivity 1, equal X/Y");
            }
        }
        plat_mutex_unlock(&sample_lock);
        orbit.object = 0;
    }
    uint32_t bits;
    memcpy(&bits, (const void *)(uintptr_t)(c->sp + 0x44), sizeof bits);
    c->v[12] = bits;   // displaced ldr s12, [sp, #0x44]
}

// Ultrahand reads the player's manipulation input rather than the camera's processed stick. Insert the mouse
// after that controller's per-axis power curves (main+0x1d59460), preserving the keyboard/controller contribution.
static void hand_input(Cpu *c) {
    __atomic_store_n(&hand_update, plat_now_ns(), __ATOMIC_RELEASE);
    static _Thread_local uint64_t object, generation;
    if (direct_mode()) {
        MouseSample m = sample_read();
        if (m.generation != generation || object != c->x[19]) {
            float x = guest_float(c->sp + 0x60), y;
            memcpy(&y, &c->v[15], sizeof y);
            x += m.motion[0];
            y += m.motion[1];
            guest_float_store(c->sp + 0x60, x);
            uint32_t bits;
            memcpy(&bits, &y, sizeof bits);
            c->v[15] = bits;
            object = c->x[19]; generation = m.generation;
            static int logged;
            if (!logged && (m.motion[0] || m.motion[1])) {
                logged = 1;
                rt_log("mouse: held-object input after the manipulation controller's power curves");
            }
        }
    }
    uint32_t original = (uint32_t)c->x[8];
    memcpy((void *)(uintptr_t)(c->sp + 0x48), &original, sizeof original);
}

// The game also uses a 10% activity threshold after processing the stick. A one-count mouse movement must count
// as active, even when its displacement is smaller than that threshold. The site is the displaced ldp d9, d8, [sp, #8]
// just before the update's last `str w8, [x20, #0x418]`: w8 holds the flags then (bit 0 raw input, bit 4 any input,
// bits 3 and 5 processed input below the threshold), so they are changed in w8; a write to +0x418 would be overwritten
// by that store. No flag is changed for controller-only input.
static void camera_activity(Cpu *c) {
    if (mouse_input_object && mouse_input_object == c->x[20]) {
        uint32_t flags = (uint32_t)c->x[8];
        c->x[8] = (flags | 0x11u) & ~0x28u;
        mouse_input_object = 0;
    }
    uint64_t d9, d8;
    memcpy(&d9, (const void *)(uintptr_t)(c->sp + 8), sizeof d9);
    memcpy(&d8, (const void *)(uintptr_t)(c->sp + 16), sizeof d8);
    c->v[9] = d9;
    c->v[8] = d8;
}

#ifdef GAME_OOT
// OoT (recomp/patches-oot-v111.txt, set mouse_direct): the host call at main+0x37c45c, inside the camera's stick update,
// in place of `str s1, [x20, #0x538]`. x20 is the camera's input object, s1 and s0 its curved stick x and y, w21 the
// options' camera flags (bit 0 no input, bits 1 and 2 invert x and y), +0x5e0 a lock timer during which the game zeroes
// the stick. The store is performed, then the mouse's counts turn the camera's own angles at 0.08 degrees a count times
// the sensitivity (the scale TotK's orbit camera takes): the yaw at +0xdb4 and the pitch at +0xd9c of the camera that
// +0xa80 points to, both in degrees. The input object keeps copies of them (+0x52c, +0x528) that the game makes each
// frame: writing those changes nothing. Found 2026-11-15 by searching memory for values that moved with the view's yaw
// and writing each (docs/oot-notes.md, "The mouse camera"). Every register stays as it was but s0 (below).
// Should the game update more than one such object a frame, each takes all the motion since it last ran, from one
// running total, so none misses counts another took; an object not seen for 200 ms starts from the total as it is (no
// burst when a camera becomes active).
// A camera whose angle is not where this writes (swimming: the copy and +0xa80's +0xdb4 differ by far more than a frame's
// turn) is turned by oot_inject below.
#define HOST_MOUSE_OOT (RT_PATCH_PAGE + 0xf80)
#define HOST_MOUSE_OOT_FLAGS (RT_PATCH_PAGE + 0xf70)
static _Thread_local uint64_t oot_moved;   // the object the mouse turned in this update (oot_camera_flags)
static int64_t oot_total[2];
static struct { uint64_t object, when; int64_t seen[2]; float turned[2]; } oot_seen[8];   // turned: last yaw, pitch added

// The cameras that do not fit: the house cameras, the lock-on (attention) camera, the water's. Each turns the view by
// the input this update stores (+0x538 x, +0x53c y; the stick after its curve) times a rate of its own, and none of
// them holds the input to the stick's range (measured 2026-11-19 with the counts as input: the house camera 138 degrees
// a second per unit, linear up to 1.8; the lock-on camera about 60; the swimming one about 107, as the land camera).
// Until then such a camera got the mouse through the HID stick (input.c mouse_camera), calibrated for the land camera
// and held to a full stick: the house turned faster than the mouse said, the lock-on slower, and a fast move was cut at
// the stick's top speed everywhere ("the mouse is slow indoors, in the water"). Here the counts become that input: the
// angle the mouse asks for in this update (0.08 degrees a count times the sensitivity, as the direct path) over the
// update's time and the camera's rate, added to the stick's own input, without the curve or a cap. The rate is learnt
// from the view the game made of it (the yaw's copy +0x52c; the pitch takes the same rate): every 8 updates, the
// view's turn over the input given times its time, half way to it, kept within 20..600; a new spell (no call for 200 ms) starts from the land
// camera's 107 and settles in about half a second. A camera that does not answer (input given, the view still for 4
// such windows in a row) is left to the stick path for the rest of the spell. REKARINA_MOUSE_INJECT=0 leaves every such
// camera to the stick path as before; a number fixes the rate (degrees a second per unit) instead of learning it.
// Returns 1 when it took the update's mouse (the caller then reports the mouse as taken). Every register stays but s0.
static int oot_inject(Cpu *c, uint64_t self, uint32_t flags, uint32_t sx, uint64_t now) {
    static int mode = -1;   // 0 off, 1 learnt, 2 fixed
    static double fixed_rate;
    if (mode < 0) {
        const char *e = cfg_get("REKARINA_MOUSE_INJECT");
        mode = !e || !*e || !strcmp(e, "auto") ? 1 : atof(e) > 0 ? 2 : 0;
        fixed_rate = mode == 2 ? atof(e) : 0;
    }
    if (!mode) return 0;
    static uint64_t prev_now;
    static float prev_view, prev_in[2];
    static double prev_dt, acc_in, acc_view, rate;
    static int acc_n, dead, refused;
    double dt = prev_now && now > prev_now ? (now - prev_now) / 1e9 : 1.0 / 60;
    float view = guest_float(self + 0x52c), pitch = guest_float(self + 0x528);
    if (!prev_now || now - prev_now > 200000000ull) {   // a new spell
        rate = mode == 2 ? fixed_rate : 107, acc_in = acc_view = 0, acc_n = dead = refused = 0;
        prev_in[0] = prev_in[1] = 0, prev_view = view;
    }
    prev_now = now;
    if (refused) return 0;
    dt = dt < 0.001 ? 0.001 : dt > 0.05 ? 0.05 : dt;
    float turned = remainderf(view - prev_view, 360.0f);
    if (fabsf(turned) < 45) {   // a cut to another view is no turn
        acc_in += prev_in[0] * prev_dt;
        acc_view += turned;
        if (++acc_n >= 8) {
            if (fabs(acc_in) > 0.02) {
                double r = -acc_view / acc_in;   // the stick's +x turns the view right: the yaw goes down
                if (mode == 1 && r >= 20 && r <= 600) rate = 0.5 * rate + 0.5 * r;
                dead = fabs(acc_view) < 0.2 ? dead + 1 : 0;
                if (dead >= 4) {
                    refused = 1;
                    rt_log("mouse: a camera did not turn by the mouse given as its stick input: the stick path turns it");
                    return 0;
                }
            }
            acc_in = acc_view = 0, acc_n = 0;
        }
    }
    prev_view = view;
    int32_t dx, dy;
    input_direct_mouse_sample(1, &dx, &dy);
    float sensitivity = fminf(10, fmaxf(0, (float)cfg_double("REKARINA_MOUSE_SENS")));
    // degrees this update; the stick's +x turns the view right as the mouse's +x does, its +y looks up
    double turn_x = dx * 0.08 * sensitivity * (flags & 2 ? -1 : 1), turn_y = -dy * 0.08 * sensitivity * (flags & 4 ? -1 : 1);
    float st[2], in[2];
    memcpy(&st[0], &sx, 4);
    uint32_t y0 = (uint32_t)c->v[0];
    memcpy(&st[1], &y0, 4);
    in[0] = st[0] + (float)(turn_x / (rate * dt));
    in[1] = st[1] + (float)(turn_y / (rate * dt));
    memcpy((void *)(uintptr_t)(self + 0x538), &in[0], sizeof in[0]);
    uint32_t yb;
    memcpy(&yb, &in[1], sizeof yb);
    c->v[0] = yb;   // the caller's next instruction stores it at +0x53c
    prev_in[0] = in[0], prev_in[1] = in[1], prev_dt = dt;
    if (dx || dy) oot_moved = self;
    static int logged;
    if (!logged) logged = 1, rt_log("mouse: a camera the direct path does not fit takes the mouse as its stick input (no "
                                    "curve, no cap, its rate learnt from the view)");
    {   // DEBUG-MOUSELOG: one line per such update
        static int on = -1;
        if (on < 0) { const char *e = cfg_get("REKARINA_MOUSELOG"); on = e && *e == '1'; }
        if (on)
            rt_log("INJECT t=%.4f dt=%.4f dx=%d dy=%d in=%.3f %.3f view=%.3f pitch=%.3f cam=0 rate=%.1f", now / 1e9, dt, dx,
                   dy, in[0], in[1], view, pitch, rate);
    }
    return 1;
}
static void oot_camera_input(Cpu *c) {
    uint64_t self = c->x[20];
    oot_moved = 0;
    uint32_t sx = (uint32_t)c->v[1];
    memcpy((void *)(uintptr_t)(self + 0x538), &sx, sizeof sx);
    uint32_t flags = (uint32_t)c->x[21];
    int accept = !(flags & 1) && !(guest_float(self + 0x5e0) > 0) && !input_direct_mouse_selector_active();
    uint64_t now = plat_now_ns();
    // The angle the camera keeps is not this object's +0x52c, a copy the game makes each frame, but +0xdb4 of the camera
    // its +0xa80 points to. The copy trails the angle by a frame: it lacks the turn this hook added last time, and the
    // stick's turn of a frame (under 2 degrees at 60 fps). The angle is written only while it is that close to the copy,
    // so another layout is left alone; a 1-degree test failed every other frame of a steady mouse (the frame's turn was
    // 1.09 degrees) and those frames' counts were lost: a jerky camera. Tested before the counts are taken, so a camera
    // that does not fit gets them as its stick input instead (oot_inject).
    // Swimming, a house, the lock-on: the view is another camera's and the one at +0xa80 stops (its yaw and pitch stay
    // where they were; writing them turns nothing). The pitch copy shows it at once: on land it equals the camera's pitch
    // within a hundredth of a degree, in the water it moved 10 to 15 degrees away in the first frames. So both angles
    // are tested.
    uint64_t cam = guest_pointer(self + 0xa80);
    float last_turn = 0, last_pitch = 0;
    plat_mutex_lock(&sample_lock);
    for (int i = 0; i < 8; i++)
        if (oot_seen[i].object == self && now - oot_seen[i].when <= 200000000ull)
            last_turn = oot_seen[i].turned[0], last_pitch = oot_seen[i].turned[1];
    plat_mutex_unlock(&sample_lock);
    int fits = cam && fabsf(remainderf(guest_float(cam + 0xdb4) - guest_float(self + 0x52c), 360.0f)) <= 10.0f + fabsf(last_turn) &&
               fabsf(guest_float(cam + 0xd9c) - guest_float(self + 0x528)) <= 2.0f + fabsf(last_pitch);
    {   // DEBUG-MOUSELOG: REKARINA_MOUSELOG=1: one line per camera update (object, camera, the angles and their copies, the fit)
        static int on = -1;
        if (on < 0) { const char *e = cfg_get("REKARINA_MOUSELOG"); on = e && *e == '1'; }
        if (on)
            rt_log("MOUSELOG t=%.3f self=%llx cam=%llx yaw=%.2f copy=%.2f pitch=%.2f pcopy=%.2f flags=%x lock=%.2f last=%.2f fits=%d "
                   "vt=%llx camvt=%llx sx=%.3f sy=%.3f",
                   now / 1e9, (unsigned long long)self, (unsigned long long)cam, cam ? guest_float(cam + 0xdb4) : 0.0f,
                   guest_float(self + 0x52c), cam ? guest_float(cam + 0xd9c) : 0.0f, guest_float(self + 0x528), flags,
                   guest_float(self + 0x5e0), last_turn, fits, (unsigned long long)(guest_pointer(self) - MOD_MAIN_BASE),
                   cam ? (unsigned long long)(guest_pointer(cam) - MOD_MAIN_BASE) : 0ull, guest_float(self + 0x538),
                   guest_float(self + 0x53c));
    }
    {   // DEBUG-CAMSCAN: REKARINA_CAMSCAN=1: twice a second, every float that equals the view's yaw (the copy +0x52c, in
        // degrees, or its opposite, or either in radians) in the input object, the camera at +0xa80 and the objects their
        // words point to (=2: and the objects those point to): where a camera that does not fit keeps its angle (compare
        // the lines of two view angles)
        static int on = -1;
        static uint64_t next;
        if (on < 0) { const char *e = cfg_get("REKARINA_CAMSCAN"); on = e && *e >= '1' && *e <= '2' ? *e - '0' : 0; }
        if (on && now >= next) {
            next = now + 500000000ull;
            float y = guest_float(self + 0x52c);
            const float pi = 3.14159265f;
            float want[4] = {y, remainderf(y + 180.0f, 360.0f), y * pi / 180.0f, remainderf(y + 180.0f, 360.0f) * pi / 180.0f};
            static uint8_t buf[0x1000];
            static uint64_t seen[2048], ptrs[2048];
            static int from1[2048], from2[2048], level[2048];
            uint64_t roots[2] = {self, cam};
            int n_seen = 0, hits = 0;
            for (int r = 0; r < 2 && roots[r]; r++) {
                int n_ptrs = 0;   // from1: the root's offset that held the pointer, from2: the next object's (-1: none)
                from1[n_ptrs] = from2[n_ptrs] = -1, level[n_ptrs] = 0, ptrs[n_ptrs++] = roots[r];
                for (int k = 0; k < n_ptrs && hits < 300; k++) {
                    uint64_t base = ptrs[k];
                    int dup = 0;
                    for (int s2 = 0; s2 < n_seen && !dup; s2++) dup = seen[s2] == base;
                    if (dup || n_seen >= 2048 || !plat_mem_read(base, buf, sizeof buf)) continue;
                    seen[n_seen++] = base;
                    for (int o = 0; o + 4 <= (int)sizeof buf; o += 4) {
                        float f;
                        memcpy(&f, buf + o, 4);
                        for (int w = 0; w < 4; w++)
                            if (fabsf(f - want[w]) < (w < 2 ? 0.05f : 0.001f) && f != 0.0f && hits < 300) {
                                rt_log("CAMSCAN t=%.3f view=%.2f %s[%#x][%#x] obj %#llx +%#x = %.4f (%s)", now / 1e9, y,
                                       r ? "cam" : "self", from1[k] < 0 ? 0 : from1[k], from2[k] < 0 ? 0 : from2[k],
                                       (unsigned long long)base, o, f,
                                       (const char *const[]){"deg", "deg+180", "rad", "rad+180"}[w]);
                                hits++;
                            }
                        if (level[k] < on && !(o & 7) && n_ptrs < 2048) {   // words that point into guest memory
                            uint64_t p;
                            memcpy(&p, buf + o, 8);
                            if (p >= 0x100000000ull && p < (1ull << 39) && !(p & 15)) {
                                from1[n_ptrs] = level[k] == 0 ? o : from1[k];
                                from2[n_ptrs] = level[k] == 0 ? -1 : o;
                                level[n_ptrs] = level[k] + 1;
                                ptrs[n_ptrs++] = p;
                            }
                        }
                    }
                }
            }
        }
    }
    // Once the angle is off, the stick keeps the mouse until it has been close for 30 updates in a row: the swimming
    // view passes near the kept angle at times, and each switch between the two paths was a jerk (player report).
    static int close_run;
    if (!fits) close_run = 0, __atomic_store_n(&oot_stick_mode, 1, __ATOMIC_RELAXED);
    else if (__atomic_load_n(&oot_stick_mode, __ATOMIC_RELAXED) && ++close_run >= 30)
        __atomic_store_n(&oot_stick_mode, 0, __ATOMIC_RELAXED);
    fits = fits && !__atomic_load_n(&oot_stick_mode, __ATOMIC_RELAXED);
    // A camera that does not fit takes the counts as its stick input (oot_inject), unless that is switched off or
    // the camera did not answer to it in this spell.
    if (!fits && accept && direct_mode() && oot_inject(c, self, flags, sx, now)) {
        __atomic_store_n(&accepts_mouse, 1, __ATOMIC_RELAXED);
        __atomic_store_n(&last_update, now, __ATOMIC_RELEASE);
        return;
    }
    if (direct_mode() && accept) {
        static int changes;
        static int was_fit = 1;
        if (fits != was_fit && changes < 6) {
            changes++;
            rt_log(fits ? "mouse: the camera's angle is where it was found again: the mouse turns it directly"
                        : "mouse: the camera's angle is not where it was found (+0xa80 -> +0xdb4): the mouse turns it as a stick");
        }
        was_fit = fits;
    }
    accept = accept && fits;
    __atomic_store_n(&accepts_mouse, accept, __ATOMIC_RELAXED);
    __atomic_store_n(&last_update, now, __ATOMIC_RELEASE);
    if (!direct_mode()) return;
    int32_t dx, dy;
    input_direct_mouse_sample(accept, &dx, &dy);
    plat_mutex_lock(&sample_lock);
    oot_total[0] += dx, oot_total[1] += dy;
    int slot = 0;
    for (int i = 0; i < 8; i++) {
        if (oot_seen[i].object == self) { slot = i; break; }
        if (oot_seen[i].when < oot_seen[slot].when) slot = i;
    }
    if (oot_seen[slot].object != self || now - oot_seen[slot].when > 200000000ull)
        oot_seen[slot].object = self, oot_seen[slot].seen[0] = oot_total[0], oot_seen[slot].seen[1] = oot_total[1];
    int64_t mx = oot_total[0] - oot_seen[slot].seen[0], my = oot_total[1] - oot_seen[slot].seen[1];
    float turned[2] = {oot_seen[slot].turned[0], oot_seen[slot].turned[1]};
    oot_seen[slot].when = now;
    plat_mutex_unlock(&sample_lock);
    float sensitivity = fminf(10, fmaxf(0, (float)cfg_double("REKARINA_MOUSE_SENS")));
    if (!accept || !(sensitivity > 0) || (!mx && !my)) {
        if (!accept || !(sensitivity > 0)) {   // motion the camera does not take now is not kept for later
            plat_mutex_lock(&sample_lock);
            oot_seen[slot].seen[0] = oot_total[0], oot_seen[slot].seen[1] = oot_total[1];
            plat_mutex_unlock(&sample_lock);
        }
        return;
    }
    // Right on the mouse turns the view right: the yaw goes down, as with the stick.
    // at most 5000 counts at once (400 degrees at sensitivity 1): what a long refusal kept cannot fling the camera
    mx = mx > 5000 ? 5000 : mx < -5000 ? -5000 : mx;
    my = my > 5000 ? 5000 : my < -5000 ? -5000 : my;
    float dyaw = -(float)mx * 0.08f * sensitivity * (flags & 2 ? -1 : 1), dpitch = 0;
    guest_float_store(cam + 0xdb4, wrap_degrees(guest_float(cam + 0xdb4) + dyaw));
    // the pitch the camera keeps, +0xd9c (tested with the yaw above): positive looks down, so a mouse moved down (+y)
    // raises it. The game limits it to its own range; a larger value wraps there, so it is held to +-89 first.
    if (my) {
        float was = guest_float(cam + 0xd9c);
        float now_p = fminf(89, fmaxf(-89, was + (float)my * 0.08f * sensitivity * (flags & 4 ? -1 : 1)));
        guest_float_store(cam + 0xd9c, now_p);
        dpitch = now_p - was;
    }
    plat_mutex_lock(&sample_lock);
    oot_seen[slot].seen[0] = oot_total[0], oot_seen[slot].seen[1] = oot_total[1];
    oot_seen[slot].turned[0] = dyaw, oot_seen[slot].turned[1] = dpitch;
    plat_mutex_unlock(&sample_lock);
    // A frame the mouse moves the pitch also hands the camera the smallest vertical stick of the same sign (s0, which
    // the next instruction stores at +0x53c): without stick input the game eases the pitch back to its rest angle,
    // about 0.1 degree a frame, and a stick-moved pitch holds. 0.005 turns it by under 0.003 degree a frame.
    if (my && accept) {
        float y = my > 0 ? -0.005f : 0.005f;   // the stick's +y looks up
        uint32_t yb;
        memcpy(&yb, &y, sizeof yb);
        c->v[0] = yb;
    }
    oot_moved = self;
    static int logged;
    if (!logged) logged = 1, rt_log("mouse: the camera's yaw turned by the mouse's counts (raw, no stick curve)");
}

// The host call at main+0x37c4f4, the update's last store of its flags (`str w8, [x20, #0x530]`): a frame the mouse
// turned this camera counts as camera input for whatever reads them, as a stick past its threshold does (bits 0 and 3
// set, bit 4 cleared). The store is performed; registers stay.
static void oot_camera_flags(Cpu *c) {
    uint32_t w = (uint32_t)c->x[8];
    if (oot_moved && oot_moved == c->x[20]) w = (w | 0x9u) & ~0x10u;
    oot_moved = 0;
    memcpy((void *)(uintptr_t)(c->x[20] + 0x530), &w, sizeof w);
}
#endif

__attribute__((constructor)) static void mouse_direct_register(void) {
#ifdef GAME_OOT
    rt_register_host(HOST_MOUSE_OOT, oot_camera_input);
    rt_register_host(HOST_MOUSE_OOT_FLAGS, oot_camera_flags);
#endif
    rt_register_host(HOST_MOUSE_DIRECT, camera_input);
    rt_register_host(HOST_MOUSE_ACTIVITY, camera_activity);
    rt_register_host(HOST_MOUSE_ORBIT_BEGIN, orbit_begin);
    rt_register_host(HOST_MOUSE_ORBIT_ANGLES, orbit_angles);
    rt_register_host(HOST_MOUSE_HAND, hand_input);
}
#endif
