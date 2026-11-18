// What the game prepares before play, and the ledger of what play still had to build (prep.h).
//
// Ledger. A shader translated or a pipeline built after the guest started stalls play or leaves a draw out: a draw
// that waits for its graphics pipeline holds the replay, and mesh, geometry and compute work is left out until its
// pipeline is there (the grass of a place missing for frames, seconds with a cold driver cache). Each such build is
// counted here by kind and by the map the game had open at that moment (prep_map_open: the map files it reads), with
// what a thread waited for it and how long the build took. The first LOG_EACH get a line of their own with the frame
// number; prep_report logs the totals in one line ("prep: in play: ...") and a table per map at exit. A run that met
// nothing new logs "prep: in play: nothing built or translated".
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "nvn_api.h"
#include "prep.h"
#include "vk.h"
#include "platform/platform.h"

#define MAPS 160
#define LOG_EACH 400

typedef struct { uint64_t n, first, last; double wait, wait_max, build, build_max; } Cell;
typedef struct { char name[64]; Cell c[PREP_KINDS]; } MapRow;

static struct {
    PlatMutex lock;
    volatile int32_t play;
    MapRow map[MAPS];
    int nmap, cur;                     // cur: index of the current map (0: "-", before the first map file)
    uint64_t logged;
} L = {.nmap = 1, .map = {{.name = "-"}}};

static const char *const kind_name[PREP_KINDS] = {"shader translation", "graphics pipeline", "stage library",
                                                  "mesh pipeline", "geometry pipeline", "compute pipeline"};
static const char *const kind_short[PREP_KINDS] = {"xlate", "gfx", "lib", "mesh", "geom", "cs"};

void prep_play_begin(void) { __atomic_store_n(&L.play, 1, __ATOMIC_RELEASE); }

int prep_in_play(void) { return __atomic_load_n(&L.play, __ATOMIC_ACQUIRE) != 0; }

static uint64_t frame_now(void) { return __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) + 1; }

void prep_note(int kind, double wait_ms, double build_ms) {
    if (kind < 0 || kind >= PREP_KINDS || !prep_in_play()) return;
    uint64_t f = frame_now();
    plat_mutex_lock(&L.lock);
    Cell *c = &L.map[L.cur].c[kind];
    if (!c->n) c->first = f;
    c->n++, c->last = f;
    c->wait += wait_ms, c->build += build_ms;
    if (wait_ms > c->wait_max) c->wait_max = wait_ms;
    if (build_ms > c->build_max) c->build_max = build_ms;
    int say = L.logged++ < LOG_EACH;
    char map[64];
    memcpy(map, L.map[L.cur].name, sizeof map);
    plat_mutex_unlock(&L.lock);
    if (say)
        rt_log("prep: frame %llu, map %s: a %s in play (waited %.1f ms, build %.1f ms)", (unsigned long long)f, map,
               kind_name[kind], wait_ms, build_ms);
}

// The game reads <map>.bcett.byml.zs, <map>_Static... and <map>_Dynamic... when it loads a map: the name without that
// suffix is the map. Other files leave the current map as it is. Right after a map change the game reads other map
// files too: a boss room its enemy's own map and one more, the field the town's beside it. Within
// MAP_KEEP_NS of a change another map file leaves the current map as it is.
#define MAP_KEEP_NS 10000000000ull
static uint64_t map_frame;   // frame_now() at the last map change (prep_map_frame)
void prep_map_open(const char *path) {
    static const char tail[] = ".bcett.byml.zs";
    size_t n = path ? strlen(path) : 0;
    if (n <= sizeof tail - 1 || strcmp(path + n - (sizeof tail - 1), tail)) return;
    const char *base = path + n;
    while (base > path && base[-1] != '/' && base[-1] != '\\' && base[-1] != ':') base--;
    char name[64];
    size_t len = (size_t)(path + n - (sizeof tail - 1) - base);
    if (len >= sizeof name) len = sizeof name - 1;
    memcpy(name, base, len);
    name[len] = 0;
    static const char *const cut[] = {"_Static", "_Dynamic"};
    for (int i = 0; i < 2; i++) {
        size_t k = strlen(cut[i]);
        if (len > k && !strcmp(name + len - k, cut[i])) name[len - k] = 0, len -= k;
    }
    static uint64_t changed_ns;   // when the current map was entered
    uint64_t now = plat_now_ns();
    plat_mutex_lock(&L.lock);
    if (L.cur && now - changed_ns < MAP_KEEP_NS && strcmp(L.map[L.cur].name, name)) {
        plat_mutex_unlock(&L.lock);
        return;
    }
    int i = 0;
    while (i < L.nmap && strcmp(L.map[i].name, name)) i++;
    if (i == L.nmap && L.nmap < MAPS) snprintf(L.map[L.nmap++].name, sizeof L.map[0].name, "%s", name);
    int changed = i < L.nmap && i != L.cur;
    if (i < L.nmap) L.cur = i;
    if (changed) changed_ns = now;
    plat_mutex_unlock(&L.lock);
    if (changed) {
        __atomic_store_n(&map_frame, frame_now(), __ATOMIC_RELAXED);
        rt_log("prep: frame %llu: map %s", (unsigned long long)frame_now(), name);
    }
}

