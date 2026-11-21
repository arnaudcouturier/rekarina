// OoT: the host halves of the code patch sets that keep the game's 60-frames-a-second logic right above 60 fps (the
// sets from daytime on in recomp/patches-oot-v111.txt, which say what the game does at each place). The set realtime
// feeds the game its measured step in 60 Hz frames, under 1 above 60 fps; these places cut or test it as if it were a
// whole frame. Each function below is reached by a bl on the patch page at its site (the lifted bl spills the
// registers and reloads x0-x18 and x30 after it): it performs what the site's own instruction did, changes only the
// registers its comment names, and gives the game's own values back while the frame's step is one frame or more (60
// fps and below) or not a number. x19-x29 and sp are only read here: the lifted caller keeps them in locals it does not
// reload.
//
// The log names each place the first time it runs, and every 20 s what some of them changed.
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "platform/platform.h"

#if __has_include("patches.h")
#include "layout.h"
#include "patches.h"
#endif

#if defined(GAME_OOT) && defined(RT_PATCH_PAGE)

#define HOST_DAY_CLOCK (RT_PATCH_PAGE + 0xf60)
#define HOST_SOUND_GATE (RT_PATCH_PAGE + 0xf50)
#define HOST_SOUND_STEP (RT_PATCH_PAGE + 0xf40)
#define HOST_MUSIC_TIME (RT_PATCH_PAGE + 0xf30)
#define HOST_MUSIC_FLAGS (RT_PATCH_PAGE + 0xf20)
#define HOST_SONG_CUE0 (RT_PATCH_PAGE + 0xf10)
#define HOST_SONG_CUE1 (RT_PATCH_PAGE + 0xf00)
#define HOST_HORSE_JUMP (RT_PATCH_PAGE + 0xef0)
#define HOST_FLOOR_CARRY (RT_PATCH_PAGE + 0xee0)
#define HOST_SONG_PLAY (RT_PATCH_PAGE + 0xed0)
#define HOST_SONG_RECORD (RT_PATCH_PAGE + 0xec0)
#define HOST_SONG_DONE (RT_PATCH_PAGE + 0xeb0)
#define HOST_LOOP_STEP0 (RT_PATCH_PAGE + 0xea0)
#define HOST_LOOP_STEP1 (RT_PATCH_PAGE + 0xe90)
#define HOST_LOOP_POSE (RT_PATCH_PAGE + 0xe80)
#define HOST_CLOTH_TIME (RT_PATCH_PAGE + 0xe70)
#define HOST_WEB_STEPS (RT_PATCH_PAGE + 0xe60)
#define HOST_INPUT_STEP (RT_PATCH_PAGE + 0xe50)

#define VFR_SLOT (MOD_MAIN_BASE + 0x4c7e890)   // -> the VFR manager -> its time object, +0x10 the step in 60 Hz frames

static float bits_float(uint32_t b) { float f; memcpy(&f, &b, 4); return f; }
static uint32_t float_bits(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }
static uint64_t g64(uint64_t at) { uint64_t v; memcpy(&v, (const void *)(uintptr_t)at, 8); return v; }
static uint32_t g32(uint64_t at) { uint32_t v; memcpy(&v, (const void *)(uintptr_t)at, 4); return v; }
static float gf(uint64_t at) { return bits_float(g32(at)); }
static float vget(const Cpu *c, int n) { return bits_float((uint32_t)c->v[n]); }
static void vset(Cpu *c, int n, float f) { c->v[n] = float_bits(f); }   // a scalar write clears the rest, as on the guest

// The step the game takes this frame, in 60 Hz frames (NaN before the VFR manager exists).
static float frame_step(void) {
    uint64_t manager = g64(VFR_SLOT), time = manager ? g64(manager) : 0;
    return time ? gf(time + 0x10) : NAN;
}

// A frame shorter than a 60 Hz one: the only case the places below change.
static int short_frame(float step) { return step > 0.0f && step < 1.0f; }

