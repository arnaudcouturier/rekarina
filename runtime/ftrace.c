// Frame trace: see ftrace.h.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "ftrace.h"
#include "host.h"
#include "platform/platform.h"

typedef struct { uint64_t t, a, b, c; uint32_t tid; uint32_t tag; } FtEv;
#define FT_CAP (1u << 19)

int ftrace_on;
static FtEv *ev;
static volatile uint32_t nev;
static uint64_t f_first, f_count, t_open;
static int state = -1;   // -1 not read, 0 off, 1 waiting for the window, 2 recording, 3 closing (late GPU events), 4 written
static uint64_t close_frame;
static char path[512];

static const char *const names[FT_N] = {
    "FRAME", "ACQ0", "ACQ1", "PRS0", "PRSW", "PRS1", "SUB0", "SUB1", "SW0", "SW1", "FSYNC", "FIN0", "FIN1", "FLUSH", "EVSIG",
    "WE0", "WE1", "HOLD", "RESUME0", "RESUME1", "PART", "GPART", "DONE", "FWAIT0", "FWAIT1", "NOTE"};

void ftrace_ev_at(int tag, uint64_t t_ns, uint64_t a, uint64_t b, uint64_t c) {
    if (!ev) return;
    uint32_t i = __atomic_fetch_add(&nev, 1, __ATOMIC_RELAXED);
    if (i >= FT_CAP) return;
    ev[i] = (FtEv){t_ns, a, b, c, plat_thread_id(), (uint32_t)tag};
}

void ftrace_ev(int tag, uint64_t a, uint64_t b, uint64_t c) { ftrace_ev_at(tag, plat_now_ns(), a, b, c); }

static int by_time(const void *x, const void *y) {
    const FtEv *a = x, *b = y;
    return a->t < b->t ? -1 : a->t > b->t;
}

static void ft_write(void) {
    uint32_t n = __atomic_load_n(&nev, __ATOMIC_RELAXED);
    if (n > FT_CAP) n = FT_CAP;
    qsort(ev, n, sizeof *ev, by_time);
    FILE *f = fopen(path, "w");
    if (!f) { rt_log("ftrace: cannot write %s", path); return; }
    for (uint32_t i = 0; i < n; i++) {
        const FtEv *e = &ev[i];
        fprintf(f, "%.4f %u %s %llx %llx %llx\n", ((double)e->t - (double)t_open) / 1e6, e->tid,
                e->tag < FT_N ? names[e->tag] : "?", (unsigned long long)e->a, (unsigned long long)e->b, (unsigned long long)e->c);
    }
    fclose(f);
    rt_log("ftrace: %u events of frames %llu-%llu written to %s", n, (unsigned long long)f_first,
           (unsigned long long)(f_first + f_count - 1), path);
}

void ftrace_frame(uint64_t frame) {
    if (state < 0) {
        const char *e = cfg_get("REKARINA_FTRACE");
        state = 0;
        if (e && *e) {
            char *end;
            f_first = strtoull(e, &end, 10);
            f_count = *end == ':' ? strtoull(end + 1, &end, 10) : 4;
            snprintf(path, sizeof path, "%s", *end == ':' ? end + 1 : "ftrace.txt");
            ev = malloc(sizeof *ev * FT_CAP);
            if (ev && f_count) state = 1;
        }
    }
    if (state == 1 && frame >= f_first) {
        t_open = plat_now_ns();
        state = 2;
        __atomic_store_n(&ftrace_on, 1, __ATOMIC_RELEASE);
    }
    if (state == 2) {
        ftrace_ev(FT_FRAME, frame, 0, 0);
        if (frame >= f_first + f_count) state = 3, close_frame = frame + 4;
    } else if (state == 3 && frame >= close_frame) {
        __atomic_store_n(&ftrace_on, 0, __ATOMIC_RELEASE);
        state = 4;
        ft_write();
    }
}
