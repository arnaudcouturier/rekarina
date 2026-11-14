// Uniform blocks as of the submit call. NVN starts the GPU when nvnQueueSubmitCommands returns and the GPU reads the
// constants soon after; here the replay records for several milliseconds, and the GPU reads a pool's bytes when it
// runs the part that uses them (the VRAM mirror's sync copies run then, vk_memory.c "VRAM mirror"). Meanwhile the game,
// having handed the frame over, rewrites per-object uniform blocks on other threads with no fence: late draws read
// another object's matrix (phantom rocks, a flat far field; TotK 1.2.1's producer f_70008c6460, 14-24 ms after the
// submit). So at the start of a submission's recording every uniform range it binds is copied into an arena of its own,
// and the uniform bindings of the replay use the copy.
// - Only ranges of imported pools (memory the CPU writes) are copied, aligned out to 256 bytes (the largest
//   minUniformBufferOffsetAlignment) and at most 64 KiB from the bound address, overlapping ones merged.
// - A range the GPU wrote in a submission that has not completed is left out: its bytes reach guest memory only with
//   that submission's write-back, the mirror holds them until then.
// - A GPU write within the submission (UpdateUniformBuffer, a storage buffer a shader writes, a buffer copy or fill,
//   an image-to-buffer copy: vk_usnap_drop) drops the copies it touches from then on, so later draws read the result.
// - The arena goes back to a free list once the submission's fence is done (completion thread).
// The design follows MacNativeGames' TOTK_UBO_SNAPSHOT (games/totk/third_party/d2r_gfx/gfx/nvn/nvn_mtl.m usnap_take,
// usnap_drop; MIT, Copyright (c) 2026 jhaizhou); the code is ours.
// The copy is taken where vk_exec_record starts: at the submit call when the replay runs on the submitting thread, later
// with TOTK_REPLAY_ASYNC. TOTK_UBO_SNAPSHOT=0 binds live memory as before; =ab alternates (TOTK_AB).
#include <stdlib.h>
#include <string.h>

#include "cfg.h"   // DEBUG-USNAP-MASK
#include "host.h"
#include "nvn_api.h"
#include "vk.h"
#include "vk_dlaa.h"
#include "platform/platform.h"

#define ALIGN 256u
#define MAX_RANGE (64u << 10)
#define ARENA_MIN (4u << 20)
#define CALL_DEPTH 16

typedef struct Arena {
    struct Arena *next;
    VkBuffer buf;
    VmaAllocation alloc;
    uint8_t *map;
    uint8_t *cpu;    // with DLSS (vk_dlaa_available), the same bytes in host memory for the CPU's reads (vk_usnap_bytes):
                     // map is often device memory through the BAR, written fast but read back at about a microsecond a
                     // cache line. Without DLSS nothing reads them back, and the bytes go into map in one copy.
    uint64_t size;
} Arena;

typedef struct { uint64_t lo, hi; } Rng;
typedef struct { uint64_t lo, hi, off; int dropped; } Reg;
typedef struct { uint64_t lo, hi, fence; } Wrote;

static struct {
    int mode;                   // rt_knob: 0 off, 1 on, 2 ab
    int on;                     // this submission has a snapshot
    uint64_t fence, gen;
    Arena *arena;
    Arena *retired;             // earlier snapshots of this submission (vk_usnap_resnap): the draws recorded before read them
    Rng *col;                   // collected ranges
    int ncol, ccol;
    Reg *reg;                   // merged, sorted by lo, disjoint
    int nreg, creg;
    Wrote *wr;                  // GPU writes of submissions not known to be complete
    int nwr, cwr;
    // since the last summary
    uint64_t subs, regions, bytes, copy_ns, hits, drops, inflight, refused, off_subs, resnaps;
} U;

static Arena *free_arenas;
static PlatMutex free_lock = PLAT_MUTEX_INIT;

