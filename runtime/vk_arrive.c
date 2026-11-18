// The arrival hold: the frames that start a scene, and those of a burst of new textures while no scene is drawn, are
// made but not shown while the textures they name are created and uploaded.
//
// A map's first seconds and the start were the slowest of the game: the game registers a scene's textures in its texture
// pool as its first frames run (the opening after the logos: some 900 in one frame), and each one then cost its
// creation, its storage's hash and its cache file's read on the replay thread (vk_texpool.c update, vk_texture.c
// entry_of / upload): 10-20 s at 10-17 fps from the start, 2-3 s of ramp-up at an arrival (2026-11-17). The texture
// stream (vk_texture.c) now does the hash and the read on worker threads, and the loading screen is where the wait for
// them belongs. From the first frame that draws a scene after one (no submission drew a scene for COLD_LOAD frames), or
// from a frame that brings a burst of textures while no scene is drawn (BURST_JOBS stream jobs queued or BURST_MADE
// textures made in it: the opening that follows the logos fades in from black), the frames are run as fast as they
// come, at the shortest game step (frame_rate.c frame_step_hold: the animation and the fade barely move meanwhile), and
// not presented, so the display keeps its last picture; the pool's slots wait for their stream jobs and resolve with a
// long budget (vk_texpool.c update). The first frame shown is made after the backlog was worked off: the textures are
// there and the frame rate is the place's own from its first picture. The hold ends when CALM_FRAMES frames in a row
// found little left to do (pool slots waiting, textures made in the frame, stream jobs queued), or after
// REKARINA_ARRIVE_HOLD_MS (default 2500), whichever comes first. Only the synchronous replay holds (the asynchronous one
// presents on its own thread).
//
// REKARINA_ARRIVE_HOLD=0 (or REKARINA_UPFRONT=0): every frame is shown as it is made, as before. The frame-time
// summaries (perf.c) count no held frame: the player sees the last picture stay up for the hold's length, which the
// hold's log line reports.
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "gfx.h"
#include "host.h"
#include "nvn_api.h"
#include "prep.h"
#include "vk.h"
#include "platform/platform.h"

#define COLD_LOAD 30        // frames without a scene that make a loading screen (vk_texture.c COLD_LOAD)
#define CALM_FRAMES 4
#define BURST_JOBS 128     // a burst while no scene is drawn: stream jobs queued
#define BURST_MADE 64      // or textures made in one frame
#define HELD_RING 1024

int vk_tpool_bound_recently(uint64_t frame);                 // vk_texpool.c: the pool was bound in that frame
int vk_texture_scene_in_frame(void);                         // vk_texture.c: the frame just made drew a scene
void vk_tpool_backlog(int *registered, int *unresolved);     // vk_texpool.c
int vk_texture_stream_live(void);                            // vk_texture.c: texture stream jobs queued or running
uint64_t vk_texture_made_total(void);                        // vk_texture.c: entries made so far
void frame_step_hold(int on);                                // frame_rate.c

// REKARINA_UPFRONT=0: the texture work of a start and an arrival as before 2026-11-17, every part at once: no arrival
// hold, no texture stream (vk_texture.c), the pool's budget looked at every sixteenth slot (vk_texpool.c), the start's
// read of the texture cache into the file cache at once (vk_texture.c tex_cache_scan). Each part keeps its own setting.
int vk_upfront(void) {
    static int on = -1;
    if (on < 0) on = rt_knob("UPFRONT", "1") != 0;
    return on;
}

static int hold_on(void) {
    static int on = -1;
    if (on < 0) {
        on = rt_knob("ARRIVE_HOLD", vk_upfront() ? "1" : "0") != 0;
        rt_log("arrival hold: %s (REKARINA_ARRIVE_HOLD)", on ? "a scene's first frames are not shown while its textures are made"
                                                              : "off: every frame is shown as it is made");
    }
    return on;
}

static uint64_t hold_cap_ns(void) {
    static uint64_t ns;
    if (!ns) {
        const char *e = cfg_get("REKARINA_ARRIVE_HOLD_MS");
        int ms = e && *e ? atoi(e) : 2500;
        ns = (uint64_t)(ms > 0 ? ms : 1) * 1000000ull;
    }
    return ns;
}

static struct {
    int loading, holding, calm, kind;   // kind: 0 a scene's first frames, 1 a burst while no scene was drawn
    uint64_t t0, f0, made0, made_prev, last_frame;
    int unres0, jobs0, peak_unres;
    uint64_t n_holds, held_frames, held_ns;
} H = {.loading = COLD_LOAD + 1};   // the game starts behind its boot picture: a loading screen
static uint64_t held_ring[HELD_RING];

int vk_arrive_holding(void) { return H.holding; }

// A hold is under way: nothing on screen reads the scene's textures, so the texture stream's pool slots wait for their
// jobs however long they take (vk_texture.c vk_texture_stream_defer).
int vk_arrive_unshown(void) { return H.holding; }