// Every 20 s of wall time (one caller at a time gets it).
static int log_due(uint64_t *next) {
    uint64_t now = plat_now_ns(), due = __atomic_load_n(next, __ATOMIC_RELAXED);
    if (now < due) return 0;
    return __atomic_compare_exchange_n(next, &due, now + 20000000000ull, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
}

static void first_run(int *seen, const char *what) {
    if (!__atomic_exchange_n(seen, 1, __ATOMIC_RELAXED)) rt_log("oot fps: %s", what);
}

static void count(long long *n, long long add) { __atomic_add_fetch(n, add, __ATOMIC_RELAXED); }
static long long take(long long *n) { return __atomic_exchange_n(n, 0, __ATOMIC_RELAXED); }

// ---- whole frames: counters that add the step (or a multiple of it) cut to whole frames

// The fraction a counter has not taken yet, per counter (its address or its object's). 64 places, the one used longest
// ago reused: a counter that loses its place starts again from 0, which costs it less than a frame once.
#define CARRY_SLOTS 64
static struct { uint64_t at; float part; uint64_t used; } carry[CARRY_SLOTS];
static uint64_t carry_uses;
static PlatMutex carry_lock = PLAT_MUTEX_INIT;

// The whole frames of amount for the counter at `at`, the fraction kept for its next call. Not a positive number:
// as it is (the game's cut decides).
static float carry_whole(uint64_t at, float amount) {
    if (!(amount > 0.0f) || amount >= 1e9f) return amount;
    plat_mutex_lock(&carry_lock);
    int slot = -1, oldest = 0;
    for (int i = 0; i < CARRY_SLOTS; i++) {
        if (carry[i].at == at) { slot = i; break; }
        if (carry[i].used < carry[oldest].used) oldest = i;
    }
    if (slot < 0) slot = oldest, carry[slot].at = at, carry[slot].part = 0.0f;
    carry[slot].used = ++carry_uses;
    float sum = carry[slot].part + amount, whole = floorf(sum);
    carry[slot].part = sum - whole;
    plat_mutex_unlock(&carry_lock);
    return whole;
}

// set daytime, main+0x3cdfbc after fadd s0, s0, s0: s0 = 2 * step game seconds, which the clock object (x19) cuts and
// stores at +0x3c. Writes s0: its whole seconds, the fraction kept per clock. The log gives the seconds added per
// second of wall time (120 at any frame rate is the game's own speed: 2 a 60 Hz frame) and what the cut would have.
// REKARINA_DAYTIME=HH:MM, a test knob: the clock is held at that time of day, so a run (a warp included) plays at night
// without a save made at night. The clock (x19) keeps the time of day at +0x28 in seconds since midnight (0..86399;
// further on, this update adds its step there and carries whole days into +0x2c), and the next update holds it again
// after anything set it. That is enough where the time runs (the field). A town's time does not run: its load takes
// 06:00, 20:00 or 00:00 from two game-data bools (main+0x345c450: 0x804aec99 clear -> 06:00; set, with 0x8592aa82 set
// -> 20:00, clear -> 00:00; the times are the table at main+0x3c834e8). So the knob sets them too, each update, to the side of 06:00-18:00 the held time is on (a time from 18:00 on:
// 20:00, before 06:00: 00:00), and a warp into a town at night arrives at night.
static struct { int seen; uint64_t next, last_ns; long long calls, game, cut, wall_ns; int hold, flags_said; } day = {.hold = -2};

// A game-data bool by its name's hash, written in place (the manager main+0x4c7e180 -> holder -> manager; its hash
// table +0x20 of {hash, index} pairs, +0x2c slots, probed from hash % slots on; the values +0x18, 0x18 bytes each,
// +0x10 of them, the bool at +0x16), as main+0x345c450 reads it. 1 written, 0 not there.
static int game_bool_set(uint32_t hash, int value) {
    uint64_t holder = 0, mgr = 0, keys = 0, vals = 0;
    if (!plat_mem_read(MOD_MAIN_BASE + 0x4c7e180, &holder, 8) || !holder || !plat_mem_read(holder, &mgr, 8) || !mgr ||
        !plat_mem_read(mgr + 0x20, &keys, 8) || !keys || !plat_mem_read(mgr + 0x18, &vals, 8) || !vals)
        return 0;
    uint32_t slots = 0, count = 0;
    if (!plat_mem_read(mgr + 0x2c, &slots, 4) || !slots || !plat_mem_read(mgr + 0x10, &count, 4)) return 0;
    uint32_t start = hash % slots, i = start;
    do {
        uint32_t kv[2];
        if (!plat_mem_read(keys + (uint64_t)i * 8, kv, 8) || !kv[0]) return 0;
        if (kv[0] == hash) {
            uint64_t at = vals + (uint64_t)(kv[1] < count ? kv[1] : 0) * 0x18 + 0x16;
            uint8_t v = (uint8_t)(value != 0);
            if (!plat_mem_readable(at, 1)) return 0;
            memcpy((void *)(uintptr_t)at, &v, 1);
            return 1;
        }
        i = i + 1 >= slots ? 0 : i + 1;
    } while (i != start);
    return 0;
}

static void day_clock(Cpu *c) {
    first_run(&day.seen, "the day's clock keeps the fraction of its seconds (set daytime)");
    if (day.hold == -2) {
        const char *e = cfg_get("REKARINA_DAYTIME");
        int h = -1, m = 0;
        day.hold = e && sscanf(e, "%d:%d", &h, &m) >= 1 && h >= 0 && h < 24 && m >= 0 && m < 60 ? h * 3600 + m * 60 : -1;
        if (day.hold >= 0) rt_log("oot fps: the day's clock is held at %02d:%02d (REKARINA_DAYTIME)", h, m);
    }
    if (day.hold >= 0) {
        uint32_t t = (uint32_t)day.hold;
        memcpy((void *)(uintptr_t)(c->x[19] + 0x28), &t, 4);
        int night = day.hold >= 18 * 3600 || day.hold < 6 * 3600, evening = day.hold >= 18 * 3600;
        int a = game_bool_set(0x804aec99u, night), b = game_bool_set(0x8592aa82u, evening);
        if (day.flags_said != 1 + a + 2 * b) {
            day.flags_said = 1 + a + 2 * b;
            rt_log("oot fps: the towns' time of day: game-data bools %s (0x804aec99 %d: %s, 0x8592aa82 %d: %s)",
                   a && b ? "set" : "not all found", night, a ? "written" : "not there", evening, b ? "written" : "not there");
        }
    }
    float amount = vget(c, 0), step = frame_step(), out = amount;
    if (short_frame(step)) out = carry_whole(c->x[19] + 0x3c, amount), vset(c, 0, out);
    uint64_t now = plat_now_ns(), last = __atomic_exchange_n(&day.last_ns, now, __ATOMIC_RELAXED);
    if (last && now > last && now - last < 1000000000ull) count(&day.wall_ns, (long long)(now - last));
    count(&day.calls, 1);
    count(&day.game, out > 0 ? (long long)out : 0), count(&day.cut, amount > 0 ? (long long)amount : 0);
    if (log_due(&day.next)) {
        long long calls = take(&day.calls), game = take(&day.game), cut = take(&day.cut), wall = take(&day.wall_ns);
        if (wall > 0)
            rt_log("oot fps: the day's clock added %lld game seconds in %.1f s, %.1f a second (the game's own cut: %.1f a "
                   "second); %lld updates, frame step %.3f", game, wall / 1e9, game * 1e9 / wall, cut * 1e9 / wall, calls,
                   step);
    }
}

// ---- set soundjob: the sound manager's job at 60 Hz, the music manager's whole step, a song's notes once each

static PlatMutex sound_lock = PLAT_MUTEX_INIT;   // the job's phases and the music update may run on several threads
static struct {
    float part;            // the steps added since the job last ran, under 1
    int run;               // phase 0's choice, which phases 1 and 2 follow
    long long runs;        // phase 0's runs: the song playbacks' note flag
    uint64_t flags_at;     // the music manager's change flags (guest address), from its last read
    uint8_t held, given;   // the flags seen on frames the gate held the job; those handed to the update's current run
    struct { int set; long long runs; } cue[2];   // each playback's last read of its note flag
    long long frames, ran, cues, cleared;         // for the log
    int seen[5];
    uint64_t next;
} snd = {.run = 1};

// main+0x41531c, the job's head after its prologue: w2 the phase. Writes w2 = 3 (a phase the job does not have: it
// returns through its epilogue at main+0x4155fc) on a frame the job does not run.
static void sound_gate(Cpu *c) {
    first_run(&snd.seen[0], "the sound job runs as often as at 60 fps (set soundjob)");
    uint32_t phase = (uint32_t)c->x[2];
    float step = phase == 0 ? frame_step() : 0.0f;
    long long frames = 0, ran = 0, cues = 0, cleared = 0;
    plat_mutex_lock(&sound_lock);
    int run = snd.run;
    if (phase == 0) {
        if (!(step > 0.0f)) snd.part = 0.0f, run = 1;   // 0, below 0 or not a number: as the game has it
        else if (step >= 1.0f) run = 1;                // a whole step or more: runs; the part kept stays for later
        else {
            snd.part += step;
            run = snd.part >= 1.0f;
            if (run) snd.part -= 1.0f;
        }
        snd.run = run;
        snd.runs += run;
        snd.frames++, snd.ran += run;
    }
    if (!run && snd.flags_at) snd.held |= *(const volatile uint8_t *)(uintptr_t)snd.flags_at;   // what the update would read now
    int due = phase == 0 && log_due(&snd.next);
    if (due) {
        frames = snd.frames, ran = snd.ran, cues = snd.cues, cleared = snd.cleared;
        snd.frames = snd.ran = snd.cues = snd.cleared = 0;
    }
    plat_mutex_unlock(&sound_lock);
    if (!run) c->x[2] = 3;
    if (due)
        rt_log("oot fps: the sound job ran %lld of %lld frames (step %.3f); a song's note flag read again before the "
               "job ran: %lld of %lld reads", ran, frames, step, cleared, cues);
}

// main+0x415334 after ldr s0, [x8, #0x10] (the step phase 0 hands to the sound engine): writes s0 = 1 while the step
// is under 1 (the gate let the job run because a whole step has built up).
static void sound_step(Cpu *c) {
    first_run(&snd.seen[1], "the sound engine is handed whole steps (set soundjob)");
    if (short_frame(vget(c, 0))) vset(c, 0, 1.0f);
}

// main+0x3353448 after ldr s9, [x8, #0x18], the music manager's update (s8 the step, s9 the frame's seconds, from the
// time object): writes s8 = 1 and s9 = s9 / s8 while the step is under 1, a whole step and its seconds (the base, 60 or
// the half-rate mode's 30, stays in them). The change flags kept since its last run become this run's.
static void music_time(Cpu *c) {
    first_run(&snd.seen[2], "the music manager counts whole steps (set soundjob)");
    float step = vget(c, 8);
    if (short_frame(step)) vset(c, 9, vget(c, 9) / step), vset(c, 8, 1.0f);
    plat_mutex_lock(&sound_lock);
    snd.given = snd.held;
    snd.held = 0;
    plat_mutex_unlock(&sound_lock);
}

// The music update's four reads of the battle manager's change flags, in place of ldrb w8, [x26, #0x898]: writes w8,
// the byte with the flags of the frames the gate held the job.
static void music_flags(Cpu *c) {
    uint64_t at = c->x[26] + 0x898;
    uint8_t v = *(const volatile uint8_t *)(uintptr_t)at;
    plat_mutex_lock(&sound_lock);
    snd.flags_at = at;
    v |= snd.given;
    plat_mutex_unlock(&sound_lock);
    c->x[8] = v;
}

void f_7003297b50(Cpu *c);   // lifted: whether a playback's note flag is set (bit 0 of w0)

// In place of the playbacks' bl main+0x3297b50 (x30 is the call's return address, as the bl gave it): calls it, then
// writes w0 with bit 0 cleared when the flag was read set before and the job has not run since (the same note).
static void song_cue(Cpu *c, int site) {
    f_7003297b50(c);
    int set = (int)(c->x[0] & 1), timed = short_frame(frame_step());
    plat_mutex_lock(&sound_lock);
    int again = timed && set && snd.cue[site].set && snd.cue[site].runs == snd.runs;
    snd.cue[site].set = set, snd.cue[site].runs = snd.runs;
    snd.cues++, snd.cleared += again;
    plat_mutex_unlock(&sound_lock);
    if (again) c->x[0] &= ~(uint64_t)1;
}
static void song_cue0(Cpu *c) {
    first_run(&snd.seen[3], "a played song's note flag is read once a note (set soundjob)");
    song_cue(c, 0);
}
static void song_cue1(Cpu *c) {
    first_run(&snd.seen[4], "a melody's note flag is read once a note (set soundjob)");
    song_cue(c, 1);
}

// ---- set horsejump

// main+0x2c3ae18 in place of fmov s9, #1.0, before fcmp s10, s9; b.pl (past the branch for a step under 1, s10 the
// state machine's step). Writes s9: s10 itself when it is the frame's whole step (the test then skips the branch),
// else 1.0 as the game.
static struct { int seen; uint64_t next; long long calls, whole; } horse;
static void horse_jump(Cpu *c) {
    first_run(&horse.seen, "the horse's jump takes the frame's whole step as no slow motion (set horsejump)");
    float step = vget(c, 10), frame = frame_step();
    int whole = short_frame(frame) && step > 0.0f && step >= frame * 0.999f;
    vset(c, 9, whole ? step : 1.0f);
    count(&horse.calls, 1), count(&horse.whole, whole);
    if (log_due(&horse.next))
        rt_log("oot fps: the horse's jump blend: %lld of %lld tests took the whole-frame step (step %.3f, the frame's %.3f)",
               take(&horse.whole), take(&horse.calls), step, frame);
}

// ---- set floorcarry

// After fmov s1, w8 in the three modules' floor carry test (s1 = 100.0, which 1/dt in s0 is compared with): writes
// s1 = 100 / step while the step is under 1.
static struct { int seen; uint64_t next; long long calls, full; } floor_carry_n;
static void floor_carry(Cpu *c) {
    first_run(&floor_carry_n.seen, "a moving floor's carry tests 1/dt as at 60 fps (set floorcarry)");
    float step = frame_step();
    if (!short_frame(step)) return;
    float limit = vget(c, 1), inv_dt = vget(c, 0);
    vset(c, 1, limit / step);
    count(&floor_carry_n.calls, 1), count(&floor_carry_n.full, inv_dt > limit && !(inv_dt > limit / step));
    if (log_due(&floor_carry_n.next))
        rt_log("oot fps: a moving floor's carry: %lld of %lld tests carry in full where the game's took a tenth (1/dt "
               "%.1f, step %.3f)", take(&floor_carry_n.full), take(&floor_carry_n.calls), inv_dt, step);
}

// ---- set songrec: the recorded song's elapsed frames (+0x265fc of its object)

// s0 of a counter's site, the amount the game cuts next: its whole frames while the frame is short.
static void whole_counter(Cpu *c, uint64_t at) {
    if (short_frame(frame_step())) vset(c, 0, carry_whole(at, vget(c, 0)));
}

static int song_seen[3];
// main+0x329340c after ldr s0, [x8, #0x10], the playback: the counter at x19 + x20. Writes s0.
static void song_play(Cpu *c) {
    first_run(&song_seen[0], "a recorded song's playback counts whole frames (set songrec)");
    whole_counter(c, c->x[19] + c->x[20]);
}
// main+0x3296b04 and +0x3296da0 after ldr s0, [x8, #0x10], the recording (a leaf: x16 holds its x30): the counter at
// x9. Writes s0, and x30 back from x16.
static void song_record(Cpu *c) {
    c->x[30] = c->x[16];
    first_run(&song_seen[1], "a song's recording counts whole frames (set songrec)");
    whole_counter(c, c->x[9]);
}
// main+0x32926c4 after ldr s0, [x8, #0x10], the recording's end: the counter at x20 + 0x5fc. Writes s0.
static void song_done(Cpu *c) {
    first_run(&song_seen[2], "a song's recording times its last note in whole frames (set songrec)");
    whole_counter(c, c->x[20] + 0x5fc);
}

// ---- set looppose

// The fraction of the step a loop of one frame or less has not taken, per animation entry: 4096 places by the entry's
// address, a short probe; an entry that finds none starts again from 0 (its pose stands still, as it should).
#define LOOP_SLOTS 4096
#define LOOP_PROBE 32
static struct { uint64_t at; float part; } loop_carry[LOOP_SLOTS];
static PlatMutex loop_lock = PLAT_MUTEX_INIT;
static struct { int seen[2]; uint64_t next; long long steps, poses; } loops;

static int loop_is_pose(float start, float end) { return end > start && end - start <= 1.0f; }

// A step that is not a whole number of 60 Hz frames: such a loop lands on another phase each frame above 60 fps (a step
// under 1) and between 30 and 60 fps alike (1.25 at 48 fps: the frozen figures under the ice changed pose at the views
// that ran under 60 fps, 2026-11-21). Whole steps (60 and 30 fps) keep the game's own values.
static int loop_fraction(float step) { return step > 0.0f && fabsf(step - roundf(step)) > 1e-3f; }

static float loop_whole(uint64_t entry, float step) {
    uint32_t h = (uint32_t)((entry * 0x9E3779B97F4A7C15ull) >> 40) % LOOP_SLOTS, k = 0;
    plat_mutex_lock(&loop_lock);
    while (k < LOOP_PROBE && loop_carry[(h + k) % LOOP_SLOTS].at && loop_carry[(h + k) % LOOP_SLOTS].at != entry) k++;
    uint32_t i = (h + (k < LOOP_PROBE ? k : 0)) % LOOP_SLOTS;
    if (loop_carry[i].at != entry) loop_carry[i].at = entry, loop_carry[i].part = 0.0f;
    float sum = loop_carry[i].part + step, whole = floorf(sum);
    loop_carry[i].part = sum - whole;
    plat_mutex_unlock(&loop_lock);
    return whole;
}

// The animation clock's step times speed (s8 the step; the entry in x20: start +0x10, end +0x14): with the step's whole
// frames for a loop of one frame or less while the step has a fraction.
static float loop_product(Cpu *c, float speed) {
    float step = vget(c, 8);
    uint64_t entry = c->x[20];
    if (loop_fraction(frame_step()) && step > 0.0f && loop_is_pose(gf(entry + 0x10), gf(entry + 0x14))) {
        step = loop_whole(entry, step);
        count(&loops.steps, 1);
    }
    return step * speed;
}
// main+0x2afe04 in place of fmul s2, s8, s1. Writes s2.
static void loop_step0(Cpu *c) {
    first_run(&loops.seen[0], "a loop of one frame steps by whole frames (set looppose)");
    vset(c, 2, loop_product(c, vget(c, 1)));
}
// main+0x2b01d0 in place of fmul s1, s8, s0. Writes s1.
static void loop_step1(Cpu *c) {
    first_run(&loops.seen[0], "a loop of one frame steps by whole frames (set looppose)");
    vset(c, 1, loop_product(c, vget(c, 0)));
}
// main+0x924e50 in place of str s0, [x22] (s0 the time a skeleton is drawn at, its range at x22 + 4 and + 8): performs
// the store, of the range's start for a loop of one frame or less while the step has a fraction.
static void loop_pose(Cpu *c) {
    first_run(&loops.seen[1], "a loop of one frame is drawn at its start (set looppose)");
    uint64_t at = c->x[22];
    uint32_t time = (uint32_t)c->v[0];
    if (loop_fraction(frame_step()) && loop_is_pose(gf(at + 4), gf(at + 8))) time = g32(at + 4), count(&loops.poses, 1);
    memcpy((void *)(uintptr_t)at, &time, 4);
    if (log_due(&loops.next))
        rt_log("oot fps: loops of one frame: %lld steps made whole, %lld poses drawn at their start in 20 s",
               take(&loops.steps), take(&loops.poses));
}

// ---- set clothtime

// main+0x1b1ac68 after ldr d0, [x22, #8] (its low float the time of a cloth step, a 60th): writes that float times the
// step while the step is under 1 (the high half as loaded).
static struct { int seen; uint64_t next; long long calls; } cloth;
static void cloth_time(Cpu *c) {
    first_run(&cloth.seen, "cloth steps by the frame's time (set clothtime)");
    float step = frame_step();
    if (!short_frame(step)) return;
    uint64_t d = (uint64_t)c->v[0];
    float time = bits_float((uint32_t)d);
    c->v[0] = (d & 0xffffffff00000000ull) | float_bits(time * step);
    count(&cloth.calls, 1);
    if (log_due(&cloth.next))
        rt_log("oot fps: cloth: %lld setups in 20 s took %.3f ms in place of %.3f ms (step %.3f)", take(&cloth.calls),
               time * step * 1000.0f, time * 1000.0f, step);
}

// ---- set webstep

// main+0x632cfc after fdiv s0, s8, s0 (the frame's time in 60ths, which the web cuts to its simulation steps; the web
// in x19): writes s0, its whole steps, the fraction kept per web.
static int web_seen;
static void web_steps(Cpu *c) {
    first_run(&web_seen, "the floor web keeps the fraction of its steps (set webstep)");
    whole_counter(c, c->x[19] + 0x8f8);
}

// ---- set inputstep

// main+0x3b4eaf8 after fdiv s0, s1, s0 (60 / the nominal frame rate, the global step stored at main+0x4d41fc8): writes
// s0 = the real-time step (patch page +8, in 60 Hz frames, runtime/frame_rate.c) while that is under 1.
static int input_seen;
static void input_step(Cpu *c) {
    first_run(&input_seen, "the global step follows the frame's time (set inputstep)");
    float step = gf(RT_PATCH_PAGE + 8);
    if (short_frame(step)) vset(c, 0, step);
}

// ---- set dofscale: the depth of field at half the scene that is drawn
// main+0x260270 makes an effect's render target from its descriptor (x15): its size is a reference texture's size
// ([[x20+0x18]+0x20] +0x48 / +0x4a, 16 bits each) times the descriptor's floats, width * +0x53c * +0x538 and
// height * +0x540 * +0x538 (main+0x260398..0x2603b8, into w27 and w25). The depth of field's descriptor holds 0.75,
// 0.75 and 0.5, and its reference is the game's picture (2560x1440 at 1440p) whatever the scene: 0.75 brings that to
// the console's 1080p scene and 0.5 halves it, so the blur is drawn at 960x540 and stretched over the whole picture
// the fixed scene draws (pixelated behind a shop's items at 1440p and above). The size's one reader is the target
// request built on the stack (stp w27, w25, [sp, #0x50] at main+0x2603c4; both registers are loaded with other values
// before any other use). At main+0x2603c8, after that store, a descriptor with 0.75 on both sides gets its own scale
// (+0x538) of the scene the fixed path draws instead (the picture times frame_rate.c res_fixed_scene_share): 1280x720
// for the whole 2560x1440 picture. Never less than the game's own size (a DLSS mode's smaller scene keeps 960x540),
// and nothing changes while the game's own dynamic resolution runs (share 0) or for any other descriptor. The host
// writes guest memory only (it must not write x19-x29). DEBUG-DOFLOG: REKARINA_DOF_LOG=1 logs the first 32 sizes.
#define HOST_DOF_SCALE (RT_PATCH_PAGE + 0xd20)
double res_fixed_scene_share(void);   // frame_rate.c
static void dof_scale(Cpu *c) {
    static int said = -1;
    if (said < 0) { const char *e = cfg_get("REKARINA_DOF_LOG"); said = e && *e == '1' ? 0 : 32; }
    uint64_t p = c->x[15], d = c->x[20], at = c->sp + 0x50, ref = 0;
    if (p < 0x10000 || p >= RT_ADDRESS_LIMIT || (p & 3)) return;
    if (d >= 0x10000 && d < RT_ADDRESS_LIMIT && !(d & 7)) {
        uint64_t a = g64(d + 0x18);
        if (a >= 0x10000 && a < RT_ADDRESS_LIMIT && !(a & 7)) ref = g64(a + 0x20);
    }
    if (ref < 0x10000 || ref >= RT_ADDRESS_LIMIT) return;
    float s = gf(p + 0x538), sx = gf(p + 0x53c), sy = gf(p + 0x540);
    uint32_t rw = g32(ref + 0x48) & 0xffff, rh = g32(ref + 0x48) >> 16;
    uint32_t w = g32(at), h = g32(at + 4), nw = w, nh = h;
    double share = res_fixed_scene_share();
    if (sx == 0.75f && sy == 0.75f && s > 0.0f && s <= 1.0f && share > 0.0) {
        uint32_t sw = (uint32_t)(rw * share * s), sh = (uint32_t)(rh * share * s);
        if (sw > w && sh > h && sw <= 8192 && sh <= 8192) {
            nw = sw, nh = sh;
            memcpy((void *)(uintptr_t)at, &nw, 4);
            memcpy((void *)(uintptr_t)(at + 4), &nh, 4);
        }
    }
    if (said >= 32) return;
    said++;
    rt_log("DOFLOG target %ux%u -> %ux%u levels %u, reference %ux%u, scene share %.3f, descriptor %#llx: %g %g %g", w, h,
           nw, nh, (unsigned)c->x[26], rw, rh, share, (unsigned long long)p, s, sx, sy);
}

// ---- set promptglyphs: texts laid out before the button prompts changed in the running game
// main+0xe4e100, after the prologue of a text's draw (main+0xe4e0e0, x0 the text; called for every text drawn, each
// frame): the text's glyph buffer is at +0x10 (its characters' count at +0xac; their records in pages of 1024 at
// +0x30, each record 0xa0 bytes with the character at +8 and the glyph the shaping gave at +0x94, or 0x48 bytes with
// them at +0 and +0x3c when the byte at +0xd5 is set: main+0x1512b80, main+0x15146a0), and the draw builds the text's
// quads again from those records, each glyph through its font's plot (main+0x1515dd4), when the text's flags (+0x180)
// hold bit 7 (main+0xe4e298; the draw clears it). The font's glyph cache (main+0x1540640) keys a picture by font,
// character, glyph and size. runtime/romfs.c live_main swaps the button font's table in the running game, which
// reaches a text only when the game shapes it again: a text set once kept the old set's glyphs, and a text drawn by
// character (glyph 0: the action hint's key, the pause screen's guide) the picture its character had when it was
// first drawn. So once per text after each change (romfs.c romfs_text_generation), and whenever the game is about to
// build its quads anew, the host gives each button character holding another set's glyph the glyph of the set in use
// (romfs.c romfs_prompt_glyph), marks the size of each one drawn by character (glyph 0) so its cache key is new, and,
// where it changed a record, sets bit 7, so this very draw builds the quads again with the new buttons (and again
// later while the build skipped the glyph: glyph_pending_draw). The texts looked at for this change are kept in a
// table by address (a text freed and another made at its address is looked at once more only after the table fills,
// which costs little: a text laid out after the change holds the new glyphs). The host writes guest memory only.
#define HOST_PROMPT_GLYPHS (RT_PATCH_PAGE + 0xd30)
uint32_t romfs_text_generation(void);                     // romfs.c
uint32_t romfs_prompt_glyph(uint32_t cp, uint32_t glyph);   // romfs.c
enum { GLYPH_TEXTS = 8192 };
static uint64_t glyph_texts[GLYPH_TEXTS];
static uint32_t glyph_gen, glyph_used, glyph_looked, glyph_fixed_texts, glyph_fixed, glyph_said;
static uint64_t glyph_since;
static PlatMutex glyph_lock = PLAT_MUTEX_INIT;

// A text whose quads are built while it is hidden (the action hint between its showings) skips each glyph outside its
// clip (main+0x1515ce8): the glyph is not plotted, its quad keeps the old glyph's place in the cache, and the text
// showed no button, or the old one, when it appeared (2026-11-20, runs that swapped the hint's key in a loading
// screen). So a text with swapped glyphs stays pending until the draw record of its first swapped glyph (text+0x38:
// pages of 1024 records of 0xe0 bytes; its quad's texture coordinates at +0x10..+0x1f, written by the build
// main+0x1515eb0 when the glyph is plotted) holds other texture coordinates: until then every fourth draw asks for
// the build again. A change of set starts the list anew.
enum { GLYPH_PENDING = 32 };
static struct { uint64_t text, drec; uint8_t stale[64]; uint32_t draws; } glyph_pend[GLYPH_PENDING];
static int glyph_npend;   // under glyph_lock; read without it as a hint
// texts with a large glyph waiting for their sheet (sheet_pending_draw, below)
enum { SHEET_PENDING = 16 };
static struct { uint64_t text, hash; uint32_t draws, builds, idx, watch; } sheet_pend[SHEET_PENDING];   // idx: the target sheet
static int sheet_npend;   // under glyph_lock; read without it as a hint

// A glyph the font's cache has no picture for (a glyph id this pass gave, or a size it marked: both are new cache keys)
// is drawn by the build into the text's sheet in use (main+0x1515eb0 -> main+0x1519300): the CPU rasterises its rows
// through the sheet's mapped address at sheet+0xf0. Only the full build sets that address (main+0x150f4d0:
// nvnMemoryPoolMap of the sheet's pool + its offset) and its end clears it again (main+0x150f520); the build through
// bit 7 (main+0x1516120) never maps a sheet, so a miss there wrote the glyph's rows at address 0 + their place in the
// sheet whenever the sheet still had room (2026-11-20: the fault at main+0x1519424, a size-90 text built through
// bit 7 right after its size was marked; with no room the glyph is skipped, which is why it was rare). The game never
// misses in that build: the full build that laid the text out put its glyphs in the cache. So before a build through
// bit 7 of a text whose records this pass changed (asked by this pass or by the game), every set-up sheet of its ring
// gets the mapped address the full build would give it, and the texture cache is told the sheets were written, once
// before the build and once at the text's next draw (the build's CPU writes are not seen otherwise). A text whose
// sheet cannot be mapped is not built by this pass. The texts changed since the last change are kept by address.
enum { GLYPH_MARKED = 1024, SHEET_STAMPS = 32 };
static uint64_t glyph_marked[GLYPH_MARKED];   // under glyph_lock
// sheet_prepared: builds prepared since the change; sheet_drawn: of those, builds that drew a glyph into a sheet mapped
// for them (each would have written at address 0 before)
static uint32_t glyph_nmarked, sheet_unmappable, sheet_prepared, sheet_drawn;
static uint32_t sheet_noring, sheet_unset;   // builds of texts without a ring / with no sheet set up (nothing to map)
static struct { uint64_t text, h[4]; uint32_t idx, draws; } sheet_stamp[SHEET_STAMPS];   // under glyph_lock; h: sheets' texels
static uint64_t sheet_hash(uint64_t cpu, uint64_t n);
static int sheet_nstamp;   // under glyph_lock; read without it as a hint
void vk_guest_written(uint64_t addr, uint64_t size);

// glyph_lock held: is text in the table (add: put it there)?
static int glyph_marked_at(uint64_t text, int add) {
    if (add && glyph_nmarked >= GLYPH_MARKED / 4 * 3) memset(glyph_marked, 0, sizeof glyph_marked), glyph_nmarked = 0;
    for (uint32_t h = (uint32_t)((text >> 4) * 0x9E3779B1u) & (GLYPH_MARKED - 1);; h = (h + 1) & (GLYPH_MARKED - 1)) {
        if (glyph_marked[h] == text) return 1;
        if (!glyph_marked[h]) {
            if (add) glyph_marked[h] = text, glyph_nmarked++;
            return 0;
        }
    }
}

// The CPU address of a sheet's texels as the full build maps it (0: not set up, or its pool has no CPU view), and their
// size.
static uint64_t sheet_cpu(uint64_t s, uint64_t *size) {
    if (!*(const uint8_t *)(uintptr_t)(s + 8)) return 0;
    // +0xe0 holds the game's pool holder, whose first word is the NVNmemoryPool the sheet begin maps
    // (main+0x150f4f0 -> main+0x129e240: ldr x0, [x0]; nvnMemoryPoolMap)
    uint64_t holder = g64(s + 0xe0), pool = holder && !(holder & 7) && holder < RT_ADDRESS_LIMIT ? g64(holder) : 0;
    if (!pool || (pool & 7) || pool >= RT_ADDRESS_LIMIT) return 0;
    // the runtime's pool object (runtime/nvn_obj.h pool_t: magic 'POOL', flags, mem); nvnMemoryPoolMap gives mem, or
    // nothing for a pool without CPU access (flag 1)
    if (g32(pool) != 0x4c4f4f50u || (g32(pool + 4) & 1)) return 0;
    uint64_t mem = g64(pool + 8);
    uint32_t w = g32(s + 0x140), h = g32(s + 0x144);
    if (!mem || !w || !h || (uint64_t)w * h > (16u << 20)) return 0;
    *size = (uint64_t)w * h;
    return mem + g64(s + 0xe8);
}

// Before a build through bit 7 of text: maps every set-up sheet of its ring that is not mapped and tells the texture
// cache they are written. 1: the build is safe (no ring: a miss is skipped there, main+0x1516048; or every set-up
// sheet mapped), 0: a set-up sheet cannot be mapped.
static int sheet_ready(uint64_t text) {
    uint64_t sheets = g64(text + 0x68);
    uint32_t n = g32(text + 0xa4);
    if (!sheets) {
        __atomic_add_fetch(&sheet_noring, 1, __ATOMIC_RELAXED);
        return 1;
    }
    if ((sheets & 7) || sheets >= RT_ADDRESS_LIMIT || !n || n > 16) return 0;
    int any = 0;
    for (uint32_t k = 0; k < n; k++) {
        uint64_t s = sheets + 0x158ull * k, size = 0, cpu;
        if (!*(const uint8_t *)(uintptr_t)(s + 8)) continue;   // not set up: the build through bit 7 skips the text
        if (!(cpu = sheet_cpu(s, &size))) {
            plat_mutex_lock(&glyph_lock);
            int say = sheet_unmappable++ < 8;
            plat_mutex_unlock(&glyph_lock);
            if (say)
                rt_log("prompt glyphs: text %#llx: sheet %u of its ring has no CPU view; the text is not built again",
                       (unsigned long long)text, k);
            return 0;
        }
        if (g64(s + 0xf0) != cpu) {
            static uint32_t said;
            if (__atomic_fetch_add(&said, 1, __ATOMIC_RELAXED) < 8)
                rt_log("prompt glyphs: text %#llx: sheet %u mapped at %#llx for a build through bit 7 (%llu texels; the "
                       "holder's own address %#llx)", (unsigned long long)text, k, (unsigned long long)cpu,
                       (unsigned long long)size, (unsigned long long)(g64(g64(s + 0xe0) + 8) + g64(s + 0xe8)));
            memcpy((void *)(uintptr_t)(s + 0xf0), &cpu, 8);
        }
        vk_guest_written(cpu, size);
        any = 1;
    }
    if (!any) {
        __atomic_add_fetch(&sheet_unset, 1, __ATOMIC_RELAXED);
        return 1;
    }
    plat_mutex_lock(&glyph_lock);
    sheet_prepared++;
    int k = 0;
    while (k < sheet_nstamp && sheet_stamp[k].text != text) k++;
    if (k < SHEET_STAMPS) {
        sheet_stamp[k].text = text, sheet_stamp[k].idx = g32(text + 0xa0), sheet_stamp[k].draws = 0;
        for (uint32_t j = 0; j < 4; j++) {
            uint64_t cpu = 0, size = 0;
            sheet_stamp[k].h[j] = j < n && (cpu = sheet_cpu(sheets + 0x158ull * j, &size)) ? sheet_hash(cpu, size) : 0;
        }
        if (k == sheet_nstamp) __atomic_store_n(&sheet_nstamp, sheet_nstamp + 1, __ATOMIC_RELAXED);
    }
    plat_mutex_unlock(&glyph_lock);
    return 1;
}

// At a text's draw after a build through bit 7 that sheet_ready prepared: once the ring moved on (the build ran), the
// texture cache is told the sheet it took was written.
static void sheet_stamp_draw(uint64_t text) {
    plat_mutex_lock(&glyph_lock);
    for (int i = 0; i < sheet_nstamp; i++) {
        if (sheet_stamp[i].text != text) continue;
        uint32_t idx = g32(text + 0xa0), n = g32(text + 0xa4);
        uint64_t sheets = g64(text + 0x68), cpu = 0, size = 0;
        if (idx != sheet_stamp[i].idx && sheets && !(sheets & 7) && sheets < RT_ADDRESS_LIMIT && n && n <= 16 && idx < n &&
            (cpu = sheet_cpu(sheets + 0x158ull * idx, &size))) {
            vk_guest_written(cpu, size);
            if (idx < 4 && sheet_hash(cpu, size) != sheet_stamp[i].h[idx] && sheet_drawn++ < 8)
                rt_log("prompt glyphs: text %#llx: its build drew into sheet %u, mapped for it (%llu texels)",
                       (unsigned long long)text, idx, (unsigned long long)size);
        }
        if (idx != sheet_stamp[i].idx || ++sheet_stamp[i].draws > 64) {
            sheet_stamp[i] = sheet_stamp[--sheet_nstamp];
            __atomic_store_n(&sheet_nstamp, sheet_nstamp, __ATOMIC_RELAXED);
        }
        break;
    }
    plat_mutex_unlock(&glyph_lock);
}

// 1 when it asked for a build through bit 7 (the text's sheets prepared for it).
static int glyph_pending_draw(uint64_t text) {
    plat_mutex_lock(&glyph_lock);
    for (int i = 0; i < glyph_npend; i++) {
        if (glyph_pend[i].text != text) continue;
        uint8_t now[64];
        int read = plat_mem_read(glyph_pend[i].drec, now, sizeof now), uv = 0;
        for (int k = 0x10; k < 0x20 && read; k++) uv |= now[k];
        // done when the texture coordinates changed (a quad moved but still on the old glyph is not done: seen when the
        // text was laid out anew in a loading screen without its glyph being plotted)
        if (!read || (uv && memcmp(now + 0x10, glyph_pend[i].stale + 0x10, 0x10))) {
            if (read && glyph_fixed < 64)
                rt_log("prompt glyphs: text %#llx was drawn with its new buttons after %u draws", (unsigned long long)text,
                       glyph_pend[i].draws);
            glyph_pend[i] = glyph_pend[--glyph_npend];
            break;
        }
        uint32_t f = g32(text + 0x180);
        int ask = !(f & 0x80) && ++glyph_pend[i].draws % 4 == 0;
        plat_mutex_unlock(&glyph_lock);
        if (ask && sheet_ready(text)) {   // sheet_ready takes glyph_lock itself
            f = g32(text + 0x180) | 0x80;
            memcpy((void *)(uintptr_t)(text + 0x180), &f, 4);
            return 1;
        }
        return 0;
    }
    plat_mutex_unlock(&glyph_lock);
    return 0;
}

// 1 when text is to be looked at for change gen (its first draw since), 0 when it was.
static int glyph_text_first(uint64_t text, uint32_t gen) {
    uint64_t now = plat_now_ns();
    plat_mutex_lock(&glyph_lock);
    if (gen != glyph_gen) {
        if (glyph_gen)
            rt_log("prompt glyphs: %u texts drawn since the last change, %u of them held another set's buttons (%u "
                   "characters swapped; %u builds with their sheets mapped, %u drew into them; %u without a ring, %u "
                   "with none set up)", glyph_looked, glyph_fixed_texts, glyph_fixed, sheet_prepared, sheet_drawn,
                   sheet_noring, sheet_unset);
        memset(glyph_texts, 0, sizeof glyph_texts);
        glyph_gen = gen, glyph_used = glyph_looked = glyph_fixed_texts = glyph_fixed = glyph_said = 0, glyph_since = now;
        glyph_npend = 0, sheet_npend = 0, sheet_prepared = sheet_drawn = sheet_noring = sheet_unset = 0;
        memset(glyph_marked, 0, sizeof glyph_marked), glyph_nmarked = 0;
    }
    if (glyph_said < 2 && now - glyph_since > (glyph_said ? 60000000000ull : 5000000000ull)) {
        rt_log("prompt glyphs: in the %d s after the change %u texts were drawn, %u of them held another set's buttons "
               "(%u characters swapped; %u builds with their sheets mapped, %u drew into them; %u without a ring, %u "
               "with none set up)", glyph_said ? 60 : 5, glyph_looked, glyph_fixed_texts, glyph_fixed, sheet_prepared,
               sheet_drawn, sheet_noring, sheet_unset);
        glyph_said++;
    }
    if (glyph_used >= GLYPH_TEXTS / 4 * 3) memset(glyph_texts, 0, sizeof glyph_texts), glyph_used = 0;
    int first = 1;
    uint32_t h = (uint32_t)((text >> 4) * 0x9E3779B1u) & (GLYPH_TEXTS - 1);
    for (;; h = (h + 1) & (GLYPH_TEXTS - 1)) {
        if (glyph_texts[h] == text) { first = 0; break; }
        if (!glyph_texts[h]) { glyph_texts[h] = text, glyph_used++; break; }
    }
    if (first) glyph_looked++;
    plat_mutex_unlock(&glyph_lock);
    return first;
}

// A large glyph (the action hint's key, size 160; the pause screen's guide, 102) is not drawn from the font's glyph
// cache: the text's full quad build (main+0x1515660) rasterises it on the CPU (main+0x1519300) into one of the text's
// own sheets, a ring (array at text+0x68, count at +0xa4, the one in use at +0xa0; each sheet 0x158 bytes: valid
// byte +8, memory pool +0xe0 and offset +0xe8, size +0x140 x +0x144, a byte a texel; the build moves the ring on,
// main+0xe4e2a4, and empties the sheet it takes, main+0x150f4d0). The draw (main+0xe4e0e0) runs that full build only
// when the text's state word (*(text+8)) holds bit 20 with bits 17 and 18 clear (main+0xe4e20c); bit 7 of text+0x180,
// which the small glyphs' fix sets, runs the other build (main+0x1516120), which moves the ring on but plots no
// large glyph: the blank key of the earlier tries, or the old one when the ring came back to the old sheet. The
// game sets a sheet up in its own layout only: the hint's key had sheet 1 set up and sheet 0 never. And the texture
// cache does not see CPU writes into a sheet after its first upload.
// So a text holding a large button glyph of another set is rebuilt through the full build: the ring is set one
// before a sheet that is set up (sheet_request), bit 20 asks for the build, and at a later draw, once the ring is on that
// sheet and its texels changed, the texture cache is told they were written (vk_guest_written). A text the game does
// not build now (hidden: bit 17 or 18) is asked again every fourth draw, for at most 3600 draws. A text whose sheet in
// use was never set up (the pause screen's guide before the screen opens) is left to the game's layout, and once the
// game built it the texture cache is told too: its sheet's storage may have held another picture before (the guide's
// Back kept the keyboard's SHIFT in one run while its Rotate and Equip took the pad's buttons).
// REKARINA_PROMPT_SHEETS=0: such a text is left to the game's own builds (the key keeps the old set until a restart).
void vk_guest_written(uint64_t addr, uint64_t size);
static int sheets_on = -1;

// Sheet k of text: its texels' CPU address and size; 0 when it is not set up.
static int sheet_at(uint64_t text, uint32_t k, uint64_t *cpu, uint64_t *size) {
    uint64_t sheets = g64(text + 0x68);
    uint32_t n = g32(text + 0xa4);
    if (!sheets || (sheets & 7) || !n || n > 16 || k >= n) return 0;
    uint64_t s = sheets + 0x158ull * k;
    if (!*(const uint8_t *)(uintptr_t)(s + 8)) return 0;
    uint64_t pool = g64(s + 0xe0), mem = pool && !(pool & 7) ? g64(pool + 8) : 0;
    uint32_t w = g32(s + 0x140), h = g32(s + 0x144);
    if (!mem || !w || !h || (uint64_t)w * h > (16u << 20)) return 0;
    *cpu = mem + g64(s + 0xe8), *size = (uint64_t)w * h;
    return 1;
}

static uint64_t sheet_hash(uint64_t cpu, uint64_t n) {
    const uint8_t *q = (const uint8_t *)(uintptr_t)cpu;
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint64_t i = 0; i < n; i++) h = (h ^ q[i]) * 0x100000001b3ull;
    return h;
}