// DEBUG-USNAP-GPUW: TOTK_USNAP_GPUW=1 leaves out of the snapshot every range the GPU has written at any time, =2 only
// logs such ranges. Pages written: vk_usnap_drop (record time), vk_gpu_written and global memory writes (fence).
#define GW_N (1u << 18)
static uint64_t gw_pages[GW_N];
static PlatMutex gw_lock = PLAT_MUTEX_INIT;
static volatile int gw_mode;
void vk_usnap_gpuw(uint64_t addr, uint64_t n) {
    if (!__atomic_load_n(&gw_mode, __ATOMIC_RELAXED) || !n) return;
    uint64_t p0 = addr >> 12, p1 = (addr + n - 1) >> 12;
    if (p1 - p0 > 16384) p1 = p0 + 16384;
    plat_mutex_lock(&gw_lock);
    for (uint64_t p = p0; p <= p1; p++) {
        uint32_t h = (uint32_t)((p * 0x9E3779B97F4A7C15ull) >> 46) & (GW_N - 1);
        for (uint32_t k = 0; k < GW_N; k++, h = (h + 1) & (GW_N - 1)) {
            if (gw_pages[h] == p + 1) break;
            if (!gw_pages[h]) { gw_pages[h] = p + 1; break; }
        }
    }
    plat_mutex_unlock(&gw_lock);
}
static int gw_hit(uint64_t lo, uint64_t hi) {
    int hit = 0;
    plat_mutex_lock(&gw_lock);
    for (uint64_t p = lo >> 12; p <= (hi - 1) >> 12 && !hit; p++) {
        uint32_t h = (uint32_t)((p * 0x9E3779B97F4A7C15ull) >> 46) & (GW_N - 1);
        for (uint32_t k = 0; k < GW_N && gw_pages[h]; k++, h = (h + 1) & (GW_N - 1))
            if (gw_pages[h] == p + 1) { hit = 1; break; }
    }
    plat_mutex_unlock(&gw_lock);
    return hit;
}

static void *grow(void *p, int *cap, size_t elem) {
    int n = *cap ? *cap * 2 : 1024;
    void *q = realloc(p, (size_t)n * elem);
    if (!q) rt_fatal("vk_usnap: out of memory");
    *cap = n;
    return q;
}

static Arena *arena_get(uint64_t need) {
    plat_mutex_lock(&free_lock);
    Arena **pp = &free_arenas, *a = NULL;
    for (; *pp; pp = &(*pp)->next)
        if ((*pp)->size >= need) {
            a = *pp, *pp = a->next;
            break;
        }
    plat_mutex_unlock(&free_lock);
    if (a) return a;
    a = calloc(1, sizeof *a);
    if (!a) rt_fatal("vk_usnap: out of memory");
    a->size = need < ARENA_MIN ? ARENA_MIN : (need + (1u << 20) - 1) & ~(uint64_t)((1u << 20) - 1);
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = a->size,
                             // transfer source: DLAA copies the camera block out (vk_dlaa.c mv_pass)
                             .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
    // Device-local through the BAR when the device has it (the GPU reads constants on every draw), else host memory.
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                           VMA_ALLOCATION_CREATE_MAPPED_BIT};
    VmaAllocationInfo info;
    VkResult res = vmaCreateBuffer(vk_ctx()->vma, &bi, &ai, &a->buf, &a->alloc, &info);
    if (res != VK_SUCCESS) {
        rt_log("vk_usnap: arena of %llu bytes failed (%d): this submission binds live memory",
               (unsigned long long)a->size, (int)res);
        free(a);
        return NULL;
    }
    a->map = info.pMappedData;
    VkMemoryPropertyFlags mf = 0;   // where VMA put it: under VRAM pressure AUTO_PREFER_DEVICE can fall back to host memory
    vmaGetAllocationMemoryProperties(vk_ctx()->vma, a->alloc, &mf);
    rt_log("vk_usnap: new arena of %.1f MiB in %s", a->size / 1048576.0,
           (mf & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ? "device-local memory" : "host memory (uniform reads cross PCIe)");
    return a;
}

static void arena_done(void *arg) {   // completion thread: the GPU has read it
    Arena *a = arg;
    plat_mutex_lock(&free_lock);
    a->next = free_arenas, free_arenas = a;
    plat_mutex_unlock(&free_lock);
}