// The first scene was shown (its hold ended, or it needed none): the start's cache warm read waits for it (vk_texture.c).
static volatile int first_shown;
int vk_arrive_first_shown(void) { return __atomic_load_n(&first_shown, __ATOMIC_ACQUIRE); }

int vk_arrive_held(uint64_t frame) { return frame && held_ring[frame % HELD_RING] == frame; }

static void release(uint64_t frame, uint64_t now, int unres, int jobs, uint64_t made, const char *why) {
    uint64_t ns = now - H.t0, n = frame - H.f0;
    H.holding = 0, H.calm = 0;
    H.n_holds++, H.held_frames += n, H.held_ns += ns;
    frame_step_hold(0);
    __atomic_store_n(&first_shown, 1, __ATOMIC_RELEASE);
    rt_log("arrival hold: map %s: frames %llu-%llu not shown, %.0f ms (%s); at its start %d pool slots waited and %d stream jobs "
           "ran, at most %d slots; %llu textures made in it; at its end %d slots, %d jobs, %llu made in the last frame "
           "(holds so far %llu, %llu frames, %.1f s)",
           prep_map_now(), (unsigned long long)H.f0, (unsigned long long)(frame - 1), ns / 1e6, why, H.unres0, H.jobs0,
           H.peak_unres, (unsigned long long)(vk_texture_made_total() - H.made0), unres, jobs, (unsigned long long)made,
           (unsigned long long)H.n_holds, (unsigned long long)H.held_frames, H.held_ns / 1e9);
}

// nvn_api_obj.c nvnQueuePresentTexture, before it paces the present: 1 when the frame about to be presented is held
// (not shown, not paced). The render thread, once per present.
int gfx_arrive_hold_next(void) {
    if (!hold_on() || gfx_replay_async() || !vk_ctx()) {
        if (!first_shown && vk_ctx()) __atomic_store_n(&first_shown, vk_texture_scene_in_frame(), __ATOMIC_RELEASE);
        return 0;
    }
    uint64_t frame = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) + 1, now = plat_now_ns();
    uint64_t made = vk_texture_made_total(), made_frame = made - H.made_prev;
    H.made_prev = made;
    int scene = vk_texture_scene_in_frame();
    if (!H.holding) {
        int reg, unres, jobs = vk_texture_stream_live();
        if (!scene) {
            if (++H.loading > COLD_LOAD) H.loading = COLD_LOAD + 1;
            // a burst of textures while no scene is drawn: the game's opening after its logos starts so, a loading
            // screen too (its picture is black or still): held the same way, so the first frames shown after it find
            // the textures made. A smaller burst is the pool's and the stream's business, frame by frame.
            if (H.loading <= COLD_LOAD || (jobs < BURST_JOBS && made_frame < BURST_MADE)) return 0;
            unres = -1;
            H.kind = 1;
        } else {
            int was_loading = H.loading > COLD_LOAD;
            H.loading = 0;
            if (!was_loading) return 0;
            vk_tpool_backlog(&reg, &unres);
            // the first scene frame after a loading screen: hold when it left work for the next ones
            if (unres < 64 && made_frame < 32 && jobs < 32) {
                __atomic_store_n(&first_shown, 1, __ATOMIC_RELEASE);
                rt_log("arrival hold: map %s, frame %llu: not needed (%d pool slots wait, %llu textures made, %d stream jobs)",
                       prep_map_now(), (unsigned long long)frame, unres, (unsigned long long)made_frame, jobs);
                return 0;
            }
            H.kind = 0;
        }
        H.holding = 1, H.calm = 0, H.t0 = now, H.f0 = frame, H.made0 = made - made_frame;
        H.unres0 = H.peak_unres = unres, H.jobs0 = jobs;
        frame_step_hold(1);
    } else {
        int reg, unres, jobs = vk_texture_stream_live();
        vk_tpool_backlog(&reg, &unres);
        if (unres > H.peak_unres) H.peak_unres = unres;
        H.calm = unres < 32 && made_frame < 8 && jobs < 8 ? H.calm + 1 : 0;
        if (scene) H.loading = 0;
        else if (++H.loading > COLD_LOAD) H.loading = COLD_LOAD + 1;
        if (!scene && !H.kind) {   // another loading screen began: nothing of this map to wait for
            release(frame, now, unres, jobs, made_frame, "a loading screen again");
            H.loading = 1;
            return 0;
        }
        if (H.calm >= CALM_FRAMES) { release(frame, now, unres, jobs, made_frame, "the textures are there"); return 0; }
        if (now - H.t0 >= hold_cap_ns()) { release(frame, now, unres, jobs, made_frame, "the longest hold"); return 0; }
    }
    held_ring[frame % HELD_RING] = frame;
    return 1;
}
