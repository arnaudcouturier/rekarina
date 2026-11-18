// Frame timing (REKARINA_TRACE=perf, the frame-time summary, the performance overlay's counts), the A/B switches (rt_ab,
// rt_knob) and REKARINA_ONE_PER_CORE. The sampling profiler (REKARINA_PROFILE) is the OS's (prof_win.c); without one the weak
// prof_sampler_* below do nothing.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "flight.h"
#include "gfx.h"
#include "host.h"
#include "pacelog.h"   // DEBUG-PACELOG
#include "perfmon.h"
#include "prep.h"
#include "prof.h"
#include "platform/platform.h"

volatile uint32_t prof_render_tid, prof_main_tid, prof_replay_tid;

__attribute__((weak)) int prof_sampler_start(void) {
    if (cfg_get("REKARINA_PROFILE")) rt_log("prof: REKARINA_PROFILE: no sampling profiler on this OS");
    return 0;
}
__attribute__((weak)) void prof_sampler_write(void) {}
__attribute__((weak)) void prof_sampler_frame(uint64_t frame, uint64_t now, uint64_t d) { (void)frame, (void)now, (void)d; }

#ifndef _WIN32   // the debug watches and the replay threads' ids are prof_win.c's (debug registers); none elsewhere
volatile uint32_t prof_par_tid[16];
void prof_hwwatch_tick(uint64_t frame) { (void)frame; }
void prof_fnwatch(uint64_t addr, uint64_t x0) { (void)addr, (void)x0; }
#endif

// REKARINA_TRACE=perf, REKARINA_FLIGHT=1 or the full performance overlay: event counts are taken. Wait times are always taken
// (frame-time summary). perf_base: the part fixed at start.
static int perf_on, perf_base;
static int overlay;   // performance overlay mode (perf_set_overlay): 0 off, 1 fps, 2 full

void perf_set_overlay(int mode) {
    __atomic_store_n(&overlay, mode, __ATOMIC_RELAXED);
    __atomic_store_n(&perf_on, perf_base || mode == 2, __ATOMIC_RELAXED);
}

int perf_gpu_timing(void) { return (rt_trace & RT_TRACE_PERF) || __atomic_load_n(&overlay, __ATOMIC_RELAXED) == 2; }

// Whether something reads the frame clock's GPU timestamps now (gfx_present.c bt_write takes them only then): the
// performance overlay in either mode (its frame rate, lows and graph), REKARINA_FRAMETIMES, the pacing log. With all of
// them off a player pays nothing for the clock, and the minute's "frametime:" lines count the present calls.
static int ftl_wanted(void);
int perf_frame_clock_wanted(void) {
    return __atomic_load_n(&overlay, __ATOMIC_RELAXED) || ftl_wanted() || pacelog_on();
}

static volatile int64_t gpu_busy_total;

void perf_gpu_busy(uint64_t ns) { plat_add64(&gpu_busy_total, (int64_t)ns); }

uint64_t perf_gpu_busy_total(void) { return (uint64_t)__atomic_load_n(&gpu_busy_total, __ATOMIC_RELAXED); }

// ---- REKARINA_SECTIONS (host.h SEC_LIST): one row per present of what the replay did since the previous present.
int sec_on;
_Thread_local int sec_thread_off;
static double sec_tick_ns = 1.0;
static FILE *sec_f;
static volatile int64_t sec_ticks[SEC_N], sec_counts[SEC_N], sec_peaks[SEC_N];
static const char *const sec_names[SEC_N] = {
#define SEC_X(n) #n,
    SEC_LIST(SEC_X)
#undef SEC_X
};
void rec_sec_packets(int64_t *out);   // vk_rec.c: packets so far by class, SEC_PK_STATE order
__attribute__((weak)) void rec_sec_packets(int64_t *out) { (void)out; }
void vk_tpool_backlog(int *registered, int *unresolved);   // vk_texpool.c: slots registered, slots the pool has not resolved
__attribute__((weak)) void vk_tpool_backlog(int *registered, int *unresolved) { *registered = *unresolved = 0; }

// The section timers run in every game for the long-frame report below (REKARINA_LONGFRAME=0 turns both off); the
// rows of REKARINA_SECTIONS are written only with that setting.
static double sec_cost_ns;   // what one section costs on this machine (sec_init)
static int lf_on = -1;
void sec_init(void) {
    static int done;
    if (done) return;
    done = 1;
    lf_on = rt_knob("LONGFRAME", "1");
    const char *e = cfg_get("REKARINA_SECTIONS");
    if (e && *e) {
        sec_f = fopen(e, "w");
        if (!sec_f) rt_log("sec: REKARINA_SECTIONS: cannot write %s", e);
        else setvbuf(sec_f, NULL, _IOFBF, 1 << 20);
    }
    if (!sec_f && !lf_on) return;
    uint64_t c0 = sec_clock(), n0 = plat_now_ns(), n1;
    while ((n1 = plat_now_ns()) - n0 < 20000000) {}   // 20 ms against the ns clock
    sec_tick_ns = (double)(n1 - n0) / (double)(sec_clock() - c0);
    if (sec_f) {
        fprintf(sec_f, "frame,interval_us,replay_us,gpu_busy_us,pace_us,fence_us,drain_us,pipe_us,xlate_us,tex_us,astc_us");
        for (int i = 0; i < SEC_N; i++) fprintf(sec_f, ",%s_us,%s_n,%s_max", sec_names[i], sec_names[i], sec_names[i]);
        fprintf(sec_f, "\n");
    }
    // What one section costs on this machine, for the overhead the rows carry: a million sections on the real path.
    sec_on = 1;
    uint64_t t0 = sec_clock();
    for (int i = 0; i < 1000000; i++) sec_end(SEC_ROW, sec_begin());
    double one = sec_ticks_ms(sec_clock() - t0);   // ms for a million: ns for one
    sec_on = 0, sec_ticks[SEC_ROW] = sec_counts[SEC_ROW] = sec_peaks[SEC_ROW] = 0;
    sec_cost_ns = one;
    rt_log("sec: section timers on, %.3f GHz cycle clock, %s, a section costs %.1f ns", 1.0 / sec_tick_ns,
           sec_f ? "rows to the REKARINA_SECTIONS file" : "for the long-frame report", one);
    __atomic_store_n(&sec_on, 1, __ATOMIC_RELEASE);
}

double sec_ticks_ms(uint64_t ticks) { return (double)ticks * sec_tick_ns / 1e6; }
static volatile uint64_t sec_frame_now;   // the frame being built (log lines from other threads)