// DEBUG-USNAP-MASK: TOTK_USNAP_SCHED=<tick0>:<ticks> with TOTK_USNAP_RANGES=lo-hi,lo-hi,...: from tick0 on, each
// window of <ticks> game ticks snapshots only the bindings in the next range (the stage mask still applies)
static uint64_t sched_lo, sched_hi = ~0ull;
static void sched_update(void) {
    static int read, nr, cur = -2;
    static uint64_t t0, w, rl[32], rh[32];
    if (!read) {
        read = 1;
        const char *e = cfg_get("TOTK_USNAP_SCHED"), *r = cfg_get("TOTK_USNAP_RANGES");
        if (e && r) {
            char *x;
            t0 = strtoull(e, &x, 0), w = *x == ':' ? strtoull(x + 1, NULL, 0) : 40;
            for (const char *p = r; *p && nr < 32;) {
                rl[nr] = strtoull(p, &x, 16);
                rh[nr] = *x == '-' ? strtoull(x + 1, &x, 16) : rl[nr];
                nr++;
                p = *x == ',' ? x + 1 : x;
                if (!*x) break;
            }
        }
    }
    if (!nr) return;
    uint64_t t = frame_step_ticks();
    int i = t < t0 ? -1 : (int)((t - t0) / (w ? w : 1));
    if (i >= nr) i = -1;
    if (i == cur) return;
    cur = i;
    sched_lo = i < 0 ? 0 : rl[i], sched_hi = i < 0 ? ~0ull : rh[i];
    rt_log("USNAPSCHED: config %d (%#llx..%#llx) from frame %llu tick %llu", i, (unsigned long long)sched_lo,
           (unsigned long long)sched_hi, (unsigned long long)__atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED),
           (unsigned long long)t);
}

static void collect(const op_hdr *h, int depth) {
    for (;;) {
        if (h->size < sizeof *h || (h->size & 7) || h->op >= OP_COUNT_)
            rt_fatal("vk_usnap: corrupt command record at %p: op %u size %u", (const void *)h, h->op, h->size);
        const void *a = nvn_op_payload(h);
        if (h->op == OP_END) return;
        if (h->op == OP_JUMP) {
            h = (const op_hdr *)(uintptr_t)((const p_jump *)a)->target;
            continue;
        }
        if (h->op == OP_CALL && depth < CALL_DEPTH) collect(nvn_op_first(((const p_jump *)a)->target), depth + 1);
        if (h->op == OP_BIND_UBO) {
            const p_bind_buf *b = a;
            // DEBUG-USNAP-MASK: TOTK_USNAP_STAGES=<mask of stage slots>, TOTK_USNAP_LO/HI=<guest range> snapshotted
            static int64_t smask = -1;
            static uint64_t alo, ahi;
            if (smask < 0) {
                const char *e = cfg_get("TOTK_USNAP_STAGES");
                smask = e ? strtoll(e, NULL, 0) : 0xffff;
                e = cfg_get("TOTK_USNAP_LO"), alo = e ? strtoull(e, NULL, 0) : 0;
                e = cfg_get("TOTK_USNAP_HI"), ahi = e ? strtoull(e, NULL, 0) : ~0ull;
            }
            if (!(smask >> (b->stage & 15) & 1) || b->addr < alo || b->addr >= ahi ||
                (!(b->stage & 15) && (b->addr < sched_lo || b->addr >= sched_hi))) {
                h = nvn_op_next(h);
                continue;
            }
            if (b->addr && b->size) {
                uint64_t n = b->size < MAX_RANGE ? b->size : MAX_RANGE;
                // the vertex stage's object and bone blocks: also the game's other copy on either side, which still
                // holds last frame's (vk_mvprev.c)
                uint64_t step = vk_mvprev_step(b->stage, b->index, b->size);
                // which neighbours lie in an imported pool, per block and vk_mem_gen: the same blocks are bound by
                // many draws, and two resolves for each binding were a quarter of the snapshot's time
                static struct { uint64_t addr, size, gen; int ok; } nc[1024];
                uint32_t ci = (uint32_t)((b->addr ^ n << 48) * 0x9E3779B97F4A7C15ull >> 54);
                uint64_t gen = vk_mem_gen();
                int known = step && nc[ci].addr == b->addr && nc[ci].size == n && nc[ci].gen == gen, ok = 0;
                for (int k = step ? -1 : 0; k <= (step ? 1 : 0); k++) {
                    uint64_t at = b->addr + (uint64_t)((int64_t)k * (int64_t)step);
                    if (k < 0 && b->addr < step) continue;
                    // a neighbour past its pool would make the whole merged region fail the pool check below, and
                    // the bindings in it read live memory again
                    VkMemSpan sp;
                    uint64_t lo = at & ~(uint64_t)(ALIGN - 1);
                    if (k && known) {
                        if (!(nc[ci].ok >> (k + 1) & 1)) continue;
                    } else if (k) {
                        if (!vk_mem_resolve(lo, at + n - lo, &sp) || sp.alloc || !sp.pool || sp.avail < at + n - lo ||
                            (sp.cls != VK_MCLS_HOST_CACHED && sp.cls != VK_MCLS_HOST_UNC))
                            continue;
                        ok |= 1 << (k + 1);
                    }
                    if (U.ncol == U.ccol) U.col = grow(U.col, &U.ccol, sizeof *U.col);
                    U.col[U.ncol++] = (Rng){at & ~(uint64_t)(ALIGN - 1), at + n};
                }
                if (step && !known) nc[ci].addr = b->addr, nc[ci].size = n, nc[ci].gen = gen, nc[ci].ok = ok;
            }
        }
        h = nvn_op_next(h);
    }
}