// Sets the ring one before the first sheet that is set up (from the one after the sheet in use) and asks for the full
// build; the sheet the build will take in *target and its texels' hash in *hash. 0: no sheet set up, or the text is
// not built now (its state word holds bit 17 or 18).
static int sheet_request(uint64_t text, uint32_t *target, uint64_t *hash) {
    uint64_t st = g64(text + 8);
    uint32_t idx = g32(text + 0xa0), n = g32(text + 0xa4);
    if (!st || (st & 3) || st >= RT_ADDRESS_LIMIT || !n || n > 16 || idx >= n) return 0;
    uint32_t w = g32(st);
    if (w & 0x60000) return 0;
    for (uint32_t k = 1; k <= n; k++) {
        uint32_t t = (idx + k) % n, before = (t + n - 1) % n;
        uint64_t cpu, size;
        if (!sheet_at(text, t, &cpu, &size)) continue;
        *target = t, *hash = sheet_hash(cpu, size);
        memcpy((void *)(uintptr_t)(text + 0xa0), &before, 4);
        w |= 0x100000;
        memcpy((void *)(uintptr_t)st, &w, 4);
        return 1;
    }
    return 0;
}

static void sheet_pending_draw(uint64_t text) {
    plat_mutex_lock(&glyph_lock);
    for (int i = 0; i < sheet_npend; i++) {
        if (sheet_pend[i].text != text) continue;
        uint64_t st = g64(text + 8), cpu = 0, size = 0;
        uint32_t w = st && !(st & 3) && st < RT_ADDRESS_LIMIT ? g32(st) : 0, idx = g32(text + 0xa0);
        if (sheet_pend[i].watch) {   // left to the game's layout: once it built the text, its sheet's texels are taken again
            if (!(w & 0x100000) && idx != sheet_pend[i].idx && sheet_at(text, idx, &cpu, &size)) {
                vk_guest_written(cpu, size);
                rt_log("prompt glyphs: text %#llx: the game built it again (sheet %u); the texture cache takes the sheet's "
                       "%llu texels again", (unsigned long long)text, idx, (unsigned long long)size);
                sheet_pend[i] = sheet_pend[--sheet_npend];
            }
            break;   // no time limit: such a text is drawn only while its screen is open
        }
        int asked = sheet_pend[i].builds > 0, have = asked && sheet_at(text, sheet_pend[i].idx, &cpu, &size);
        if (asked && !(w & 0x100000) && idx == sheet_pend[i].idx && have && sheet_hash(cpu, size) != sheet_pend[i].hash) {
            vk_guest_written(cpu, size);   // the build plotted the new glyph into that sheet
            rt_log("prompt glyphs: text %#llx: its large glyph was plotted again into sheet %u (%u requests, %u draws); the "
                   "texture cache takes the sheet's %llu texels again", (unsigned long long)text, idx, sheet_pend[i].builds,
                   sheet_pend[i].draws, (unsigned long long)size);
            sheet_pend[i] = sheet_pend[--sheet_npend];
            break;
        }
        if (++sheet_pend[i].draws > 3600) {
            rt_log("prompt glyphs: text %#llx: its large glyph was not plotted again after %u draws (%u requests; state "
                   "%#x, ring at %u)", (unsigned long long)text, sheet_pend[i].draws, sheet_pend[i].builds, w, idx);
            sheet_pend[i] = sheet_pend[--sheet_npend];
            break;
        }
        if (!(w & 0x100000) && sheet_pend[i].draws % 4 == 0 && sheet_request(text, &sheet_pend[i].idx, &sheet_pend[i].hash))
            sheet_pend[i].builds++;
        if (sheet_pend[i].draws % 600 == 0)
            rt_log("prompt glyphs: text %#llx: waiting for its large glyph (%u draws, %u requests; state %#x, ring at %u)",
                   (unsigned long long)text, sheet_pend[i].draws, sheet_pend[i].builds, w, idx);
        break;
    }
    plat_mutex_unlock(&glyph_lock);
}