void sec_add(int kind, uint64_t ticks) {
    __atomic_fetch_add(&sec_ticks[kind], (int64_t)ticks, __ATOMIC_RELAXED);
    __atomic_fetch_add(&sec_counts[kind], 1, __ATOMIC_RELAXED);
    int64_t o = __atomic_load_n(&sec_peaks[kind], __ATOMIC_RELAXED), v = (int64_t)ticks;
    while (v > o && !__atomic_compare_exchange_n(&sec_peaks[kind], &o, v, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
}

void sec_add_ns(int kind, uint64_t ns) { if (sec_tick_ns > 0) sec_add(kind, (uint64_t)((double)ns / sec_tick_ns)); }

void sec_count(int kind, int64_t n) { __atomic_fetch_add(&sec_counts[kind], n, __ATOMIC_RELAXED); }

void sec_peak(int kind, uint64_t v) {   // v is a count: stored so that the row's tick conversion gives it back
    int64_t o = __atomic_load_n(&sec_peaks[kind], __ATOMIC_RELAXED), t = (int64_t)((double)v * 1e3 / sec_tick_ns);
    while (t > o && !__atomic_compare_exchange_n(&sec_peaks[kind], &o, t, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
}

// VMA's vkAllocateMemory (vk_device.c): every call is summed by the thread class, one of 32 MiB or more gets a log line.
static volatile int64_t lf_alloc_ticks, lf_alloc_size;   // the frame's slowest vkAllocateMemory (long-frame report)
void sec_alloc_note(uint64_t size, uint32_t type, uint64_t ticks) {
    uint32_t tid = plat_thread_id();
    int replay = tid == prof_render_tid || tid == prof_replay_tid;
    sec_add(replay ? SEC_VKALLOC_R : SEC_VKALLOC_O, ticks);
    int64_t o = __atomic_load_n(&lf_alloc_ticks, __ATOMIC_RELAXED);
    if ((int64_t)ticks > o &&
        __atomic_compare_exchange_n(&lf_alloc_ticks, &o, (int64_t)ticks, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
        __atomic_store_n(&lf_alloc_size, (int64_t)size, __ATOMIC_RELAXED);
    if (size >= 32ull << 20)
        rt_log("sec: vkAllocateMemory frame %llu: %s thread %u, %.0f MiB type %u: %.2f ms",
               (unsigned long long)sec_frame_now, replay ? "the replay" : "another", tid,
               size / 1048576.0, type, sec_ticks_ms(ticks));
}

static int64_t fr_ticks[SEC_N], fr_counts[SEC_N];   // the sections of the frame just presented (sec_frame)
static void sec_frame(uint64_t frame, uint64_t d, uint64_t rep, const uint64_t *waits) {
    sec_frame_now = frame + 1;
    static int64_t pk_prev[6], gpu_prev, accum[SEC_N], total_reg;
    int64_t pk[6] = {0};
    rec_sec_packets(pk);
    for (int i = 0; i < 6; i++) sec_counts[SEC_PK_STATE + i] += pk[i] - pk_prev[i], pk_prev[i] = pk[i];
    int64_t gpu = (int64_t)perf_gpu_busy_total(), gd = gpu - gpu_prev;
    gpu_prev = gpu;
    if (sec_f) {
        fprintf(sec_f, "%llu,%.1f,%.1f,%.1f", (unsigned long long)frame, d / 1e3, rep / 1e3, gd / 1e3);
        for (int k = 0; k < 7; k++) fprintf(sec_f, ",%.1f", waits[k] / 1e3);
    }
    for (int i = 0; i < SEC_N; i++) {
        int64_t t = plat_xchg64(&sec_ticks[i], 0), n = plat_xchg64(&sec_counts[i], 0), m = plat_xchg64(&sec_peaks[i], 0);
        fr_ticks[i] = t, fr_counts[i] = n;
        if (!sec_f) continue;
        accum[i] += n;
        fprintf(sec_f, ",%.1f,%lld,%.1f", (double)t * sec_tick_ns / 1e3, (long long)n, (double)m * sec_tick_ns / 1e3);
    }
    if (!sec_f) return;   // the CSV row and the load line below: only with REKARINA_SECTIONS
    fprintf(sec_f, "\n");
    if (frame % 60) return;
    int reg, unres;
    vk_tpool_backlog(&reg, &unres);
    total_reg += accum[SEC_TEXREG];
    rt_log("sec: frame %llu: texture_register %lld calls (%lld in all); pool slots %d registered, %d unresolved; entries made: "
           "targets and texture gets %lld, draw bindings %lld, pool update %lld; images aliasing pool memory new %lld, repeating a "
           "destroyed address and shape %lld; own memory new %lld, repeating %lld; mirrors refused for budget %lld",
           (unsigned long long)frame, (long long)accum[SEC_TEXREG], (long long)total_reg, reg, unres, (long long)accum[SEC_MADE_T],
           (long long)accum[SEC_MADE_D], (long long)accum[SEC_MADE_P], (long long)accum[SEC_REP_ALIAS_NEW],
           (long long)accum[SEC_REP_ALIAS_REPEAT], (long long)accum[SEC_REP_OWN_NEW], (long long)accum[SEC_REP_OWN_REPEAT],
           (long long)accum[SEC_MR_BUDGET]);
    memset(accum, 0, sizeof accum);
}

void prof_start(void) {
    sec_init();
    flight_init();
    atexit(perf_summary_flush);   // boot.c's _exit path goes through prof_write instead
    perf_base = (rt_trace & RT_TRACE_PERF) || flight_on;
    perf_on = perf_base || overlay == 2;
    if (prof_sampler_start()) atexit(prof_write);   // a --seconds run ends in _exit: boot.c calls prof_write itself first
}

void prof_write(void) {
    perf_summary_flush();
    prof_sampler_write();
}

// ---- REKARINA_TRACE=perf: frame interval and the time the render thread spends waiting, logged every 60 presents. The
// flight recorder (REKARINA_FLIGHT=1, flight.c) takes the same numbers per frame.
static uint64_t wait_ns[PERF_N], frame_ns[PERF_N], worst_ns[PERF_N];   // window totals, this frame, the slowest frame

uint64_t perf_begin(void) { return kern_now_ns(); }

void perf_replay_thread(void) { prof_replay_tid = plat_thread_id(); }
int sec_replay_thread(void) { uint32_t t = plat_thread_id(); return t == prof_replay_tid || (!prof_replay_tid && t == prof_render_tid); }
void perf_main_thread(void) { prof_main_tid = plat_thread_id(); }

void perf_end(int kind, uint64_t t0) {
    if (!t0) return;
    uint32_t tid = plat_thread_id();
    if (tid != prof_render_tid && tid != prof_replay_tid) return;
    uint64_t d = kern_now_ns() - t0;
    if (tid == prof_render_tid && !prof_replay_tid) {   // one thread adds: no atomics on the synchronous path
        wait_ns[kind] += d;
        frame_ns[kind] += d;
        return;
    }
    __atomic_fetch_add(&wait_ns[kind], d, __ATOMIC_RELAXED);
    __atomic_fetch_add(&frame_ns[kind], d, __ATOMIC_RELAXED);
}

static volatile int64_t other_wait_ns;     // perf_gpu_wait on threads other than the render thread, over the window
static volatile int64_t render_wait_ns;    // the same on the render thread
void (*perf_gpu_stats)(PerfGpu *out);

void perf_gpu_wait(uint64_t t0) {
    if (!t0 || !__atomic_load_n(&perf_on, __ATOMIC_RELAXED)) return;
    int64_t d = (int64_t)(kern_now_ns() - t0);
    plat_add64(plat_thread_id() == prof_render_tid ? &render_wait_ns : &other_wait_ns, d);
}

static volatile int64_t replay_ns;         // perf_replay: summed over the window, any thread
static volatile int64_t frame_replay_ns;   // the same since the previous present (frame-time summary)

void perf_replay(uint64_t t0) {
    if (!t0) return;
    int64_t d = (int64_t)(kern_now_ns() - t0);
    plat_add64(&frame_replay_ns, d);
    if (__atomic_load_n(&perf_on, __ATOMIC_RELAXED)) plat_add64(&replay_ns, d);
}

static volatile int64_t counts[PERF_CNT_N];   // running totals: the perf log and the flight recorder take differences

void perf_count(int kind) {   // always: the long-frame report takes each frame's counts (an atomic add per event)
    plat_inc64(&counts[kind]);
}

// ---- Frame-time summary, always on: every 60 s of play and at exit one line of frame count, mean fps, 1% low and the
// frames over 25/50/100 ms, then the worst frames of the period with the render thread's waits in them. Per frame: a
// histogram increment, the compare against the worst list, a memset of the wait array. No allocation.
#define FT_BINS 1024              // 0.25 ms bins up to 256 ms; slower frames land in the last bin
#define FT_WORST 5
#define FT_PERIOD_NS 60000000000ull
typedef struct { uint64_t frame, ns, wait[PERF_N], replay; } FtWorst;
static struct {
    uint64_t t0, frames, sum, over25, over50, over100;
    uint32_t hist[FT_BINS];
    FtWorst worst[FT_WORST];   // slowest first
    int nworst;
} ft;
static uint64_t summary_t0;   // start of the current period (0: not started)

static const char *ft_clock_note(void);   // the frame clock (below)

static void ft_report(uint64_t now, const char *tag) {
    if (ft.frames && ft.sum) {
        uint64_t cut = (ft.frames + 99) / 100, acc = 0, low = 0;   // the slowest 1% of frames
        for (int b = FT_BINS - 1; b >= 0; b--) {
            acc += ft.hist[b];
            if (acc >= cut) { low = (uint64_t)(b + 1) * 250000ull; break; }   // upper edge of the bin
        }
        rt_log("frametime: %s: %llu frames in %.1f s, mean %.1f fps, 1%% low %.1f fps, over 25 ms %llu, 50 ms %llu, 100 ms %llu (%s)",
               tag, (unsigned long long)ft.frames, (now - ft.t0) / 1e9, ft.frames * 1e9 / ft.sum, low ? 1e9 / low : 0.0,
               (unsigned long long)ft.over25, (unsigned long long)ft.over50, (unsigned long long)ft.over100, ft_clock_note());
        for (int i = 0; i < ft.nworst; i++) {
            const FtWorst *w = &ft.worst[i];
            rt_log("frametime:   worst frame %llu: %.1f ms; shader translation %.1f, pipeline build %.1f, texture upload %.1f (astc %.1f), replay %.1f, fence/present wait %.1f, pacing %.1f",
                   (unsigned long long)w->frame, w->ns / 1e6, w->wait[PERF_XLATE] / 1e6, w->wait[PERF_PIPE] / 1e6,
                   w->wait[PERF_TEX] / 1e6, w->wait[PERF_ASTC] / 1e6, w->replay / 1e6,
                   (w->wait[PERF_FENCE] + w->wait[PERF_DRAIN]) / 1e6, w->wait[PERF_PACE] / 1e6);
        }
    }
    memset(&ft, 0, sizeof ft);
    ft.t0 = now;
}

// REKARINA_FRAMETIMES=<file>: every frame's number and intervals in microseconds, kept in memory and written at exit as
// "frame shown_us call_us" lines: the interval to the frame before as shown (the clock below) and between the two
// present calls (what the file held alone before 2026-11-17: take the third field for old comparisons). shown_us is
// 0 where the frame's showing was not seen. The frame-time picture of a run without any other instrument (stores per
// present; the buffer is reserved once and holds about 80 minutes at 120 fps, later frames are not kept).
static struct { uint32_t *v; size_t n, cap; int on; } ftl = {.on = -1};
static int ftl_wanted(void) {
    static int on = -1;
    if (on < 0) on = cfg_get("REKARINA_FRAMETIMES") != NULL;
    return on;
}

static void ftl_write(void) {
    if (ftl.on <= 0 || !ftl.n) return;
    FILE *f = fopen(cfg_get("REKARINA_FRAMETIMES"), "w");
    if (!f) { rt_log("frametime: could not write %s", cfg_get("REKARINA_FRAMETIMES")); return; }
    for (size_t i = 0; i + 2 < ftl.n; i += 3) fprintf(f, "%u %u %u\n", ftl.v[i], ftl.v[i + 1], ftl.v[i + 2]);
    fclose(f);
    rt_log("frametime: %zu frame times written to %s", ftl.n / 3, cfg_get("REKARINA_FRAMETIMES"));
}

__attribute__((weak)) void gfx_exit_log(void) {}

void perf_summary_flush(void) {   // at exit (prof_write)
    if (sec_f) fflush(sec_f);
    if (summary_t0) {
        ft_report(kern_now_ns(), "exit");
        gfx_exit_log();
    }
    summary_t0 = 0;
    ftl_write();
    ftl.on = 0, ftl.n = 0;
    pacelog_write();   // DEBUG-PACELOG (frame_rate.c)
}

// One frame of the summaries and the file: d its interval on the frame clock (shown_ns below), d_call between the present
// calls, rep its replay time and wait its render-thread waits (both of the present call that made the frame).
static void ft_frame(uint64_t frame, uint64_t now, uint64_t d, uint64_t d_call, uint64_t shown, int64_t rep,
                     const uint64_t *wait) {
    if (__builtin_expect(ftl.on != 0, 0)) {
        if (ftl.on < 0) {
            const char *e = cfg_get("REKARINA_FRAMETIMES");
            ftl.cap = (size_t)3 << 20;
            ftl.on = e && *e && (ftl.v = malloc(ftl.cap * sizeof *ftl.v)) != NULL;
        }
        if (ftl.on && d && ftl.n + 3 <= ftl.cap)
            ftl.v[ftl.n++] = (uint32_t)frame, ftl.v[ftl.n++] = (uint32_t)(shown / 1000), ftl.v[ftl.n++] = (uint32_t)(d_call / 1000);
    }
    if (!summary_t0) {   // the first frame starts the period; there is no interval yet
        summary_t0 = now;
        ft.t0 = now;
        return;
    }
    ft.frames++;
    ft.sum += d;
    uint64_t b = d / 250000ull;
    ft.hist[b < FT_BINS ? b : FT_BINS - 1]++;
    if (d > 25000000ull) ft.over25++;
    if (d > 50000000ull) ft.over50++;
    if (d > 100000000ull) ft.over100++;
    if (d > 25000000ull && (ft.nworst < FT_WORST || d > ft.worst[FT_WORST - 1].ns)) {
        int i = ft.nworst < FT_WORST ? ft.nworst++ : FT_WORST - 1;
        while (i > 0 && ft.worst[i - 1].ns < d) ft.worst[i] = ft.worst[i - 1], i--;
        ft.worst[i].frame = frame;
        ft.worst[i].ns = d;
        memcpy(ft.worst[i].wait, wait, sizeof ft.worst[i].wait);
        ft.worst[i].replay = (uint64_t)rep;
    }
    if (now - summary_t0 >= FT_PERIOD_NS) {
        ft_report(now, "60 s");
        summary_t0 = now;
    }
}

// The second line of the 60-frame window: GPU busy time (timestamps), the time threads waited for GPU fences, VRAM and
// process memory, and which side holds the frame back. pace: the frame waited for pacing (vsync, the frame limiter) at
// least 15% of the window; gpu: the GPU was busy for 80% of the frame interval or more (without timestamps: threads
// waited for fences for half the interval or more); cpu otherwise.
// With calibrated timestamps a third line splits the GPU's idle time between timed parts: the part was submitted only
// after the previous one had ended (the GPU waited for the CPU's replay or the game), or it was already queued (untimed
// work such as uploads and presents ran there, or a wait held it).
static void perf_gpu_line(uint64_t frame, double interval_ms, double pace_share, uint64_t n) {
    PerfGpu g;
    memset(&g, 0, sizeof g);
    if (perf_gpu_stats) perf_gpu_stats(&g);
    double rw = plat_xchg64(&render_wait_ns, 0) / 1e6 / n, ow = plat_xchg64(&other_wait_ns, 0) / 1e6 / n;
    double gpu_ms = g.have_gpu ? g.gpu_ns / 1e6 / n : 0;
    char gs[96], vs[96];
    if (g.have_gpu) snprintf(gs, sizeof gs, "gpu avg %.2f ms (%llu parts timed, %llu not)", gpu_ms,
                             (unsigned long long)g.gpu_parts, (unsigned long long)g.gpu_missed);
    else snprintf(gs, sizeof gs, "gpu avg n/a");
    if (g.have_vram) snprintf(vs, sizeof vs, "vram %.0f of %.0f MiB%s", g.vram_used / 1048576.0, g.vram_budget / 1048576.0,
                              g.vram_exact ? "" : " (own allocations of the heap size)");
    else snprintf(vs, sizeof vs, "vram n/a");
    uint64_t res, prv;
    plat_process_memory(&res, &prv);
    double ws = res / 1048576.0, priv = prv / 1048576.0;
    const char *bound = pace_share >= 0.15 ? "pace"
                      : g.have_gpu ? (gpu_ms >= 0.8 * interval_ms ? "gpu" : "cpu")
                      : (rw + ow >= 0.5 * interval_ms ? "gpu" : "cpu");
    rt_log("perf: frame %llu: %s; fence wait per frame: render thread %.2f ms, other threads %.2f ms; %s; ram working set "
           "%.0f MiB, private %.0f MiB; bound: %s",
           (unsigned long long)frame, gs, rw, ow, vs, ws, priv, bound);
    if (g.have_gpu && g.have_idle)
        rt_log("perf: frame %llu: gpu idle per frame %.2f ms between timed parts: %.2f ms waiting for the next submission, "
               "%.2f ms with it queued (untimed work or waits); %.1f gaps over 0.5 ms, longest %.2f ms; submit to start avg %.2f ms",
               (unsigned long long)frame, g.idle_ns / 1e6 / n, g.starved_ns / 1e6 / n, (g.idle_ns - g.starved_ns) / 1e6 / n,
               (double)g.idle_gaps / n, g.max_gap_ns / 1e6, g.gpu_parts ? g.queued_ns / 1e6 / g.gpu_parts : 0.0);
}

int rt_ab(void) {
    static int64_t period = -1;   // us of world clock; 0: off
    if (period < 0) {
        const char *e = cfg_get("REKARINA_AB");
        period = e && *e ? (int64_t)(atof(e) * 1e6) : 0;
    }
    int64_t w = period ? frame_step_world_us() : -1;
    if (w < 0) return 0;   // before the world clock: A
    int64_t k = w / period % 4;
    return k == 1 || k == 2;
}

int rt_knob(const char *name, const char *def) {
    char key[64];
    snprintf(key, sizeof key, "REKARINA_%s", name);
    const char *e = cfg_get(key);
    if (!e || !*e) e = def;
    return !strcmp(e, "ab") ? 2 : strcmp(e, "0") != 0;
}

// REKARINA_ONE_PER_CORE=1: the process keeps the first logical processor of each physical core, so busy threads (the
// render thread, the guest cores, the job workers) never share a core's execution units with another; "ab" switches
// it with the A/B blocks. Off by default until measured here; Savage Wind Recomp
// reported +4-10% from the same rule on an 8-core SMT CPU.
static void cpu_placement(void) {
    static int mode = -1, applied = -1;
    static uint64_t all, one;
    if (mode < 0) {
        mode = rt_knob("ONE_PER_CORE", "0");
        int cores = plat_cpu_cores(&all, &one);
        if (mode && one && one != all)
            rt_log("cpu: %d cores, %d logical processors: one per core%s", cores, __builtin_popcountll(all),
                   mode == 2 ? " in the B blocks" : "");
        else
            mode = 0;
    }
    int on = RT_KNOB_ON(mode);
    if (on == applied) return;
    applied = on;
    plat_process_affinity(on ? one : all);
}

// DEBUG-HEAPCHECK: REKARINA_HEAP_CHECK=<n> checks the process heap every n presents (HeapValidate). A run whose heap
// broke names the first check that found it broken and the last that found it whole: the writer ran in between. Also
// the end of the last entry a walk read before the break, and the bytes around it (what the writer left there). Off
// by default: a check walks every block under the heap's lock (milliseconds).
static struct { const char *what; uintptr_t p; size_t n; } hnote[32];
void rt_heap_note(const char *what, const void *p, size_t bytes) {
    for (int i = 0; i < 32; i++)
        if (hnote[i].what == what || !hnote[i].what) {
            hnote[i].p = (uintptr_t)p, hnote[i].n = bytes;
            __atomic_store_n(&hnote[i].what, what, __ATOMIC_RELEASE);
            return;
        }
}

static void heap_check(uint64_t frame) {
    static int every = -1, broken;
    static uint64_t ok_at, ns_sum, ns_max, nchecks;
    if (every < 0) {
        const char *e = cfg_get("REKARINA_HEAP_CHECK");   // a test setting: read raw, no table row needed
        every = e && *e ? atoi(e) : 0;
        if (every < 0) every = 0;
        if (every) rt_log("heap: the process heap is checked every %d presents (REKARINA_HEAP_CHECK)", every);
    }
    if (!every || broken || frame % (uint64_t)every) return;
    uint64_t t0 = plat_now_ns(), at, n;
    int ok = plat_heap_check(&at, &n);
    uint64_t dt = plat_now_ns() - t0;
    ns_sum += dt, nchecks++;
    if (dt > ns_max) ns_max = dt;
    if (ok) {
        ok_at = frame;
        if (!(nchecks & 63))
            rt_log("heap: frame %llu: whole (%llu checks, %.2f ms each, %.2f at most)", (unsigned long long)frame,
                   (unsigned long long)nchecks, ns_sum / 1e6 / (double)nchecks, ns_max / 1e6);
        return;
    }
    broken = 1;
    rt_log("heap: BROKEN at frame %llu, whole at frame %llu; a walk stopped after %llu entries at %#llx",
           (unsigned long long)frame, (unsigned long long)ok_at, (unsigned long long)n, (unsigned long long)at);
    for (int i = 0; i < 32 && hnote[i].what; i++) {   // the suspects' arrays near that place
        uintptr_t lo = hnote[i].p, hi = lo + hnote[i].n;
        if (at + 4096 >= lo && at <= hi + 4096)
            rt_log("heap: near it: %s at %#llx+%#llx", hnote[i].what, (unsigned long long)lo, (unsigned long long)hnote[i].n);
    }
    if (at) {   // the bytes before and after the place the walk stopped (the overrun's tail and the header it broke)
        const unsigned char *p = (const unsigned char *)(uintptr_t)(at - 64);
        for (int r = 0; r < 8; r++) {
            char line[160];
            int o = snprintf(line, sizeof line, "heap: %#llx:", (unsigned long long)(at - 64 + (uint64_t)r * 16));
            for (int k = 0; k < 16; k++) o += snprintf(line + o, sizeof line - (size_t)o, " %02x", p[r * 16 + k]);
            rt_log("%s", line);
        }
    }
}

// ---- Long frames, always on (REKARINA_LONGFRAME=0: off). A frame of LF_MS or more while a scene is on screen (not
// behind a loading screen, and not in the second after one: the first frames of a map are its loading's tail) gets a
// line of its own: its length, the render thread's waits, the replay's time and its three most expensive sections of
// that frame (the section timers above run in every game for this), and what the back end did in it: images made,
// textures uploaded and their MiB, released images adopted, entries dropped under pressure, textures evicted, pool
// slots resolved and put on the stand-in, pipelines built, the slowest vkAllocateMemory. The cause named first is the
// one that took the largest share of the frame: a wait of the render thread (pipeline build, shader translation,
// texture upload or decode, the GPU, pacing) when it took LF_SHARE of the frame or more, else the replay when it did,
// else "game": the guest delivered the frame late (its own CPU work, or a wait the runtime does not time). At most
// LF_LINES a minute, then a count; a line every minute with long frames sums them by cause. perf_last_long hands the
// last one to the performance overlay (perfmon.h).
#define LF_NS 25000000ull
#define LF_SHARE 0.4
#define LF_LINES 20
#define LF_QUIET_NS 1000000000ull
static const char *const lf_cause_name[PERF_LONG_N] = {"game", "replay", "gpu", "pipeline build", "shader translation",
                                                       "texture upload", "texture decode", "pacing"};
static volatile int64_t lf_cnt[LF_N];
void lf_count(int kind, int64_t n) { __atomic_fetch_add(&lf_cnt[kind], n, __ATOMIC_RELAXED); }
__attribute__((weak)) int perf_scene_frames_ago(void) { return 0; }
__attribute__((weak)) int vk_arrive_held(uint64_t frame) { (void)frame; return 0; }
static struct { uint64_t frame; float ms; int cause; } lf_last;
static PlatMutex lf_last_lock = PLAT_MUTEX_INIT;
void perf_last_long(uint64_t *frame, float *ms, int *cause) {
    plat_mutex_lock(&lf_last_lock);
    *frame = lf_last.frame, *ms = lf_last.ms, *cause = lf_last.cause;
    plat_mutex_unlock(&lf_last_lock);
}

// ---- Arrivals: the ramp-up after a loading screen. From the first scene frame after one until the frame time has
// stayed within AR_BAND of its settled value for a second (AR_BUCKETS buckets of AR_BUCKET_NS whose mean frame times
// all lie within AR_BAND of their median; at most AR_CAP_NS), one line: how long the ramp-up took, the milliseconds
// over the settled frame time, the frames over 25 ms, and what the back end did meanwhile, by share (the render
// thread's waits, the replay and its most expensive sections, images made, uploads, pool slots, pipelines), with the
// map's name. The player sees this as the 2-3 s it takes a map to reach its frame rate.
#define AR_BUCKET_NS 100000000ull
#define AR_BUCKETS 10
#define AR_BAND 0.2
#define AR_CAP_NS 10000000000ull
#define AR_MAXF 8192
static struct {
    int on, loading;
    uint64_t f0, t0, bt0, bn, nb;
    double bsum, bmean[AR_BUCKETS];
    uint32_t bfirst[AR_BUCKETS];                 // frame index (into ms[]) of each kept bucket's first frame
    uint64_t bstart[AR_BUCKETS];                 // and its time
    uint32_t nf, over25;
    float ms[AR_MAXF];
    uint64_t w[PERF_N], rep;
    int64_t sec[SEC_N], c[PERF_CNT_N], l[LF_N];
} ar;

static int ar_cmp(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y); }

static void ar_report(uint64_t now, int settled, double settled_ms, uint32_t end_i, uint64_t end_t, const char *why) {
    double excess = 0;
    uint32_t over = 0;
    for (uint32_t i = 0; i < end_i && i < ar.nf; i++) {
        if (ar.ms[i] > settled_ms) excess += ar.ms[i] - settled_ms;
        over += ar.ms[i] > 25.0f;
    }
    int top[3] = {-1, -1, -1};
    for (int i = 0; i < SEC_N; i++) {
        if (i == SEC_SUB_TOTAL || i == SEC_ROW || ar.sec[i] <= 0) continue;
        for (int j = 0; j < 3; j++)
            if (top[j] < 0 || ar.sec[i] > ar.sec[top[j]]) {
                for (int m = 2; m > j; m--) top[m] = top[m - 1];
                top[j] = i;
                break;
            }
    }
    char secs[200];
    int o = 0;
    for (int j = 0; j < 3 && top[j] >= 0; j++)
        o += snprintf(secs + o, sizeof secs - (size_t)o, "%s%s %.0f ms", j ? ", " : "", sec_names[top[j]], sec_ticks_ms((uint64_t)ar.sec[top[j]]));
    double span = (now - ar.t0) / 1e6;
    rt_log("arrival: map %s from frame %llu: %s %.2f s (settled at %.1f ms a frame, %.0f fps), %.0f ms over it in %u frames, "
           "%u frames over 25 ms (%u in the whole %.1f s watched); in those %.1f s: waits pipeline %.0f ms, shader %.0f, "
           "texture %.0f (decode %.0f), gpu %.0f; replay %.0f ms, top sections: %s; images made %lld, textures uploaded %lld "
           "(%.0f MiB), pool slots resolved %lld / on the stand-in %lld, pipelines built %lld, draws that waited for a pipeline "
           "%lld, evicted %lld, dropped %lld",
           prep_map_now(), (unsigned long long)ar.f0, settled ? "ramp-up" : why, (end_t - ar.t0) / 1e9, settled_ms,
           settled_ms > 0 ? 1000.0 / settled_ms : 0.0, excess, end_i, over, ar.over25, span / 1e3, span / 1e3,
           ar.w[PERF_PIPE] / 1e6, ar.w[PERF_XLATE] / 1e6, ar.w[PERF_TEX] / 1e6, ar.w[PERF_ASTC] / 1e6,
           (ar.w[PERF_FENCE] + ar.w[PERF_DRAIN]) / 1e6, ar.rep / 1e6, o ? secs : "none", (long long)ar.c[PERF_CNT_TEX_NEW],
           (long long)(ar.c[PERF_CNT_TEX_UP] + ar.c[PERF_CNT_TEX_REUP]), ar.l[LF_UP_BYTES] / 1048576.0,
           (long long)ar.l[LF_POOL_RES], (long long)ar.l[LF_POOL_STANDIN], (long long)ar.c[PERF_CNT_PIPE_BUILT],
           (long long)(ar.c[PERF_CNT_PIPE_WAIT] + ar.c[PERF_CNT_ONESHOT_WAIT]), (long long)ar.l[LF_EVICT], (long long)ar.l[LF_DROP]);
    ar.on = 0;
}

// Per frame (lf_frame): scene says whether a scene was drawn; dc / dl the frame's counts.
static void ar_frame(uint64_t frame, uint64_t now, uint64_t d, uint64_t rep, const uint64_t *w, int scene, const int64_t *dc,
                     const int64_t *dl, const int64_t *fr_ticks) {
    if (!scene) {
        if (ar.on) ar_report(now, 0, 0, ar.nf, now, "cut short by another loading screen after");
        ar.loading = 1;
        return;
    }
    if (ar.loading && !ar.on) {   // the first scene frame after a loading screen
        memset(&ar, 0, sizeof ar);
        ar.on = 1, ar.f0 = frame, ar.t0 = ar.bt0 = now;
        return;
    }
    if (!ar.on || !d) return;
    if (ar.nf < AR_MAXF) ar.ms[ar.nf++] = (float)(d / 1e6);
    ar.over25 += d > 25000000ull;
    for (int k = 0; k < PERF_N; k++) ar.w[k] += w[k];
    ar.rep += rep;
    for (int i = 0; i < SEC_N; i++) ar.sec[i] += fr_ticks[i];
    for (int i = 0; i < PERF_CNT_N; i++) ar.c[i] += dc[i];
    for (int i = 0; i < LF_N; i++) ar.l[i] += dl[i];
    ar.bsum += d / 1e6, ar.bn++;
    if (now - ar.bt0 >= AR_BUCKET_NS) {   // a bucket is full: the last AR_BUCKETS of them decide
        if (ar.nb == AR_BUCKETS) {
            memmove(ar.bmean, ar.bmean + 1, sizeof ar.bmean[0] * (AR_BUCKETS - 1));
            memmove(ar.bfirst, ar.bfirst + 1, sizeof ar.bfirst[0] * (AR_BUCKETS - 1));
            memmove(ar.bstart, ar.bstart + 1, sizeof ar.bstart[0] * (AR_BUCKETS - 1));
            ar.nb--;
        }
        ar.bmean[ar.nb] = ar.bsum / (double)ar.bn;
        ar.bfirst[ar.nb] = ar.nf >= ar.bn ? (uint32_t)(ar.nf - ar.bn) : 0;
        ar.bstart[ar.nb] = ar.bt0;
        ar.nb++;
        ar.bsum = 0, ar.bn = 0, ar.bt0 = now;
        if (ar.nb == AR_BUCKETS) {
            double s[AR_BUCKETS];
            memcpy(s, ar.bmean, sizeof s);
            qsort(s, AR_BUCKETS, sizeof s[0], ar_cmp);
            double m = (s[AR_BUCKETS / 2 - 1] + s[AR_BUCKETS / 2]) / 2;
            int in = 1;
            for (int i = 0; i < AR_BUCKETS; i++) in &= fabs(ar.bmean[i] - m) <= AR_BAND * m;
            if (in) { ar_report(now, 1, m, ar.bfirst[0], ar.bstart[0], ""); return; }
            if (now - ar.t0 >= AR_CAP_NS) { ar_report(now, 0, m, ar.nf, now, "not settled within"); return; }
        }
    }
}

// The frame clock (below) knows a frame's length two presents later, when the GPU has shown it. So each present call
// keeps what its frame did (lf_capture: the waits, the replay, the sections, the counts, whether a scene was drawn), and
// lf_frame judges the frame the summaries count now, with the length they count (perf_present): the long-frame lines,
// the minute's sums and the arrival lines agree with the overlay and the "frametime:" summaries.
#define LF_RING 4
static struct {
    uint64_t frame, now, rep, w[PERF_N];
    int64_t dc[PERF_CNT_N], dl[LF_N], at, asz, ticks[SEC_N], cnts[SEC_N];
    int scene;
} lfr[LF_RING];
static void lf_capture(uint64_t frame, uint64_t now, uint64_t rep, const uint64_t *w) {
    static int64_t prev_c[PERF_CNT_N], prev_l[LF_N];
    typeof(lfr[0]) *e = &lfr[frame % LF_RING];
    e->frame = frame, e->now = now, e->rep = rep;
    memcpy(e->w, w, sizeof e->w);
    for (int i = 0; i < PERF_CNT_N; i++) {
        int64_t c = __atomic_load_n(&counts[i], __ATOMIC_RELAXED);
        e->dc[i] = c - prev_c[i], prev_c[i] = c;
    }
    for (int i = 0; i < LF_N; i++) {
        int64_t l = __atomic_load_n(&lf_cnt[i], __ATOMIC_RELAXED);
        e->dl[i] = l - prev_l[i], prev_l[i] = l;
    }
    e->at = plat_xchg64(&lf_alloc_ticks, 0), e->asz = plat_xchg64(&lf_alloc_size, 0);
    memcpy(e->ticks, fr_ticks, sizeof e->ticks), memcpy(e->cnts, fr_counts, sizeof e->cnts);
    e->scene = perf_scene_frames_ago() <= 2 && !vk_arrive_held(frame);   // a held frame counts as the loading screen
}

static void lf_frame(uint64_t frame, uint64_t d) {
    const typeof(lfr[0]) *e = &lfr[frame % LF_RING];
    if (e->frame != frame) return;   // not captured (the report was off then)
    static uint64_t quiet_until, min_t0, n_long, n_50, n_cause[PERF_LONG_N], n_top[SEC_N], lines, left_out;
    uint64_t now = e->now, rep = e->rep;
    const uint64_t *w = e->w;
    const int64_t *dc = e->dc, *dl = e->dl, *fr_ticks = e->ticks, *fr_counts = e->cnts;
    int64_t at = e->at, asz = e->asz;
    if (!min_t0) min_t0 = now;
    static uint64_t sec_n, sec_frames, minutes;   // what the timers cost: sections timed per frame
    for (int i = 0; i < SEC_N; i++) sec_n += fr_ticks[i] > 0 ? (uint64_t)fr_counts[i] : 0;
    sec_frames++;
    if (now - min_t0 >= 60000000000ull) {   // the minute's sum
        if (n_long) {
            char by[400];
            int o = 0;
            for (int k = 0; k < PERF_LONG_N; k++)
                if (n_cause[k]) o += snprintf(by + o, sizeof by - (size_t)o, "%s%s %llu", o ? ", " : "", lf_cause_name[k], (unsigned long long)n_cause[k]);
            int top = -1;
            for (int i = 0; i < SEC_N; i++)
                if (n_top[i] && (top < 0 || n_top[i] > n_top[top])) top = i;
            rt_log("longframe: the last minute: %llu frames of %llu ms or more in play (%llu of 50 ms or more); by cause: %s%s%s; "
                   "%llu lines left out",
                   (unsigned long long)n_long, LF_NS / 1000000ull, (unsigned long long)n_50, by,
                   top >= 0 ? "; the replay's most frequent top section " : "", top >= 0 ? sec_names[top] : "",
                   (unsigned long long)left_out);
        }
        if (!(minutes++ % 10) && sec_frames)
            rt_log("longframe: %.0f sections timed a frame, about %.3f ms a frame at %.1f ns each", (double)sec_n / sec_frames,
                   (double)sec_n / sec_frames * sec_cost_ns / 1e6, sec_cost_ns);
        sec_n = sec_frames = 0;
        min_t0 = now, n_long = n_50 = lines = left_out = 0;
        memset(n_cause, 0, sizeof n_cause), memset(n_top, 0, sizeof n_top);
    }
    int scene = e->scene;
    ar_frame(frame, now, d, rep, w, scene, dc, dl, fr_ticks);
    if (!scene) {   // no scene drawn: a loading screen (or the game's own black frames)
        quiet_until = now + LF_QUIET_NS;
        return;
    }
    if (d < LF_NS || now < quiet_until || !d) return;
    // the dominant cause
    uint64_t ws[PERF_LONG_N] = {0};
    ws[PERF_LONG_PIPELINE] = w[PERF_PIPE], ws[PERF_LONG_SHADER] = w[PERF_XLATE];
    ws[PERF_LONG_DECODE] = w[PERF_ASTC], ws[PERF_LONG_TEXTURE] = w[PERF_TEX] > w[PERF_ASTC] ? w[PERF_TEX] - w[PERF_ASTC] : 0;
    ws[PERF_LONG_GPU] = w[PERF_FENCE] + w[PERF_DRAIN], ws[PERF_LONG_PACING] = w[PERF_PACE];
    int cause = PERF_LONG_GAME;
    uint64_t best = 0;
    for (int k = PERF_LONG_GPU; k < PERF_LONG_N; k++)
        if (ws[k] > best) best = ws[k], cause = k;
    if ((double)best < LF_SHARE * (double)d) cause = (double)rep >= LF_SHARE * (double)d ? PERF_LONG_REPLAY : PERF_LONG_GAME;
    // the frame's three most expensive sections (SUB_TOTAL is the sum of the SUB_ partition: left out)
    int top[3] = {-1, -1, -1};
    for (int i = 0; i < SEC_N; i++) {
        if (i == SEC_SUB_TOTAL || i == SEC_ROW || fr_ticks[i] <= 0) continue;
        for (int j = 0; j < 3; j++)
            if (top[j] < 0 || fr_ticks[i] > fr_ticks[top[j]]) {
                for (int m = 2; m > j; m--) top[m] = top[m - 1];
                top[j] = i;
                break;
            }
    }
    n_long++, n_50 += d >= 50000000ull, n_cause[cause]++;
    if (cause == PERF_LONG_REPLAY && top[0] >= 0) n_top[top[0]]++;
    plat_mutex_lock(&lf_last_lock);
    lf_last.frame = frame, lf_last.ms = (float)(d / 1e6), lf_last.cause = cause;
    plat_mutex_unlock(&lf_last_lock);
    if (lines >= LF_LINES) { left_out++; return; }
    lines++;
    char secs[200];
    int o = 0;
    for (int j = 0; j < 3 && top[j] >= 0; j++)
        o += snprintf(secs + o, sizeof secs - (size_t)o, "%s%s %.1f ms (%lld)", j ? ", " : "", sec_names[top[j]],
                      sec_ticks_ms((uint64_t)fr_ticks[top[j]]), (long long)fr_counts[top[j]]);
    rt_log("longframe: frame %llu: %.1f ms, cause %s; waits: pipeline %.1f, shader %.1f, texture %.1f (decode %.1f), gpu %.1f, "
           "pacing %.1f; replay %.1f ms, top sections: %s; images made %lld, textures uploaded %lld (%.1f MiB), adopted %lld, "
           "dropped %lld, evicted %lld (%.1f MiB), pool slots resolved %lld / on the stand-in %lld, pipelines built %lld, "
           "slowest vkAllocateMemory %.1f ms (%.0f MiB)",
           (unsigned long long)frame, d / 1e6, lf_cause_name[cause], w[PERF_PIPE] / 1e6, w[PERF_XLATE] / 1e6, w[PERF_TEX] / 1e6,
           w[PERF_ASTC] / 1e6, (w[PERF_FENCE] + w[PERF_DRAIN]) / 1e6, w[PERF_PACE] / 1e6, rep / 1e6, o ? secs : "none",
           (long long)dc[PERF_CNT_TEX_NEW], (long long)(dc[PERF_CNT_TEX_UP] + dc[PERF_CNT_TEX_REUP]), dl[LF_UP_BYTES] / 1048576.0,
           (long long)dl[LF_ADOPT], (long long)dl[LF_DROP], (long long)dl[LF_EVICT], dl[LF_EVICT_BYTES] / 1048576.0,
           (long long)dl[LF_POOL_RES], (long long)dl[LF_POOL_STANDIN], (long long)dc[PERF_CNT_PIPE_BUILT],
           at ? sec_ticks_ms((uint64_t)at) : 0.0, asz / 1048576.0);
}

// ---- the frame clock (REKARINA_FRAME_CLOCK: shown, the default, or call)
// The overlay's frame rate, lows and graph, the "frametime:" summaries and the frame-time file count a frame's interval
// where the frame is SHOWN: the GPU's completion of its blit into the swap chain image (vk_backend.c present_main, on the
// completion thread), the earliest moment the display can show it. Until 2026-11-17 they counted the interval between
// the game's present CALLS. In a GPU-bound view those wobble around the GPU's cadence (the present waits for its frame's
// results point and that wait absorbs the GPU's lead: lag-1 correlation -0.2 to -0.4) while the frames come out evenly:
// the still meadow read p99/median 1.14-1.34 on the calls and 1.04-1.07 on the GPU's completions, so the overlay's
// 1 % low read 75-85 fps for a picture that held 100. A present call counts the frame two presents before it, whose
// completion is known by then (the present waits for its own frame's results point, which the GPU reaches after the blit
// of the frame before last); a frame whose completion was not seen counts with its call interval. With frame generation
// the interval is between two of the game's own frames (the overlay's large number counts the display's frames,
// generated ones included, from gfx_output_fps). The game's step (frame_rate.c) keeps the present calls.
#define SH_N 256
static struct { volatile uint64_t frame, ns; } sh[SH_N];   // completion thread: when the frame was shown
static struct { uint64_t frame, d_call; int64_t rep; uint64_t wait[PERF_N]; } shq[SH_N];   // render thread: its present call
static int sh_mode = -1;
static uint64_t sh_used, sh_fallback;   // frames counted on the shown clock / with their call interval instead

void perf_shown(uint64_t frame, uint64_t ns) {
    uint32_t i = (uint32_t)(frame % SH_N);
    __atomic_store_n(&sh[i].ns, ns, __ATOMIC_RELAXED);
    __atomic_store_n(&sh[i].frame, frame, __ATOMIC_RELEASE);
}

static uint64_t shown_at(uint64_t frame) {
    uint32_t i = (uint32_t)(frame % SH_N);
    return __atomic_load_n(&sh[i].frame, __ATOMIC_ACQUIRE) == frame ? __atomic_load_n(&sh[i].ns, __ATOMIC_RELAXED) : 0;
}

static const char *ft_clock_note(void) {
    static char s[96];
    if (sh_mode <= 0) return "present calls";
    snprintf(s, sizeof s, "shown frames; %llu of %llu counted on their calls", (unsigned long long)sh_fallback,
             (unsigned long long)(sh_used + sh_fallback));
    sh_used = sh_fallback = 0;
    return s;
}

static int frame_clock_shown(void) {
    if (sh_mode < 0) {
        const char *e = cfg_get("REKARINA_FRAME_CLOCK");
        sh_mode = !(e && !strcmp(e, "call"));
        rt_log("frametime: frame times %s (REKARINA_FRAME_CLOCK)",
               sh_mode ? "where the frame is shown (the GPU's completion of its blit: its timestamp, or with =seen when the "
                         "completion thread saw it)" : "between the game's present calls");
    }
    return sh_mode;
}

void perf_present(uint64_t frame) {
    heap_check(frame);   // DEBUG-HEAPCHECK
    static uint64_t last, sum, max, n, win_start, ab_b;
    static int64_t win_prev[PERF_CNT_N];
    prof_render_tid = plat_thread_id();
    if (rt_trace & RT_TRACE_SCHED) kern_sched_report(frame);
    uint64_t now = kern_now_ns(), d = last ? now - last : 0;
    prof_sampler_frame(frame, now, last ? d : 0);
    int64_t rep = plat_xchg64(&frame_replay_ns, 0);
    {   // the frame clock: this call is kept, the frame three presents back is counted (two missed 5-13 % of the frames'
        // shown times in the forest and the tour once the present is held after its call, nvn_api_obj.c hold_until)
        typeof(shq[0]) *q = &shq[frame % SH_N];
        q->frame = frame, q->d_call = d, q->rep = rep;
        memcpy(q->wait, frame_ns, sizeof q->wait);
    }
    uint64_t kf = frame > 3 ? frame - 3 : 0, k_used = 0;   // the frame counted now, and its interval
    const typeof(shq[0]) *kq = kf ? &shq[kf % SH_N] : NULL;
    if (kq && kq->frame == kf) {
        uint64_t a = shown_at(kf), p = shown_at(kf - 1), ds = a && p && a > p ? a - p : 0;
        int shown = frame_clock_shown() && ds;
        if (frame_clock_shown()) { if (shown) sh_used++; else sh_fallback++; }
        k_used = shown ? ds : kq->d_call;
        // a frame vk_arrive.c held unshown is no frame the player saw: the summaries leave it out (the hold's own line
        // reports how long the loading screen stayed up for it)
        if (!vk_arrive_held(kf)) ft_frame(kf, now, k_used, kq->d_call, ds, kq->rep, kq->wait);   // a 0 interval (the first) only starts the period
    } else {
        kq = NULL;
    }
    if (__builtin_expect(sec_on, 0)) {
        uint64_t s0 = sec_clock();
        sec_frame(frame, d, rep, frame_ns);
        if (lf_on > 0) {
            lf_capture(frame, now, rep, frame_ns);
            if (kq) lf_frame(kf, k_used);   // the frame the summaries count now, on their clock
        }
        sec_add(SEC_ROW, sec_clock() - s0);   // the row's own cost lands in the next row
    }
    ab_b += rt_ab();
    cpu_placement();
    {   // REKARINA_RENDER_CORE=1: the presenting thread, which replays the frame, gets a physical core of its own (the
        // game's twelve busy threads otherwise put one beside it on the core's other logical processor)
        static int done;
        if (!done) {
            done = 1;
            if (rt_knob("RENDER_CORE", "0")) {
                int c = plat_thread_own_core();
                rt_log(c >= 0 ? "cpu: the render thread has core %d to itself (REKARINA_RENDER_CORE)"
                              : "cpu: no core of its own for the render thread (%d)", c);
            }
        }
    }
    if (__atomic_load_n(&overlay, __ATOMIC_RELAXED) && kq)   // the overlay's only cost while it is off: this branch
        perfmon_frame(now, k_used, kq->wait, (uint64_t)kq->rep, __atomic_load_n(&counts[PERF_CNT_TEX_UP], __ATOMIC_RELAXED));
    kern_core_diag_frame(frame, d);
    if (flight_on) flight_frame(frame, now, d, frame_ns, counts);
    if (!(rt_trace & RT_TRACE_PERF)) {
        memset(frame_ns, 0, sizeof frame_ns);
        last = now;
        return;
    }
    static uint64_t slow_ns;   // REKARINA_PERF_SLOW_MS (default 50): frames at least this long get a line of their own
    if (!slow_ns) {
        const char *e = cfg_get("REKARINA_PERF_SLOW_MS");
        slow_ns = (uint64_t)(e && atoi(e) > 0 ? atoi(e) : 50) * 1000000ull;
    }
    if (last) {
        sum += d;
        n++;
        if (d > max) max = d, memcpy(worst_ns, frame_ns, sizeof worst_ns);
        if (d >= slow_ns)
            rt_log("perf: slow frame %llu: %.1f ms; blocked pipeline %.1f xlate %.1f texture %.1f astc %.1f fence %.1f pacing %.1f",
                   (unsigned long long)frame, d / 1e6, frame_ns[PERF_PIPE] / 1e6, frame_ns[PERF_XLATE] / 1e6,
                   frame_ns[PERF_TEX] / 1e6, frame_ns[PERF_ASTC] / 1e6, frame_ns[PERF_FENCE] / 1e6,
                   frame_ns[PERF_PACE] / 1e6);
    }
    memset(frame_ns, 0, sizeof frame_ns);
    last = now;
    if (!win_start) win_start = now;
    if (frame % 60) return;
    uint64_t span = now - win_start;
    int64_t win[PERF_CNT_N];
    for (int i = 0; i < PERF_CNT_N; i++) {
        int64_t v = __atomic_load_n(&counts[i], __ATOMIC_RELAXED);
        win[i] = v - win_prev[i];
        win_prev[i] = v;
    }
    int64_t async = win[PERF_CNT_PIPE_ASYNC], skips = win[PERF_CNT_PIPE_SKIP], up = win[PERF_CNT_TEX_UP];
    int64_t reup = win[PERF_CNT_TEX_REUP], sskip = win[PERF_CNT_SHADER_SKIP], waited = win[PERF_CNT_PIPE_WAIT];
    int64_t oneshot = win[PERF_CNT_ONESHOT_WAIT];
    uint64_t shader_keys, shader_xlated;
    gfx_shader_counts(&shader_keys, &shader_xlated);
    if (n && span)
        rt_log("perf: frame %llu: interval avg %.2f ms max %.2f ms (%.1f fps); render thread blocked: pacing %.0f%% fence %.0f%% drain %.0f%% pipeline build %.0f%% shader xlate %.0f%% texture %.0f%% (astc %.0f%%); pipelines built in background %lld, draws skipped pending %lld, waited %lld, waiting for a shader %lld, one-shot waited %lld; textures uploaded %lld re-uploaded %lld; slowest frame: xlate %.1f ms texture %.1f ms (astc %.1f) pipeline %.1f ms fence %.1f ms; shaders: %llu keys, %llu translated; render busy avg %.2f ms; replay avg %.2f ms; ab %llu/%llu; world %.1f s",
               (unsigned long long)frame, sum / 1e6 / n, max / 1e6, n * 1e9 / sum, 100.0 * wait_ns[PERF_PACE] / span,
               100.0 * wait_ns[PERF_FENCE] / span, 100.0 * wait_ns[PERF_DRAIN] / span,
               100.0 * wait_ns[PERF_PIPE] / span, 100.0 * wait_ns[PERF_XLATE] / span,
               100.0 * wait_ns[PERF_TEX] / span, 100.0 * wait_ns[PERF_ASTC] / span, (long long)async, (long long)skips, (long long)waited, (long long)sskip, (long long)oneshot,
               (long long)up, (long long)reup, worst_ns[PERF_XLATE] / 1e6, worst_ns[PERF_TEX] / 1e6,
               worst_ns[PERF_ASTC] / 1e6, worst_ns[PERF_PIPE] / 1e6, worst_ns[PERF_FENCE] / 1e6,
               (unsigned long long)shader_keys, (unsigned long long)shader_xlated,
               (span > wait_ns[PERF_PACE] ? span - wait_ns[PERF_PACE] : 0) / 1e6 / n,
               plat_xchg64(&replay_ns, 0) / 1e6 / n, (unsigned long long)ab_b, (unsigned long long)n, frame_step_world_us() / 1e6);
    if (n && span) perf_gpu_line(frame, sum / 1e6 / n, (double)wait_ns[PERF_PACE] / span, n);
    sum = max = n = ab_b = 0;
    memset(wait_ns, 0, sizeof wait_ns);
    win_start = now;
}