static int by_lo(const void *a, const void *b) {
    uint64_t x = ((const Rng *)a)->lo, y = ((const Rng *)b)->lo;
    return x < y ? -1 : x > y;
}
static int wr_by_lo(const void *a, const void *b) {
    uint64_t x = ((const Wrote *)a)->lo, y = ((const Wrote *)b)->lo;
    return x < y ? -1 : x > y;
}

// Sorts the collected ranges by lo. A town frame binds about 10,000 uniform ranges and qsort took half a millisecond
// of the copy's 0.96 ms on the game thread, so they go through an LSD radix sort on (lo - lowest) / ALIGN, three
// passes of 11 bits; qsort is left for a span that does not fit in 33 bits (2 TiB).
#define RADIX_BITS 11
static void sort_col(void) {
    static Rng *tmp;
    static int ctmp;
    int n = U.ncol;
    if (n < 2) return;
    uint64_t lo = UINT64_MAX, hi = 0;
    for (int i = 0; i < n; i++) {
        if (U.col[i].lo < lo) lo = U.col[i].lo;
        if (U.col[i].lo > hi) hi = U.col[i].lo;
    }
    if ((hi - lo) / ALIGN >> (3 * RADIX_BITS)) {
        qsort(U.col, (size_t)n, sizeof *U.col, by_lo);
        return;
    }
    if (ctmp < n) {
        free(tmp);
        ctmp = U.ccol;
        if (!(tmp = malloc((size_t)ctmp * sizeof *tmp))) rt_fatal("vk_usnap: out of memory");
    }
    Rng *src = U.col, *dst = tmp;
    for (int pass = 0; pass < 3; pass++) {
        int shift = pass * RADIX_BITS, count[1 << RADIX_BITS] = {0};
        for (int i = 0; i < n; i++) count[(src[i].lo - lo) / ALIGN >> shift & ((1 << RADIX_BITS) - 1)]++;
        for (int k = 0, sum = 0; k < 1 << RADIX_BITS; k++) {
            int c = count[k];
            count[k] = sum, sum += c;
        }
        for (int i = 0; i < n; i++) dst[count[(src[i].lo - lo) / ALIGN >> shift & ((1 << RADIX_BITS) - 1)]++] = src[i];
        Rng *t = src;
        src = dst, dst = t;
    }
    if (src != U.col) memcpy(U.col, src, (size_t)n * sizeof *src);   // three passes end in tmp
}

