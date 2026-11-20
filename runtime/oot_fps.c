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
// frames for a loop of one frame or less while the frame is short.
static float loop_product(Cpu *c, float speed) {
    float step = vget(c, 8);
    uint64_t entry = c->x[20];
    if (short_frame(frame_step()) && step > 0.0f && loop_is_pose(gf(entry + 0x10), gf(entry + 0x14))) {
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
// the store, of the range's start for a loop of one frame or less while the frame is short.
static void loop_pose(Cpu *c) {
    first_run(&loops.seen[1], "a loop of one frame is drawn at its start (set looppose)");
    uint64_t at = c->x[22];
    uint32_t time = (uint32_t)c->v[0];
    if (short_frame(frame_step()) && loop_is_pose(gf(at + 4), gf(at + 8))) time = g32(at + 4), count(&loops.poses, 1);
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
}

#endif