// The pass over one text at its draw; 1 when it asked for a build through bit 7 (the text's sheets prepared for it).
static int prompt_glyphs_pass(uint64_t text, uint32_t gen) {
    if (__atomic_load_n(&glyph_npend, __ATOMIC_RELAXED) && glyph_pending_draw(text)) return 1;
    if (__atomic_load_n(&sheet_npend, __ATOMIC_RELAXED)) sheet_pending_draw(text);
    // once per text after a change, and again whenever its quads are about to be built anew (bit 7: the game laid it
    // out again, which takes its records back to the shaping's glyphs and sizes)
    if (!glyph_text_first(text, gen) && !(g32(text + 0x180) & 0x80)) return 0;
    uint64_t pages = 0;
    uint32_t n = 0;
    uint8_t compact = 0;
    if (!plat_mem_read(text + 0xac, &n, 4) || !n || n > (1u << 20) || !plat_mem_read(text + 0x30, &pages, 8) ||
        !pages || (pages & 7) || !plat_mem_read(text + 0xd5, &compact, 1))
        return 0;
    uint32_t size = compact ? 0x48 : 0xa0, code_at = compact ? 0 : 8, glyph_at = compact ? 0x3c : 0x94;
    int fixed = 0, passive = 0;   // records changed: built now / when the game builds the text
    uint64_t drec = 0;   // the draw record of the first glyph swapped
    uint8_t *rec = malloc((size_t)size * (n < 1024 ? n : 1024));
    if (!rec) return 0;
    for (uint32_t p = 0; p * 1024 < n; p++) {
        uint64_t page = 0;
        uint32_t k = n - p * 1024 < 1024 ? n - p * 1024 : 1024;
        if (!plat_mem_read(pages + 8ull * p, &page, 8) || !page || (page & 7) || !plat_mem_read(page, rec, (size_t)size * k))
            break;
        for (uint32_t i = 0; i < k; i++) {
            uint32_t cp;
            uint16_t glyph;
            memcpy(&cp, rec + (size_t)size * i + code_at, 4);
            memcpy(&glyph, rec + (size_t)size * i + glyph_at, 2);
            uint32_t now = romfs_prompt_glyph(cp, glyph);
            if (now == glyph) continue;
            uint64_t at = page + (uint64_t)size * i;
            float fs = 0;
            if (!compact) memcpy(&fs, rec + (size_t)size * i + 0x1c, 4);
            // A large glyph (size over 96: the action hint's key, the pause screen's guide) is drawn from the text's
            // own sheet, not the glyph cache, and a build through bit 7 does not plot it: its record is changed the
            // same way, and a text the game built before is built again through the full build (sheet_request,
            // sheet_pending_draw); one it never built (the guide before the pause screen first opened) is built when
            // the game builds it.
            int large = fs > 96.0f;
            if (sheets_on < 0) sheets_on = rt_knob("PROMPT_SHEETS", "1");
            int forced = 0;   // rebuilt through the full build (sheet_request), never through bit 7
            if (large && sheets_on) {
                plat_mutex_lock(&glyph_lock);
                int k = 0;
                while (k < sheet_npend && sheet_pend[k].text != text) k++;
                uint64_t cpu, sz;
                if (k < sheet_npend) forced = 1;
                else if (k < SHEET_PENDING && sheet_at(text, g32(text + 0xa0), &cpu, &sz)) {   // the game built it before
                    forced = 1;
                    sheet_pend[k].text = text, sheet_pend[k].draws = sheet_pend[k].builds = sheet_pend[k].watch = 0;
                    if (sheet_request(text, &sheet_pend[k].idx, &sheet_pend[k].hash)) sheet_pend[k].builds = 1;
                    __atomic_store_n(&sheet_npend, sheet_npend + 1, __ATOMIC_RELAXED);
                } else if (k < SHEET_PENDING) {   // never built: the game's layout builds it, then its sheet is taken again
                    sheet_pend[k].text = text, sheet_pend[k].draws = sheet_pend[k].builds = 0, sheet_pend[k].watch = 1;
                    sheet_pend[k].idx = g32(text + 0xa0), sheet_pend[k].hash = 0;
                    __atomic_store_n(&sheet_npend, sheet_npend + 1, __ATOMIC_RELAXED);
                }
                plat_mutex_unlock(&glyph_lock);
            }
            int *count = large ? &passive : &fixed;
            if (!glyph && !compact) {
                // Drawn by character: the plot looks the character up in the table in use, but the glyph cache keys the
                // picture by character, glyph 0 and size, so it gave the picture the character had when it was first
                // drawn. So the record keeps 0 and its size (+0x1c and +0x20, the two floats the plot's size and the
                // cache key come from: main+0x1512ea0) gets this change's mark in the lowest four bits of each: a key
                // no earlier change used, at the same size to a millionth.
                uint32_t sz[2], k = gen % 15 + 1;
                memcpy(sz, rec + (size_t)size * i + 0x1c, 8);
                uint32_t mk[2] = {(sz[0] & ~0xFu) | k, (sz[1] & ~0xFu) | k};
                if (mk[0] == sz[0] && mk[1] == sz[1]) continue;   // marked for this change already
                if (!fixed && !large) {
                    uint64_t dp = 0;
                    if (plat_mem_read(text + 0x38, &dp, 8) && dp && !(dp & 7) && plat_mem_read(dp + 8ull * p, &drec, 8) && drec)
                        drec += 0xe0ull * i;
                }
                memcpy((void *)(uintptr_t)(at + 0x1c), mk, 8);
                if (++*count <= 4 && glyph_fixed < 32)
                    rt_log("prompt glyphs: text %#llx character %u U+%04X (size %.1f): drawn by character, its size marked "
                           "for this change%s", (unsigned long long)text, p * 1024 + i, cp, fs,
                           large && !forced ? ", built when the game builds it" : forced ? ", rebuilt with its sheet" : "");
                continue;
            }
            if (!fixed && !large) {
                uint64_t dp = 0;
                if (plat_mem_read(text + 0x38, &dp, 8) && dp && !(dp & 7) && plat_mem_read(dp + 8ull * p, &drec, 8) && drec)
                    drec += 0xe0ull * i;
            }
            uint16_t v = (uint16_t)now;
            memcpy((void *)(uintptr_t)(at + glyph_at), &v, 2);
            if (++*count <= 4 && glyph_fixed < 32)
                rt_log("prompt glyphs: text %#llx character %u U+%04X (size %.1f): glyph %u -> %u%s", (unsigned long long)text,
                       p * 1024 + i, cp, fs, glyph, now, large && !forced ? ", built when the game builds it" : forced ? ", rebuilt with its sheet" : "");
        }
    }
    free(rec);
    if (fixed || passive) {
        plat_mutex_lock(&glyph_lock);
        glyph_marked_at(text, 1);   // a build through bit 7 of it now misses the glyph cache: sheet_ready first
        glyph_fixed += passive;
        if (!fixed) glyph_fixed_texts++;
        plat_mutex_unlock(&glyph_lock);
    }
    if (!fixed || !sheet_ready(text)) return 0;
    uint32_t f = g32(text + 0x180) | 0x80;   // the quads are built again from the records, in this draw
    plat_mutex_lock(&glyph_lock);
    glyph_fixed_texts++, glyph_fixed += fixed;
    if (drec && glyph_npend < GLYPH_PENDING && plat_mem_read(drec, glyph_pend[glyph_npend].stale, 64)) {
        int k = 0;
        while (k < glyph_npend && glyph_pend[k].text != text) k++;
        if (k == glyph_npend) {
            glyph_pend[k].text = text, glyph_pend[k].drec = drec, glyph_pend[k].draws = 0;
            __atomic_store_n(&glyph_npend, glyph_npend + 1, __ATOMIC_RELAXED);
        }
    }
    plat_mutex_unlock(&glyph_lock);
    memcpy((void *)(uintptr_t)(text + 0x180), &f, 4);
    return 1;
}