// Keeps the GPU writes of submissions that are not done, merged into disjoint ranges sorted by lo (the terrain rewrites
// one block before every draw: hundreds of equal entries), each with the newest fence among its parts.
static void wr_prune(void) {
    uint64_t done = vk_sync_fence_done();
    int n = 0;
    for (int i = 0; i < U.nwr; i++)
        if (U.wr[i].fence > done) U.wr[n++] = U.wr[i];
    qsort(U.wr, (size_t)n, sizeof *U.wr, wr_by_lo);
    int m = 0;
    for (int i = 0; i < n; i++) {
        if (m && U.wr[i].lo <= U.wr[m - 1].hi) {
            if (U.wr[i].hi > U.wr[m - 1].hi) U.wr[m - 1].hi = U.wr[i].hi;
            if (U.wr[i].fence > U.wr[m - 1].fence) U.wr[m - 1].fence = U.wr[i].fence;
        } else {
            U.wr[m++] = U.wr[i];
        }
    }
    U.nwr = m;
}
static int wr_hits(uint64_t lo, uint64_t hi) {   // wr_prune ran: disjoint, sorted
    int a = 0, b = U.nwr;
    while (a < b) {
        int k = (a + b) / 2;
        if (U.wr[k].hi <= lo) a = k + 1;
        else b = k;
    }
    return a < U.nwr && U.wr[a].lo < hi;
}

static void take(uint64_t t0);

static void summary(void) {
    if (U.subs + U.off_subs < 3600) return;
    double s = U.subs ? (double)U.subs : 1.0;
    rt_log("vk_usnap: %llu submissions with a snapshot, %llu without; per submission %.0f ranges, %.1f KiB copied in "
           "%.3f ms, %.0f uniform bindings from the copy, %.1f dropped by GPU writes, %.1f left out (GPU writes in "
           "flight), %.1f refused (not an imported pool); %llu taken again after a wait for the CPU",
           (unsigned long long)U.subs, (unsigned long long)U.off_subs, U.regions / s, U.bytes / s / 1024.0,
           U.copy_ns / s / 1e6, U.hits / s, U.drops / s, U.inflight / s, U.refused / s, (unsigned long long)U.resnaps);
    U.subs = U.off_subs = U.regions = U.bytes = U.copy_ns = U.hits = U.drops = U.inflight = U.refused = U.resnaps = 0;
}

void vk_usnap_begin(const uint64_t *handles, int n, uint64_t fence) {
    if (!U.mode) U.mode = rt_knob("UBO_SNAPSHOT", "1") + 1;   // stored +1: 0 is "not read yet"
    static int gw_read;   // DEBUG-USNAP-GPUW
    if (!gw_read) gw_read = 1, __atomic_store_n(&gw_mode, rt_knob("USNAP_GPUW", "0"), __ATOMIC_RELAXED);
    U.on = 0, U.fence = fence, U.gen++;
    summary();
    if (!RT_KNOB_ON(U.mode - 1)) {
        U.off_subs++;
        return;
    }
    uint64_t t0 = kern_now_ns();
    sched_update();   // DEBUG-USNAP-MASK
    U.ncol = 0;
    for (int i = 0; i < n; i++)
        if (handles[i]) collect(nvn_op_first(handles[i]), 0);
    take(t0);
}