uint64_t prep_map_frame(void) { return __atomic_load_n(&map_frame, __ATOMIC_RELAXED); }

const char *prep_map_now(void) { return L.map[L.cur].name; }

void prep_report(void) {
    vk_pipeline_prewarm_quit();   // an exit during the entry step is not a driver crash
    plat_mutex_lock(&L.lock);
    Cell tot[PREP_KINDS];
    memset(tot, 0, sizeof tot);
    uint64_t all = 0;
    for (int m = 0; m < L.nmap; m++)
        for (int k = 0; k < PREP_KINDS; k++) {
            const Cell *c = &L.map[m].c[k];
            if (!c->n) continue;
            Cell *t = &tot[k];
            if (!t->n || c->first < t->first) t->first = c->first;
            if (c->last > t->last) t->last = c->last;
            t->n += c->n, t->wait += c->wait, t->build += c->build;
            if (c->wait_max > t->wait_max) t->wait_max = c->wait_max;
            if (c->build_max > t->build_max) t->build_max = c->build_max;
            all += c->n;
        }
    if (!all) {
        plat_mutex_unlock(&L.lock);
        rt_log("prep: in play: nothing built or translated (%d maps seen)", L.nmap - 1);
        return;
    }
    char line[1024];
    int o = snprintf(line, sizeof line, "prep: in play: %llu built or translated:", (unsigned long long)all);
    for (int k = 0; k < PREP_KINDS && o < (int)sizeof line; k++)
        if (tot[k].n)
            o += snprintf(line + o, sizeof line - (size_t)o,
                          " %s %llu (waits %.0f ms, worst %.1f; builds %.0f ms, worst %.1f)", kind_short[k],
                          (unsigned long long)tot[k].n, tot[k].wait, tot[k].wait_max, tot[k].build, tot[k].build_max);
    rt_log("%s", line);
    rt_log("prep: per map: map | kind count worst-wait-ms worst-build-ms first-frame last-frame ...");
    for (int m = 0; m < L.nmap; m++) {
        o = snprintf(line, sizeof line, "prep:   %-34s |", L.map[m].name);
        int any = 0;
        for (int k = 0; k < PREP_KINDS && o < (int)sizeof line; k++) {
            const Cell *c = &L.map[m].c[k];
            if (!c->n) continue;
            any = 1;
            o += snprintf(line + o, sizeof line - (size_t)o, " %s %llu %.1f %.1f f%llu-%llu", kind_short[k],
                          (unsigned long long)c->n, c->wait_max, c->build_max, (unsigned long long)c->first,
                          (unsigned long long)c->last);
        }
        if (any) rt_log("%s", line);
    }
    plat_mutex_unlock(&L.lock);
}

// ---- the entry step's progress
static struct {
    volatile int64_t done, total;
    volatile int32_t phase;
    uint64_t t0;
} G;

void prep_progress_begin(void) {
    G.t0 = plat_now_ns();
    __atomic_store_n(&G.phase, PREP_PHASE_SHADERS, __ATOMIC_RELEASE);
}
void prep_progress_end(void) { __atomic_store_n(&G.phase, PREP_PHASE_DONE, __ATOMIC_RELEASE); }
void prep_progress_total(int64_t n) { __atomic_fetch_add(&G.total, n, __ATOMIC_RELAXED); }
void prep_progress_step(int64_t n) { __atomic_fetch_add(&G.done, n, __ATOMIC_RELAXED); }
void prep_progress_phase(int phase) { __atomic_store_n(&G.phase, phase, __ATOMIC_RELEASE); }

static volatile int64_t longest_us;
void prep_progress_build(double ms) {
    int64_t us = (int64_t)(ms * 1000), cur = __atomic_load_n(&longest_us, __ATOMIC_RELAXED);
    while (us > cur && !__atomic_compare_exchange_n(&longest_us, &cur, us, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
}
double prep_progress_longest(void) { return (double)__atomic_load_n(&longest_us, __ATOMIC_RELAXED) / 1000.0; }

int prep_progress(int64_t *done, int64_t *total, double *seconds) {
    int64_t d = __atomic_load_n(&G.done, __ATOMIC_RELAXED), t = __atomic_load_n(&G.total, __ATOMIC_RELAXED);
    if (done) *done = d > t ? t : d;
    if (total) *total = t;
    if (seconds) *seconds = G.t0 ? (double)(plat_now_ns() - G.t0) / 1e9 : 0;
    return __atomic_load_n(&G.phase, __ATOMIC_ACQUIRE);
}

static volatile int32_t screen_on;
int prep_screen_active(void) { return __atomic_load_n(&screen_on, __ATOMIC_ACQUIRE); }
void prep_screen_set(int on) { __atomic_store_n(&screen_on, on, __ATOMIC_RELEASE); }