static void prompt_glyphs(Cpu *c) {
    uint32_t gen = romfs_text_generation();
    uint64_t text = c->x[0];
    if (!gen || !text || (text & 7) || text >= RT_ADDRESS_LIMIT) return;
    if (__atomic_load_n(&sheet_nstamp, __ATOMIC_RELAXED)) sheet_stamp_draw(text);   // the build of an earlier draw
    if (prompt_glyphs_pass(text, gen)) return;
    // the game asks for a build through bit 7 of a text this pass changed (it laid the text out again): its sheets too
    if (!(g32(text + 0x180) & 0x80) || !__atomic_load_n(&glyph_nmarked, __ATOMIC_RELAXED)) return;
    plat_mutex_lock(&glyph_lock);
    int marked = glyph_marked_at(text, 0);
    plat_mutex_unlock(&glyph_lock);
    if (marked) sheet_ready(text);
}

__attribute__((constructor)) static void oot_fps_register(void) {
    rt_register_host(HOST_DAY_CLOCK, day_clock);
    rt_register_host(HOST_SOUND_GATE, sound_gate);
    rt_register_host(HOST_SOUND_STEP, sound_step);
    rt_register_host(HOST_MUSIC_TIME, music_time);
    rt_register_host(HOST_MUSIC_FLAGS, music_flags);
    rt_register_host(HOST_SONG_CUE0, song_cue0);
    rt_register_host(HOST_SONG_CUE1, song_cue1);
    rt_register_host(HOST_HORSE_JUMP, horse_jump);
    rt_register_host(HOST_FLOOR_CARRY, floor_carry);
    rt_register_host(HOST_SONG_PLAY, song_play);
    rt_register_host(HOST_SONG_RECORD, song_record);
    rt_register_host(HOST_SONG_DONE, song_done);
    rt_register_host(HOST_LOOP_STEP0, loop_step0);
    rt_register_host(HOST_LOOP_STEP1, loop_step1);
    rt_register_host(HOST_LOOP_POSE, loop_pose);
    rt_register_host(HOST_CLOTH_TIME, cloth_time);
    rt_register_host(HOST_WEB_STEPS, web_steps);
    rt_register_host(HOST_INPUT_STEP, input_step);
    rt_register_host(HOST_DOF_SCALE, dof_scale);
    rt_register_host(HOST_PROMPT_GLYPHS, prompt_glyphs);
}

#endif