// The collected ranges merged, checked and copied into a new arena (U.on when it worked).
static void take(uint64_t t0) {
    sort_col();
    wr_prune();
    // merge, keeping only what lies wholly in one imported pool and no GPU write in flight touches
    U.nreg = 0;
    uint64_t total = 0;
    for (int i = 0; i < U.ncol;) {
        uint64_t lo = U.col[i].lo, hi = U.col[i].hi;
        for (i++; i < U.ncol && U.col[i].lo <= hi; i++)
            if (U.col[i].hi > hi) hi = U.col[i].hi;
        VkMemSpan sp;
        if (!vk_mem_resolve(lo, hi - lo, &sp) || sp.alloc || !sp.pool || sp.avail < hi - lo ||
            (sp.cls != VK_MCLS_HOST_CACHED && sp.cls != VK_MCLS_HOST_UNC)) {
            U.refused++;
            continue;
        }
        if (wr_hits(lo, hi)) {
            U.inflight++;
            continue;
        }
        if (gw_mode && gw_hit(lo, hi)) {   // DEBUG-USNAP-GPUW
            static uint64_t seen[256];
            static int nseen;
            int k = 0;
            while (k < nseen && seen[k] != lo) k++;
            if (k == nseen && nseen < 256) {
                seen[nseen++] = lo;
                rt_log("USNAPGW: snapshot region %#llx..%#llx overlaps pages the GPU wrote%s", (unsigned long long)lo,
                       (unsigned long long)hi, gw_mode == 1 ? ": left out" : "");
            }
            if (gw_mode == 1) continue;
        }
        if (U.nreg == U.creg) U.reg = grow(U.reg, &U.creg, sizeof *U.reg);
        U.reg[U.nreg++] = (Reg){lo, hi, total, 0};
        total += (hi - lo + ALIGN - 1) & ~(uint64_t)(ALIGN - 1);
    }
    if (!U.nreg) return;
    if (!(U.arena = arena_get(total))) return;
    uint8_t *dst = U.arena->map;
    if (vk_dlaa_available()) {
        if (!U.arena->cpu && !(U.arena->cpu = malloc(U.arena->size))) rt_fatal("vk_usnap: out of memory");
        dst = U.arena->cpu;
    }
    for (int i = 0; i < U.nreg; i++)
        memcpy(dst + U.reg[i].off, (const void *)(uintptr_t)U.reg[i].lo, U.reg[i].hi - U.reg[i].lo);
    if (dst != U.arena->map) memcpy(U.arena->map, dst, total);   // one sequential write into the device copy
    vmaFlushAllocation(vk_ctx()->vma, U.arena->alloc, 0, total);   // a no-op on coherent memory
    U.on = 1;
    U.subs++, U.regions += (uint64_t)U.nreg, U.bytes += total, U.copy_ns += kern_now_ns() - t0;
}

// A wait in the command stream for the CPU (OP_WAIT_EVENT) has been satisfied: the commands after it read the uniform
// blocks as the CPU left them by then, as the console's GPU, which stopped there, reads them. The ranges bound from
// `from` to the end of its recording and in the recordings `rest` are copied again into a new arena; the draws
// recorded before keep the old one, which goes back at the fence with it.
void vk_usnap_resnap(const void *from, const uint64_t *rest, int nrest) {
    if (!U.on) return;   // no snapshot in this submission: its bindings read memory as it is
    uint64_t t0 = kern_now_ns();
    if (U.arena) U.arena->next = U.retired, U.retired = U.arena, U.arena = NULL;
    U.on = 0, U.gen++;
    U.ncol = 0;
    collect(from, 0);
    for (int i = 0; i < nrest; i++)
        if (rest[i]) collect(nvn_op_first(rest[i]), 0);
    U.resnaps++;
    take(t0);
}

// A submission held at a wait for another queue (vk_exec.c "Queues interleaved") reads the uniform blocks as of its own
// submit, as the rest of it would have: its snapshot is taken out of U here and goes in and out of U around each stretch
// of the rest (the game rewrites these blocks for the next frame meanwhile; read live, the culling of a held compute
// queue used the next frame's block, and every second frame's mesh lists were wrong). A GPU write of the rest also drops
// the copies of the submission it is recorded in (U.other).
typedef struct UsnapHeld { Arena *arena; Reg *reg; int nreg, creg, on; } UsnapHeld;
static UsnapHeld *U_other;

void *vk_usnap_hold(void) {
    UsnapHeld *h = calloc(1, sizeof *h);
    if (!h) rt_fatal("vk_usnap: out of memory");
    h->arena = U.arena, h->on = U.on, h->nreg = U.nreg, h->creg = U.nreg;
    if (U.on && U.nreg) {
        if (!(h->reg = malloc(sizeof *h->reg * (size_t)U.nreg))) rt_fatal("vk_usnap: out of memory");
        memcpy(h->reg, U.reg, sizeof *h->reg * (size_t)U.nreg);
    }
    U.arena = NULL, U.on = 0, U.gen++;   // the parts recorded so far keep it: it goes back at the rest's last fence
    return h;
}

