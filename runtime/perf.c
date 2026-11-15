// Frame timing (REKARINA_TRACE=perf, the frame-time summary, the performance overlay's counts), the A/B switches (rt_ab,
// rt_knob) and REKARINA_ONE_PER_CORE. The sampling profiler (REKARINA_PROFILE) is the OS's (prof_win.c); without one the weak
// prof_sampler_* below do nothing.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "flight.h"
#include "gfx.h"
#include "host.h"
#include "perfmon.h"
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

void sec_init(void) {
    static int done;
    if (done) return;
    done = 1;
    const char *e = cfg_get("REKARINA_SECTIONS");
    if (!e || !*e) return;
    sec_f = fopen(e, "w");
    if (!sec_f) { rt_log("sec: REKARINA_SECTIONS: cannot write %s", e); return; }
    setvbuf(sec_f, NULL, _IOFBF, 1 << 20);
    uint64_t c0 = sec_clock(), n0 = plat_now_ns(), n1;
    while ((n1 = plat_now_ns()) - n0 < 20000000) {}   // 20 ms against the ns clock
    sec_tick_ns = (double)(n1 - n0) / (double)(sec_clock() - c0);
    fprintf(sec_f, "frame,interval_us,replay_us,gpu_busy_us,pace_us,fence_us,drain_us,pipe_us,xlate_us,tex_us,astc_us");
    for (int i = 0; i < SEC_N; i++) fprintf(sec_f, ",%s_us,%s_n,%s_max", sec_names[i], sec_names[i], sec_names[i]);
    fprintf(sec_f, "\n");
    // What one section costs on this machine, for the overhead the rows carry: a million sections on the real path.
    sec_on = 1;
    uint64_t t0 = sec_clock();
    for (int i = 0; i < 1000000; i++) sec_end(SEC_ROW, sec_begin());
    double one = sec_ticks_ms(sec_clock() - t0);
    sec_on = 0, sec_ticks[SEC_ROW] = sec_counts[SEC_ROW] = sec_peaks[SEC_ROW] = 0;
    rt_log("sec: section timers on, %.3f GHz cycle clock, rows to %s, a section costs %.1f ns", 1.0 / sec_tick_ns, e, one);
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
void sec_alloc_note(uint64_t size, uint32_t type, uint64_t ticks) {
    uint32_t tid = plat_thread_id();
    int replay = tid == prof_render_tid || tid == prof_replay_tid;
    sec_add(replay ? SEC_VKALLOC_R : SEC_VKALLOC_O, ticks);
    if (size >= 32ull << 20)
        rt_log("sec: vkAllocateMemory frame %llu: %s thread %u, %.0f MiB type %u: %.2f ms",
               (unsigned long long)sec_frame_now, replay ? "the replay" : "another", tid,
               size / 1048576.0, type, sec_ticks_ms(ticks));
}

static void sec_frame(uint64_t frame, uint64_t d, uint64_t rep, const uint64_t *waits) {
    sec_frame_now = frame + 1;
    static int64_t pk_prev[6], gpu_prev, accum[SEC_N], total_reg;
    int64_t pk[6] = {0};
    rec_sec_packets(pk);
    for (int i = 0; i < 6; i++) sec_counts[SEC_PK_STATE + i] += pk[i] - pk_prev[i], pk_prev[i] = pk[i];
    int64_t gpu = (int64_t)perf_gpu_busy_total(), gd = gpu - gpu_prev;
    gpu_prev = gpu;
    fprintf(sec_f, "%llu,%.1f,%.1f,%.1f", (unsigned long long)frame, d / 1e3, rep / 1e3, gd / 1e3);
    for (int k = 0; k < 7; k++) fprintf(sec_f, ",%.1f", waits[k] / 1e3);
    for (int i = 0; i < SEC_N; i++) {
        int64_t t = plat_xchg64(&sec_ticks[i], 0), n = plat_xchg64(&sec_counts[i], 0), m = plat_xchg64(&sec_peaks[i], 0);
        accum[i] += n;
        fprintf(sec_f, ",%.1f,%lld,%.1f", (double)t * sec_tick_ns / 1e3, (long long)n, (double)m * sec_tick_ns / 1e3);
    }
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

void perf_count(int kind) {
    if (__atomic_load_n(&perf_on, __ATOMIC_RELAXED)) plat_inc64(&counts[kind]);
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

static void ft_report(uint64_t now, const char *tag) {
    if (ft.frames && ft.sum) {
        uint64_t cut = (ft.frames + 99) / 100, acc = 0, low = 0;   // the slowest 1% of frames
        for (int b = FT_BINS - 1; b >= 0; b--) {
            acc += ft.hist[b];
            if (acc >= cut) { low = (uint64_t)(b + 1) * 250000ull; break; }   // upper edge of the bin
        }
        rt_log("frametime: %s: %llu frames in %.1f s, mean %.1f fps, 1%% low %.1f fps, over 25 ms %llu, 50 ms %llu, 100 ms %llu",
               tag, (unsigned long long)ft.frames, (now - ft.t0) / 1e9, ft.frames * 1e9 / ft.sum, low ? 1e9 / low : 0.0,
               (unsigned long long)ft.over25, (unsigned long long)ft.over50, (unsigned long long)ft.over100);
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

__attribute__((weak)) void gfx_exit_log(void) {}

void perf_summary_flush(void) {   // at exit (prof_write)
    if (sec_f) fflush(sec_f);
    if (summary_t0) {
        ft_report(kern_now_ns(), "exit");
        gfx_exit_log();
    }
    summary_t0 = 0;
}

// Returns the replay time since the previous present.
static uint64_t ft_frame(uint64_t frame, uint64_t now, uint64_t d) {
    int64_t rep = plat_xchg64(&frame_replay_ns, 0);
    if (!summary_t0) {   // the first present starts the period; there is no interval yet
        summary_t0 = now;
        ft.t0 = now;
        return (uint64_t)rep;
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
        memcpy(ft.worst[i].wait, frame_ns, sizeof frame_ns);
        ft.worst[i].replay = (uint64_t)rep;
    }
    if (now - summary_t0 >= FT_PERIOD_NS) {
        ft_report(now, "60 s");
        summary_t0 = now;
    }
    return (uint64_t)rep;
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

void perf_present(uint64_t frame) {
    static uint64_t last, sum, max, n, win_start, ab_b;
    static int64_t win_prev[PERF_CNT_N];
    prof_render_tid = plat_thread_id();
    if (rt_trace & RT_TRACE_SCHED) kern_sched_report(frame);
    uint64_t now = kern_now_ns(), d = last ? now - last : 0;
    prof_sampler_frame(frame, now, last ? d : 0);
    uint64_t rep = ft_frame(frame, now, d);   // d is 0 on the first present: it only starts the period
    if (__builtin_expect(sec_on, 0)) {
        uint64_t s0 = sec_clock();
        sec_frame(frame, d, rep, frame_ns);
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
    if (__atomic_load_n(&overlay, __ATOMIC_RELAXED))   // the overlay's only cost while it is off: this branch
        perfmon_frame(now, d, frame_ns, rep, __atomic_load_n(&counts[PERF_CNT_TEX_UP], __ATOMIC_RELAXED));
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