static void held_exchange(UsnapHeld *h) {
    Arena *a = U.arena; Reg *r = U.reg; int n = U.nreg, c = U.creg, on = U.on;
    U.arena = h->arena, U.reg = h->reg, U.nreg = h->nreg, U.creg = h->creg, U.on = h->on;
    h->arena = a, h->reg = r, h->nreg = n, h->creg = c, h->on = on;
    U.gen++;
}
void vk_usnap_held_enter(void *held) { held_exchange(held); U_other = held; }   // now U is the held one's, held the running one's
void vk_usnap_held_leave(void *held) { U_other = NULL; held_exchange(held); }

void vk_usnap_held_done(void *held, uint64_t fence) {
    UsnapHeld *h = held;
    if (h->arena) vk_completion_add(fence, arena_done, h->arena);
    free(h->reg);
    free(h);
}

void vk_usnap_end(uint64_t fence) {
    if (U.arena) vk_completion_add(fence, arena_done, U.arena), U.arena = NULL;
    for (Arena *a = U.retired, *next; a; a = next) next = a->next, vk_completion_add(fence, arena_done, a);
    U.retired = NULL;
    U.on = 0, U.gen++;
}

// First region whose end is above a.
static int first_above(uint64_t a) {
    int lo = 0, hi = U.nreg;
    while (lo < hi) {
        int m = (lo + hi) / 2;
        if (U.reg[m].hi <= a) lo = m + 1;
        else hi = m;
    }
    return lo;
}

// The snapshot of the submission whose draw stretches are recorded in chunks (vk_exec.c), as the chunk threads read
// it: taken by the replay thread before the chunks go out (vk_usnap_par_view) and left alone until they are waited for.
// U itself changes places with a held rest's snapshot while a chunk may still be recorded (vk_usnap_held_enter); the
// arena and the regions the view names stay the running submission's. A region dropped meanwhile is seen dropped.
static struct { Arena *arena; Reg *reg; int nreg, on; uint64_t gen; } PV;
void vk_usnap_par_view(void) {
    if (PV.arena == U.arena && PV.reg == U.reg && PV.nreg == U.nreg && PV.on == U.on) return;
    PV.arena = U.arena, PV.reg = U.reg, PV.nreg = U.nreg, PV.on = U.on, PV.gen++;
}

static int find_view(uint64_t addr, uint64_t n, uint64_t align, VkDescriptorBufferInfo *out) {
    if (!PV.on || !n) return 0;
    static _Thread_local struct { uint64_t addr, gen; int i; } vc[256];
    typeof(vc[0]) *c = &vc[(addr >> 8 ^ addr >> 16) & 255];
    int i;
    if (c->addr == addr && c->gen == PV.gen) i = c->i;
    else {
        int lo = 0, hi = PV.nreg;
        while (lo < hi) {
            int m = (lo + hi) / 2;
            if (PV.reg[m].hi <= addr) lo = m + 1;
            else hi = m;
        }
        i = lo, c->addr = addr, c->gen = PV.gen, c->i = i;
    }
    if (i >= PV.nreg) return 0;
    const Reg *r = &PV.reg[i];
    if (r->dropped || addr < r->lo || addr + n > r->hi) return 0;
    uint64_t off = r->off + (addr - r->lo);
    if (off % align) return 0;
    *out = (VkDescriptorBufferInfo){PV.arena->buf, off, n};
    return 1;
}

int vk_usnap_find(uint64_t addr, uint64_t n, uint64_t align, VkDescriptorBufferInfo *out) {
    extern _Thread_local int vk_par_worker;   // vk_exec.c: this thread records a chunk of a draw stretch
    if (vk_par_worker) return find_view(addr, n, align, out);
    if (!U.on || !n) return 0;
    // The scene's blocks are bound by draw after draw: the region found for an address is kept while the snapshot
    // stands (U.gen moves with every new snapshot and every dropped region), which spares the search of ~900 regions.
    static _Thread_local struct { uint64_t addr, gen; int i; } fc[256];   // each replaying thread its own
    typeof(fc[0]) *c = &fc[(addr >> 8 ^ addr >> 16) & 255];
    int i;
    if (c->addr == addr && c->gen == U.gen) i = c->i;
    else i = first_above(addr), c->addr = addr, c->gen = U.gen, c->i = i;
    if (i >= U.nreg) return 0;
    const Reg *r = &U.reg[i];
    if (r->dropped || addr < r->lo || addr + n > r->hi) return 0;
    uint64_t off = r->off + (addr - r->lo);
    if (off % align) return 0;
    *out = (VkDescriptorBufferInfo){U.arena->buf, off, n};
    U.hits++;
    return 1;
}

void vk_usnap_drop(uint64_t addr, uint64_t n) {
    vk_usnap_gpuw(addr, n);   // DEBUG-USNAP-GPUW
    if (!U.mode || !n) return;
    if (U.nwr == U.cwr) U.wr = grow(U.wr, &U.cwr, sizeof *U.wr);
    U.wr[U.nwr++] = (Wrote){addr, addr + n, U.fence};
    if (U_other && U_other->on)   // a held rest writes: the running submission's copies of the range go too
        for (int i = 0; i < U_other->nreg; i++)
            if (U_other->reg[i].lo < addr + n && addr < U_other->reg[i].hi && !U_other->reg[i].dropped)
                U_other->reg[i].dropped = 1, U.drops++, U.gen++;
    if (!U.on) return;
    for (int i = first_above(addr); i < U.nreg && U.reg[i].lo < addr + n; i++)
        if (!U.reg[i].dropped) U.reg[i].dropped = 1, U.drops++, U.gen++;
}

uint64_t vk_usnap_gen(void) { return U.gen; }

// the host copy where there is one (DLSS), else the device copy
static uint8_t *host_copy(void) { return U.arena->cpu ? U.arena->cpu : U.arena->map; }

const uint8_t *vk_usnap_map(void) { return U.arena ? host_copy() : NULL; }

// A deferred wait for the CPU's signal is met (vk_exec_xfer.c vk_exec_wait_resolve): the arena of the commands after the
// wait, taken before the signal, takes the CPU's bytes again before the part that reads it goes to the GPU.
void vk_usnap_refresh(void) {
    if (!U.on || !U.arena || !U.nreg) return;
    uint64_t t0 = kern_now_ns(), total = 0;
    uint8_t *dst = host_copy();
    for (int i = 0; i < U.nreg; i++) {
        memcpy(dst + U.reg[i].off, (const void *)(uintptr_t)U.reg[i].lo, U.reg[i].hi - U.reg[i].lo);
        if (U.reg[i].off + (U.reg[i].hi - U.reg[i].lo) > total) total = U.reg[i].off + (U.reg[i].hi - U.reg[i].lo);
    }
    if (dst != U.arena->map) memcpy(U.arena->map, dst, total);
    vmaFlushAllocation(vk_ctx()->vma, U.arena->alloc, 0, total);
    U.copy_ns += kern_now_ns() - t0;
}

// Replaces bytes of this submission's copy of a guest range, in the host copy and in the one the GPU reads (vk_dlaa.c
// vk_dlaa_pjitter). 0 when the range has no copy.
int vk_usnap_poke(uint64_t addr, const void *p, uint64_t n) {
    VkDescriptorBufferInfo bi;
    if (!vk_usnap_find(addr, n, 1, &bi)) return 0;
    memcpy(host_copy() + bi.offset, p, n);
    if (U.arena->cpu) memcpy(U.arena->map + bi.offset, p, n);
    vmaFlushAllocation(vk_ctx()->vma, U.arena->alloc, bi.offset, n);
    return 1;
}

const uint8_t *vk_usnap_bytes(uint64_t addr, uint64_t n) {
    VkDescriptorBufferInfo bi;
    return vk_usnap_find(addr, n, 1, &bi) ? host_copy() + bi.offset : NULL;
}

// The arenas back on the free list (vk_exec_draw_shutdown: the device is idle, every completion ran).
void vk_usnap_shutdown(void) {
    plat_mutex_lock(&free_lock);
    for (Arena *a = free_arenas, *next; a; a = next) next = a->next, vmaDestroyBuffer(vk_ctx()->vma, a->buf, a->alloc), free(a->cpu), free(a);
    free_arenas = NULL;
    plat_mutex_unlock(&free_lock);
}
