// Adapted from MacNativeGames games/totk/runtime/services.c and loader.c @ b75be8c (MIT, Copyright (c) 2026
// MacNativeGames contributors), and LAS Recomp runtime/main.c and platform/windows.c @ da35b83 (MIT, Copyright (c)
// 2026 Peiikai).
// Guest address space and the memory SVCs: heap, module data permissions, stack moves,
// QueryMemory, physical memory in the alias region, GetInfo memory ids, and the kernel-region allocator.
//
// Every region is a plain reservation; pages are committed and decommitted at 4 KiB granularity. Decommitted pages
// come back zeroed, so no stale-page bookkeeping is needed. The stack and heap block and the alias region are
// reserved write-watched (platform.h plat_mem_reserve): mem_written_pages reports the pages the CPU wrote, which
// the Vulkan back end uploads again.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "layout.h"
#include "platform/platform.h"

#define PAGE 0x1000ull
#define GiB (1ull << 30)
#define LOW_SEARCH_FROM 0x100000000ull
#define STACK_OFF 0x10000000ull
#define STACK_SIZE 0x10000000ull
#define HEAP_OFF (STACK_OFF + STACK_SIZE + 0x10000000ull)
#define ALIAS_SIZE (64 * GiB)
#define ALIAS_ALIGN (4 * GiB)
#define ALIAS_PAGES (ALIAS_SIZE / PAGE)
#ifdef GAME_NS2
// A Switch 2 game: 10 GiB of the console's 12 for the application (MacNativeGames services.c APP_POOL_DEFAULT for
// GAME_NS2, from nxnative's and pokopia-native's profiles), a heap region that holds it, and the system resource the
// game's main.npdm declares (OoT: 0x3000000; non-zero turns the SDK's virtual address memory on).
#define LOW_SIZE (12 * GiB)
#define HEAP_CAP (0x280000000ull)
#define DEFAULT_POOL 0x280000000ull
#define SYSTEM_RESOURCE_BUDGET 0x3000000ull
#else
#define LOW_SIZE (6 * GiB)
#define HEAP_CAP (4 * GiB)
#define DEFAULT_POOL 0xCD500000ull       // retail application pool
#define SYSTEM_RESOURCE_BUDGET (32ull << 20)
#endif
#define MAX_STACK_MAPS 4096
#define REEXEC_LIMIT 20

// Horizon memory states, permissions, attributes and results used here.
enum { ST_FREE = 0, ST_CODE = 3, ST_CODEDATA = 4, ST_NORMAL = 5, ST_STACK = 0xB, ST_THREADLOCAL = 0xC,
       ST_INACCESSIBLE = 0x10 };
enum { PERM_NONE = 0, PERM_R = 1, PERM_RW = 3, PERM_RX = 5 };
enum { ATTR_PERMISSION_LOCKED = 16 };
#define R_OK 0u
#define R_INVALID_ADDRESS 0xcc01u   // misaligned address
#define R_INVALID_SIZE 0xca01u
#define R_INVALID_NEW_PERM 0xd801u
#define R_INVALID_STATE 0xd401u     // InvalidCurrentMemory
#define R_INVALID_REGION 0xdc01u
#define R_LIMIT_REACHED 0x10801u

static RtGuestLayout L;
static uint64_t mods_lo, mods_hi;   // fixed module window
static PlatMutex lock = PLAT_MUTEX_INIT;
static uint64_t heap_size, alias_bytes;
static uint8_t *alias_pages;        // 1 per mapped 4 KiB page of the alias region
typedef struct { uint64_t dst, src, size; } StackMap;
static StackMap stack_maps[MAX_STACK_MAPS];
static unsigned nstack_maps;
static uint8_t *module_perm[MOD_COUNT];   // per data page of each module: perm | ATTR_PERMISSION_LOCKED

static uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

static int reserve_at(uint64_t at, uint64_t size, int watch) { return plat_mem_reserve(at, size, watch); }

// ---- frozen regions
// A 2 MiB region of the watched reservations that nothing wrote for a while can be made read-only (mem_freeze), so the
// back end's write-watch queries skip it: no page of a frozen region changes before a thaw. The first write faults and
// thaw_handler makes the region writable again before the write retries. A kernel write into guest memory (a file
// read) fails on a read-only page instead of faulting, so file reads thaw their buffer first (mem_thaw), and so does
// every commit, decommit and protection change here. A region stays thawed for FRZ_HOLD_MS after a mem_thaw: a file
// read still in flight writes its buffer later. frz_lock is a leaf lock: nothing writes guest memory under it.
#define FRZ_SHIFT 21
#define FRZ_SIZE (1ull << FRZ_SHIFT)
#define FRZ_HOLD_MS 5000u
enum { FRZ_RUNS = 1, FRZ_WHOLE = 2 };   // frz[]: frozen run by run (it has reserved holes), or as one 2 MiB range
static volatile uint8_t frz[RT_ADDRESS_LIMIT >> FRZ_SHIFT];
static uint8_t frz_odd[RT_ADDRESS_LIMIT >> FRZ_SHIFT];    // frz_lock: a page of it may have a protection other than read-write
static int frz_direct = 1;                                // REKARINA_FREEZE_WHOLE=0: every freeze and thaw walks the state
static uint32_t thaw_ms[RT_ADDRESS_LIMIT >> FRZ_SHIFT];   // frz_lock: GetTickCount of the last thaw request, 0 never
static volatile int32_t nfrz;
static PlatMutex frz_lock = PLAT_MUTEX_INIT;
static volatile int64_t frz_count[4];   // freezes; thaws by a write fault, by mem_thaw, by a mapping change

static int watched(uint64_t addr, uint64_t size) {
    const RtGuestLayout *g = &L;
    return (addr >= g->aslr.base && size <= LOW_SIZE && addr - g->aslr.base <= LOW_SIZE - size) ||
           (addr >= g->alias.base && size <= ALIAS_SIZE && addr - g->alias.base <= ALIAS_SIZE - size);
}

// frz_lock held. Pages of [a, a + n) are about to get a protection other than read-write: their regions freeze by walking.
static void frz_mark_odd(uint64_t a, uint64_t n) {
    if (!n || a >= RT_ADDRESS_LIMIT) return;
    uint64_t r0 = a >> FRZ_SHIFT, r1 = (a + n - 1 < RT_ADDRESS_LIMIT ? a + n - 1 : RT_ADDRESS_LIMIT - 1) >> FRZ_SHIFT;
    for (uint64_t r = r0; r <= r1; r++) frz_odd[r] = 1;
}

// frz_lock held exclusively. why: 1 a write fault, 2 mem_thaw (holds the region thawed), 3 a mapping change here.
static void thaw_locked(uint64_t r, int why) {
    if (why == 2) thaw_ms[r] = (uint32_t)plat_now_ms() | 1;
    int st = frz[r];
    if (!st) return;
    // The read-only runs only: freeze_locked left reserved holes (heap pages given back) as they were. Every committed
    // page of a frozen region was read-write at the freeze, and mapping changes thaw before they run, so these are all
    // ours. A region frozen whole has no holes, so it needs no walk (freeze_locked says what a walk costs).
    uint64_t a = r << FRZ_SHIFT, end = a + FRZ_SIZE, b;
    if (st == FRZ_WHOLE) {
        if (!plat_mem_protect(a, FRZ_SIZE, PLAT_PROT_RW))
            rt_fatal("memory: thawing %#llx+%#llx failed", (unsigned long long)a, (unsigned long long)FRZ_SIZE);
    } else {
        for (; a < end; a = b)
            if (plat_mem_state_walk(a, end, &b) == PLAT_PROT_READ && !plat_mem_protect(a, b - a, PLAT_PROT_RW))
                rt_fatal("memory: thawing %#llx+%#llx failed", (unsigned long long)a, (unsigned long long)(b - a));
    }
    __atomic_store_n(&frz[r], 0, __ATOMIC_RELEASE);
    plat_dec32(&nfrz);
    plat_inc64(&frz_count[why]);
}

// frz_lock held exclusively.
static void thaw_range_locked(uint64_t a, uint64_t n, int why) {
    if (!n || a >= RT_ADDRESS_LIMIT) return;
    uint64_t r0 = a >> FRZ_SHIFT, r1 = (a + n - 1 < RT_ADDRESS_LIMIT ? a + n - 1 : RT_ADDRESS_LIMIT - 1) >> FRZ_SHIFT;
    for (uint64_t r = r0; r <= r1; r++) thaw_locked(r, why);
}

static void lazy_touch_range(uint64_t addr, uint64_t n);
void mem_thaw(uint64_t a, uint64_t n) {
    plat_mutex_lock(&frz_lock);   // also when nothing is frozen: the hold keeps a read's buffer writable
    thaw_range_locked(a, n, 2);
    plat_mutex_unlock(&frz_lock);
    lazy_touch_range(a, n);
}

// frz_lock held exclusively. Makes the committed pages of region r read-only when every one of them is read-write;
// reserved holes (heap pages given back) stay as they are, and thaw_locked finds the read-only runs again. Returns the
// frz state, 0 when it did not freeze. A region whose pages never had another protection takes one call, which fails
// without changing anything when a page of it is not committed. The walk is what makes a freeze cost milliseconds:
// VirtualQuery measures the whole run of equal pages from its address on (about 13 us per resident MiB, a 3 GiB run
// 42 ms), and only the first 2 MiB of the answer are used. Windows has no bounded query.
static int freeze_locked(uint64_t r) {
    uint64_t a = r << FRZ_SHIFT, end = a + FRZ_SIZE, run[16][2];
    int n = 0;
    uint64_t sec0 = sec_begin();
    if (frz_direct && !frz_odd[r]) {
        int whole = plat_mem_protect_committed(a, FRZ_SIZE, PLAT_PROT_READ);
        sec_end(SEC_FZ_PROTECT, sec0);
        if (whole) return FRZ_WHOLE;
        sec0 = sec_begin();
    }
    for (uint64_t x = a, y; x < end; x = y) {
        int st = plat_mem_state_walk(x, end, &y);
        if (st >= PLAT_PROT_NONE) {
            if (st != PLAT_PROT_RW || n == 16) return 0;
            run[n][0] = x, run[n][1] = y, n++;
        } else if (st != PLAT_MEM_RESERVED) return 0;
    }
    sec_end(SEC_FZ_STATE, sec0);
    sec0 = sec_begin();
    for (int i = 0; i < n; i++)
        if (!plat_mem_protect(run[i][0], run[i][1] - run[i][0], PLAT_PROT_READ)) {
            while (i--) plat_mem_protect(run[i][0], run[i][1] - run[i][0], PLAT_PROT_RW);
            return 0;
        }
    sec_end(SEC_FZ_PROTECT, sec0);
    return n > 0 ? FRZ_RUNS : 0;
}

int mem_freeze(uint64_t a) {
    if ((a & (FRZ_SIZE - 1)) || !watched(a, FRZ_SIZE)) return 0;
    uint64_t r = a >> FRZ_SHIFT;
    int ok = 0, st;
    uint64_t sec0 = sec_begin();
    plat_mutex_lock(&frz_lock);
    sec_end(SEC_FZ_LOCK, sec0);
    if (frz[r]) ok = 1;
    else if (thaw_ms[r] && (int32_t)((uint32_t)plat_now_ms() - thaw_ms[r]) < (int32_t)FRZ_HOLD_MS) ok = 0;   // |1 may be ahead
    else if ((st = freeze_locked(r))) {
        __atomic_store_n(&frz[r], (uint8_t)st, __ATOMIC_RELEASE);
        plat_inc32(&nfrz);
        plat_inc64(&frz_count[0]);
        ok = 1;
    }
    plat_mutex_unlock(&frz_lock);
    return ok;
}

int mem_frozen(uint64_t a) { return a < RT_ADDRESS_LIMIT && __atomic_load_n(&frz[a >> FRZ_SHIFT], __ATOMIC_ACQUIRE) != 0; }

void mem_freeze_stats(uint64_t *frozen, uint64_t *freezes, uint64_t *fault_thaws, uint64_t *other_thaws) {
    *frozen = (uint64_t)__atomic_load_n(&nfrz, __ATOMIC_RELAXED);
    *freezes = (uint64_t)plat_xchg64(&frz_count[0], 0);
    *fault_thaws = (uint64_t)plat_xchg64(&frz_count[1], 0);
    *other_thaws = (uint64_t)plat_xchg64(&frz_count[2], 0) + (uint64_t)plat_xchg64(&frz_count[3], 0);
}

// The first write into a frozen region thaws it and retries. A fault in a region another thread thawed meanwhile
// retries too; any other fault goes on to the crash handler.
// The platform's write-fault hook (plat_mem_fault_hook): 1 retries the write.
// ---- copy guard (host.h): guest ranges a recorded GPU copy has yet to read (the VRAM mirrors' CPU -> mirror uploads
// run when the GPU reaches them, not when they are recorded). Read-only until the copy ran; the first CPU write waits
// for it (cg_wait), then every guard whose copy is done opens. frz_lock orders it with the freezes: a guarded page
// is read-only, so its 2 MiB region does not freeze; a frozen region is thawed before a range in it is guarded.
typedef struct { uint64_t lo, hi, value; } CgRange;
static CgRange *cg;
static int ncg, ccg;
static uint64_t (*cg_done)(void);
static int (*cg_wait)(uint64_t value);
static uint64_t cg_faults, cg_waits, cg_wait_ns, cg_ranges;

void mem_copy_guard_setup(uint64_t (*done)(void), int (*wait)(uint64_t value)) { cg_done = done, cg_wait = wait; }

static int cg_cmp(const void *a, const void *b) {
    const CgRange *x = a, *y = b;
    return (x->lo > y->lo) - (x->lo < y->lo);
}

// frz_lock held: drops the guards whose copy is done and opens their pages, except those a pending guard still covers
// (a pool copied whole every frame has a newer pending guard over the same pages at almost any time). Pending guards
// are sorted by start; each done one opens the gaps between the pending ones that overlap it.
static void cg_sweep_locked(void) {
    uint64_t done = cg_done ? cg_done() : 0;
    int np = 0;
    for (int i = 0; i < ncg; i++)   // pending first, in place
        if (cg[i].value > done) { CgRange t = cg[np]; cg[np] = cg[i]; cg[i] = t; np++; }
    if (np == ncg) return;
    qsort(cg, (size_t)np, sizeof *cg, cg_cmp);
    for (int i = np; i < ncg; i++) {
        uint64_t lo = cg[i].lo, hi = cg[i].hi;
        int k = 0;   // first pending guard that may overlap: ends after lo (they are sorted by start; ends vary)
        for (; k < np && cg[k].lo < hi; k++) {
            if (cg[k].hi <= lo) continue;
            if (cg[k].lo > lo) {   // the gap [lo, cg[k].lo) opens
                for (uint64_t a = lo, b; a < cg[k].lo; a = b)
                    if (plat_mem_state_walk(a, cg[k].lo, &b) == PLAT_PROT_READ && !frz[a >> FRZ_SHIFT]) plat_mem_protect(a, b - a, PLAT_PROT_RW);
            }
            if (cg[k].hi > lo) lo = cg[k].hi;
            if (lo >= hi) break;
        }
        for (uint64_t a = lo, b; a < hi; a = b)
            if (plat_mem_state_walk(a, hi, &b) == PLAT_PROT_READ && !frz[a >> FRZ_SHIFT]) plat_mem_protect(a, b - a, PLAT_PROT_RW);
    }
    ncg = np;
}

void mem_copy_guard(uint64_t addr, uint64_t n, uint64_t value) {
    if (!n || addr >= RT_ADDRESS_LIMIT || !cg_done) return;
    uint64_t lo = addr & ~(PAGE - 1), hi = (addr + n + PAGE - 1) & ~(PAGE - 1);
    plat_mutex_lock(&frz_lock);
    thaw_range_locked(lo, hi - lo, 3);
    frz_mark_odd(lo, hi - lo);
    for (uint64_t a = lo, b; a < hi; a = b)   // committed read-write runs only (a hole or another protection stays)
        if (plat_mem_state_walk(a, hi, &b) == PLAT_PROT_RW) plat_mem_protect(a, b - a, PLAT_PROT_READ);
    if (ncg == ccg) {
        ccg = ccg ? ccg * 2 : 256;
        CgRange *g = realloc(cg, sizeof *g * (size_t)ccg);
        if (!g) rt_fatal("memory: out of memory (copy guards)");
        cg = g;
    }
    cg[ncg++] = (CgRange){lo, hi, value};
    cg_ranges++;
    plat_mutex_unlock(&frz_lock);
}

void mem_copy_guard_sweep(void) {
    if (!ncg) return;
    plat_mutex_lock(&frz_lock);
    cg_sweep_locked();
    plat_mutex_unlock(&frz_lock);
}

void mem_copy_guard_stats(uint64_t *ranges, uint64_t *faults, uint64_t *waits, uint64_t *wait_ns) {
    plat_mutex_lock(&frz_lock);
    *ranges = cg_ranges, *faults = cg_faults, *waits = cg_waits, *wait_ns = cg_wait_ns;
    cg_ranges = cg_faults = cg_waits = cg_wait_ns = 0;
    plat_mutex_unlock(&frz_lock);
}

// A write into a guarded range: wait for its copy (the latest covering one), open every guard that is done, retry.
static int cg_fault(uint64_t a) {
    if (!ncg) return 0;
    plat_mutex_lock(&frz_lock);
    uint64_t v = 0;
    for (int i = 0; i < ncg; i++)
        if (a >= cg[i].lo && a < cg[i].hi && cg[i].value > v) v = cg[i].value;
    if (!v) { plat_mutex_unlock(&frz_lock); return 0; }
    cg_faults++;
    plat_mutex_unlock(&frz_lock);
    if (cg_done() < v) {
        uint64_t t0 = plat_now_ns();
        cg_wait(v);
        plat_mutex_lock(&frz_lock);
        cg_waits++, cg_wait_ns += plat_now_ns() - t0;
        plat_mutex_unlock(&frz_lock);
    }
    plat_mutex_lock(&frz_lock);
    cg_sweep_locked();
    plat_mutex_unlock(&frz_lock);
    return 1;
}

static int thaw_fault(uint64_t a) {
    if (cg_fault(a)) return 1;   // a copy guard, opened
    if (a >= RT_ADDRESS_LIMIT || !watched(a, 1)) return 0;
    plat_mutex_lock(&frz_lock);
    int was = frz[a >> FRZ_SHIFT];
    thaw_locked(a >> FRZ_SHIFT, 1);
    plat_mutex_unlock(&frz_lock);
    if (was) return 1;
    uint64_t end;
    return plat_mem_state(a & ~(PAGE - 1), (a & ~(PAGE - 1)) + PAGE, &end) == PLAT_PROT_RW;
}

// Mapping changes thaw the regions they touch and run under frz_lock, so no freeze sees them half done.
static void commit(uint64_t a, uint64_t n, const char *what) {
    plat_mutex_lock(&frz_lock);
    thaw_range_locked(a, n, 3);
    int ok = !n || plat_mem_commit(a, n);
    plat_mutex_unlock(&frz_lock);
    if (!ok) rt_fatal("memory: commit %s %#llx+%#llx failed", what, (unsigned long long)a, (unsigned long long)n);
}

static void decommit(uint64_t a, uint64_t n, const char *what) {
    plat_mutex_lock(&frz_lock);
    thaw_range_locked(a, n, 3);
    int ok = !n || plat_mem_decommit(a, n);
    plat_mutex_unlock(&frz_lock);
    if (!ok) rt_fatal("memory: decommit %s %#llx+%#llx failed", what, (unsigned long long)a, (unsigned long long)n);
}

static void protect(uint64_t a, uint64_t n, int prot, const char *what) {
    plat_mutex_lock(&frz_lock);
    thaw_range_locked(a, n, 3);
    if (prot != PLAT_PROT_RW) frz_mark_odd(a, n);
    int ok = !n || plat_mem_protect(a, n, prot);
    plat_mutex_unlock(&frz_lock);
    if (!ok) rt_fatal("memory: protect %s %#llx+%#llx failed", what, (unsigned long long)a, (unsigned long long)n);
}

// The process is re-run when a region is taken: Windows places DLLs, heaps and thread stacks at random addresses,
// and a fresh process gets a fresh layout.
static void reexec_or_fail(const char *what) {
    const char *e = cfg_get("REKARINA_REEXEC");
    int n = e ? atoi(e) : 0;
    if (n >= REEXEC_LIMIT) rt_fatal("memory: %s is not free after %d attempts", what, n);
    char buf[16];
    snprintf(buf, sizeof buf, "%d", n + 1);
    plat_setenv("REKARINA_REEXEC", buf);
    plat_reexec();
}

void mem_init(void) {
    if (L.pool) return;
    // Fixed window: modules, kernel region, host-function addresses.
    mods_lo = MOD_MAIN_BASE;
    mods_hi = align_up(rt_layout[rt_layout_count - 1].base + rt_layout[rt_layout_count - 1].size, 0x10000);
    if (mods_hi > RT_KERNEL_BASE) rt_fatal("memory: modules end at %#llx, inside the kernel region",
                                           (unsigned long long)mods_hi);
    if (!reserve_at(mods_lo, mods_hi - mods_lo, 0)) reexec_or_fail("the module window");
    if (!reserve_at(RT_KERNEL_BASE, RT_KERNEL_SIZE, 0)) reexec_or_fail("the kernel region");
    if (!reserve_at(RT_HOSTFN_BASE, RT_HOSTFN_SIZE, 0)) reexec_or_fail("the host-function window");

    // Stack and heap block, then the alias region above it, both searched below the module window.
    uint64_t low = 0, alias = 0;
    for (uint64_t at = LOW_SEARCH_FROM; at + LOW_SIZE <= mods_lo && !low; at += GiB)
        if (reserve_at(at, LOW_SIZE, 1)) low = at;
    if (!low) reexec_or_fail("a 6 GiB stack and heap block below 2^39");
    for (uint64_t at = align_up(low + LOW_SIZE, ALIAS_ALIGN); at + ALIAS_SIZE <= mods_lo && !alias; at += ALIAS_ALIGN)
        if (reserve_at(at, ALIAS_SIZE, 1)) alias = at;
    if (!alias) reexec_or_fail("a 64 GiB alias region below 2^39");

    plat_mem_fault_hook(thaw_fault);   // before the crash handler
    {
        const char *e = cfg_get("REKARINA_FREEZE_WHOLE");
        frz_direct = !e || *e != '0';
    }
    alias_pages = calloc(ALIAS_PAGES, 1);
    if (!alias_pages) rt_fatal("memory: cannot allocate the alias page map");
    // totk.ini is read here, once the address space is claimed and before the first setting: the pool size below
    // depends on REKARINA_APP_POOL and on the resolution settings (resolution.c decides them once, on this first call).
    cfg_init();
    const char *pool = cfg_get("REKARINA_APP_POOL");
    L = (RtGuestLayout){
        .aslr = {low, RT_ADDRESS_LIMIT - low},
        .stack = {low + STACK_OFF, STACK_SIZE},
        .heap = {low + HEAP_OFF, HEAP_CAP},
        .alias = {alias, ALIAS_SIZE},
        .kernel = {RT_KERNEL_BASE, RT_KERNEL_SIZE},
        .hostfn = {RT_HOSTFN_BASE, RT_HOSTFN_SIZE},
        .pool = pool ? strtoull(pool, NULL, 0) : DEFAULT_POOL + res_heap_extra_bytes(),   // resolution.c
    };
    if (!L.pool) rt_fatal("memory: REKARINA_APP_POOL is 0");
}

const RtGuestLayout *mem_layout(void) { return &L; }

// ---- GPU result watch (host.h). Pages by guest page number in an open-addressed table; a page is in it while it is
// inaccessible. frz_lock orders the protection changes with the freezes (a region with such a page does not freeze;
// a frozen region is thawed before a page in it is closed).
#define RW_N 131072   // (was 8192: the hold test below closes every page the GPU writes)
#define RW_SHIFT 47   // 64 - log2(RW_N)
#define RW_READERS 64
static struct { uint64_t page, fence, frame; int guard; uint8_t lz; } rw[RW_N];   // lz: mem_lazy_close (1 watched, 2 cold)
static struct { uint64_t rva, n, early, ready, writes, frames; int lines; } rw_rd[RW_READERS];
static int rw_nrd, rw_lines;
static uint64_t rw_closed, rw_released, rw_faults, rw_full;
static volatile int rw_on = -1;
static uint64_t (*rw_done)(void), (*rw_frame)(void);
// The result guard (mem_result_guard): a guest thread's access to a page whose GPU result is still in flight waits
// for that result (rw_wait), as the console's CPU, reading memory the GPU writes, sees the value once the GPU wrote
// it. Host threads (completion, recorder) never wait: their access opens the page.
static volatile int rw_guard = -1;
// Lazy write-back (vk_memory.c "write-back on demand"): a page the GPU writes and the CPU has not been seen to touch is
// closed too. Its first access calls the hook (which brings a cold page's bytes over from the mirror) and opens it.
static volatile int rw_lz_on;
static int rw_hooked;
static void (*rw_lz_hook)(uint64_t page_addr, int cold);
static int (*rw_wait)(uint64_t fence);
static int (*rw_guest)(void);
static uint64_t rg_waits, rg_wait_ns, rg_opened;
static struct { uint64_t rva, page, n, ns; } rg_who[48];   // DEBUG-GUARDWHO: the waits by instruction and page
static int rg_nwho;

static int rw_slot(uint64_t page, int insert) {   // frz_lock held
    uint64_t h = (page * 0x9E3779B97F4A7C15ull) >> RW_SHIFT;
    for (int i = 0; i < RW_N; i++) {
        int k = (int)((h + (uint64_t)i) & (RW_N - 1));
        if (rw[k].page == page) return k;
        if (!rw[k].page) return insert ? k : -1;
    }
    return -1;
}

static void rw_remove(int k) {   // frz_lock held; keeps the probe chains (backward shift)
    int i = k;
    for (;;) {
        rw[i].page = 0;
        int j = i;
        for (;;) {
            j = (j + 1) & (RW_N - 1);
            if (!rw[j].page) return;
            int h = (int)(((rw[j].page * 0x9E3779B97F4A7C15ull) >> RW_SHIFT) & (RW_N - 1));
            if ((j > i && (h <= i || h > j)) || (j < i && (h <= i && h > j))) break;
        }
        rw[i] = rw[j];
        i = j;
    }
}

int mem_result_watch_on(void) {
    if (rw_on < 0) { const char *e = cfg_get("REKARINA_RESULT_WATCH"); rw_on = e && *e == '1'; }
    return rw_on;
}

int mem_result_guard_on(void) {
    if (rw_guard < 0) rw_guard = rt_knob("RESULT_GUARD", "0");
    return rw_guard;
}

void mem_result_set_wait(int (*wait)(uint64_t fence), int (*guest)(void)) { rw_wait = wait, rw_guest = guest; }

static int result_fault(uint64_t addr, int kind, uint64_t rva);

void mem_result_setup(uint64_t (*fence_done)(void), uint64_t (*frame)(void)) {
    rw_done = fence_done, rw_frame = frame;
    if (mem_result_guard_on()) {
        if (!mem_result_watch_on() && !rw_hooked++) plat_mem_access_hook(result_fault);
        rt_log("memory: GPU result guard on: a guest access to a small GPU-written range waits for its submission "
               "(REKARINA_RESULT_GUARD)");
    }
    if (mem_result_watch_on()) {
        if (!rw_hooked++) plat_mem_access_hook(result_fault);
        rt_log("memory: GPU result watch on: the first guest access to a counter or event before its value is written is "
               "logged (REKARINA_RESULT_WATCH)");
    }
}

void mem_lazy_setup(void (*hook)(uint64_t page_addr, int cold)) {
    plat_mutex_lock(&frz_lock);
    rw_lz_hook = hook;
    if (!rw_hooked++) plat_mem_access_hook(result_fault);
    rw_lz_on = 1;
    plat_mutex_unlock(&frz_lock);
}

// Closes the page at addr for the lazy write-back (cold: its bytes are only in the mirror now). 1: it is closed and
// marked; 0: it could not be (not read-write, or the table is full), and the caller keeps writing it back.
int mem_lazy_close(uint64_t addr, int cold) {
    if (!rw_lz_on || addr >= RT_ADDRESS_LIMIT) return 0;
    uint64_t p = addr >> 12;
    int ok = 0;
    plat_mutex_lock(&frz_lock);
    int k = rw_slot(p, 1);
    if (k >= 0 && rw[k].page == p) {
        if (rw[k].lz < (cold ? 2 : 1)) rw[k].lz = cold ? 2 : 1;
        ok = 1;
    } else if (k >= 0) {
        thaw_range_locked(p << 12, PAGE, 3);
        frz_mark_odd(p << 12, PAGE);
        if (plat_mem_protect_page_from(p << 12, PLAT_PROT_RW, PLAT_PROT_NONE)) {
            rw[k].page = p, rw[k].fence = 0, rw[k].frame = rw_frame ? rw_frame() : 0, rw[k].guard = 0, rw[k].lz = cold ? 2 : 1;
            ok = 1;
        }
    }
    plat_mutex_unlock(&frz_lock);
    return ok;
}

// mem_lazy_close(page, 0) for the n pages from addr on, with one protection change per run of read-write pages that
// are not in the table yet. A scene's first frames close some 10,000 pages (every page of the ranges shaders write
// there for the first time); one VirtualProtect a page, each interrupting every core the process runs on, was 150 to
// 170 ms of the scene's first frame and 100 ms of the next (2026-11-11).
void mem_lazy_close_run(uint64_t addr, uint32_t n, uint8_t *ok) {
    memset(ok, 0, n);
    if (!rw_lz_on || addr >= RT_ADDRESS_LIMIT) return;
    uint64_t p0 = addr >> 12;
    plat_mutex_lock(&frz_lock);
    for (uint32_t i = 0; i < n;) {
        int k = rw_slot(p0 + i, 0);
        if (k >= 0) {   // closed already: marked
            if (rw[k].lz < 1) rw[k].lz = 1;
            ok[i++] = 1;
            continue;
        }
        uint32_t j = i + 1;
        while (j < n && rw_slot(p0 + j, 0) < 0) j++;
        uint64_t a = (p0 + i) << 12;
        thaw_range_locked(a, (uint64_t)(j - i) << 12, 3);
        frz_mark_odd(a, (uint64_t)(j - i) << 12);
        uint32_t done = plat_mem_protect_run_from(a, j - i, PLAT_PROT_RW, PLAT_PROT_NONE);
        uint64_t frame = rw_frame ? rw_frame() : 0;
        for (uint32_t q = 0; q < done; q++) {
            int s = rw_slot(p0 + i + q, 1);
            if (s < 0) {   // the table is full: the page opens again and is written back as ever
                plat_mem_protect((p0 + i + q) << 12, PAGE, PLAT_PROT_RW);
                continue;
            }
            rw[s].page = p0 + i + q, rw[s].fence = 0, rw[s].frame = frame, rw[s].guard = 0, rw[s].lz = 1;
            ok[i + q] = 1;
        }
        i += done ? done : 1;   // the page the run stopped at is not read-write: left as it is
    }
    plat_mutex_unlock(&frz_lock);
}

// Opens the lazily closed pages of [addr, addr + n) as they are (the caller brought their bytes over, or drops them).
void mem_lazy_open(uint64_t addr, uint64_t n) {
    if (!rw_lz_on || !n || addr >= RT_ADDRESS_LIMIT) return;
    plat_mutex_lock(&frz_lock);
    for (uint64_t p = addr >> 12; p <= (addr + n - 1) >> 12; p++) {
        int k = rw_slot(p, 0);
        if (k < 0 || !rw[k].lz) continue;
        plat_mem_protect(p << 12, PAGE, PLAT_PROT_RW);
        rw_remove(k);
    }
    plat_mutex_unlock(&frz_lock);
}

// Before the kernel writes into [addr, addr + n) (a file read): its lazily closed pages are touched as an access would.
static void lazy_touch_range(uint64_t addr, uint64_t n) {
    if (!rw_lz_on || !n || addr >= RT_ADDRESS_LIMIT) return;
    for (uint64_t p = addr >> 12; p <= (addr + n - 1) >> 12; p++) {
        plat_mutex_lock(&frz_lock);
        int k = rw_slot(p, 0), lz = k >= 0 ? rw[k].lz : 0;
        plat_mutex_unlock(&frz_lock);
        if (!lz) continue;
        if (rw_lz_hook) rw_lz_hook(p << 12, lz == 2);
        mem_lazy_open(p << 12, PAGE);
    }
}

static void rw_close(uint64_t addr, uint64_t n, uint64_t fence, int guard);
void mem_result_watch(uint64_t addr, uint64_t n, uint64_t fence) {
    if (!rw_on || !n || addr >= RT_ADDRESS_LIMIT) return;
    {   // REKARINA_RESULT_WATCH_FROM=<frame>: nothing is watched before that frame (the loads crawl under the watch)
        static int64_t from = -1;
        if (from < 0) { const char *e = cfg_get("REKARINA_RESULT_WATCH_FROM"); from = e ? atoll(e) : 0; }
        if (rw_frame && rw_frame() < (uint64_t)from) return;
    }
    rw_close(addr, n, fence, 0);
}

void mem_result_guard(uint64_t addr, uint64_t n, uint64_t fence) {
    if (rw_guard <= 0 || !n || addr >= RT_ADDRESS_LIMIT) return;
    if (rw_done && rw_done() >= fence) return;   // already written
    rw_close(addr, n, fence, 1);
}

static void rw_close(uint64_t addr, uint64_t n, uint64_t fence, int guard) {
    plat_mutex_lock(&frz_lock);
    for (uint64_t p = addr >> 12; p <= (addr + n - 1) >> 12; p++) {
        int k = rw_slot(p, 1);
        if (k < 0) { rw_full++; continue; }
        if (rw[k].page == p) {
            if (fence > rw[k].fence) rw[k].fence = fence;
            rw[k].guard |= guard;
            continue;
        }
        uint64_t a = p << 12;
        thaw_range_locked(a, PAGE, 3);
        frz_mark_odd(a, PAGE);
        if (!plat_mem_protect_page_from(a, PLAT_PROT_RW, PLAT_PROT_NONE)) continue;
        rw[k].page = p, rw[k].fence = fence, rw[k].frame = rw_frame ? rw_frame() : 0, rw[k].guard = guard;
        rw_closed++;
    }
    plat_mutex_unlock(&frz_lock);
}

// DEBUG-TRAP: closes the page of addr now (the result watch's fault hook logs and opens it at the next access), past
// REKARINA_RESULT_WATCH_FROM; for finding who writes a value (REKARINA_RESULT_WATCH=1 with a large _FROM keeps the rest quiet).
void mem_trap_page(uint64_t addr) {
    if (!rw_on || addr >= RT_ADDRESS_LIMIT) return;
    plat_mutex_lock(&frz_lock);
    uint64_t p = addr >> 12, end;
    int k = rw_slot(p, 1);
    if (k >= 0 && rw[k].page != p) {
        thaw_range_locked(p << 12, PAGE, 3);
        frz_mark_odd(p << 12, PAGE);
        if (plat_mem_state(p << 12, (p << 12) + PAGE, &end) == PLAT_PROT_RW && plat_mem_protect(p << 12, PAGE, PLAT_PROT_NONE))
            rw[k].page = p, rw[k].fence = UINT64_MAX, rw[k].frame = rw_frame ? rw_frame() : 0, rw_closed++;
    }
    plat_mutex_unlock(&frz_lock);
}

void mem_result_release(uint64_t addr, uint64_t n) {
    if ((rw_on <= 0 && rw_guard <= 0) || !n || addr >= RT_ADDRESS_LIMIT) return;
    {   // DEBUG-WATCHHOLD: REKARINA_RESULT_WATCH_HOLD=1: only an access opens a watched page
        static int hold = -1;
        if (hold < 0) { const char *e = cfg_get("REKARINA_RESULT_WATCH_HOLD"); hold = e && *e == '1'; }
        if (hold) return;
    }
    plat_mutex_lock(&frz_lock);
    for (uint64_t p = addr >> 12; p <= (addr + n - 1) >> 12; p++) {
        int k = rw_slot(p, 0);
        if (k < 0 || rw[k].lz) continue;   // a lazily closed page opens at its first access only
        plat_mem_protect(p << 12, PAGE, PLAT_PROT_RW);
        rw_remove(k);
        rw_released++;
    }
    plat_mutex_unlock(&frz_lock);
}

// Any access violation: a watched page is logged (who, early or not) and opened; anything else goes on.
static int result_fault(uint64_t addr, int kind, uint64_t rva) {
    if ((rw_on <= 0 && rw_guard <= 0 && !rw_lz_on) || addr >= RT_ADDRESS_LIMIT) return 0;
    uint64_t p = addr >> 12, end;
    plat_mutex_lock(&frz_lock);
    int k = rw_slot(p, 0);
    if (k < 0) {   // not ours, or released meanwhile by another thread: retry only if this access would pass now (a
        // write to a frozen, read-only page goes on to thaw_fault; claiming it here looped on the fault for good)
        int st = plat_mem_state(p << 12, (p << 12) + PAGE, &end);
        plat_mutex_unlock(&frz_lock);
        return kind == 1 ? st == PLAT_PROT_RW : kind == 0 && st >= PLAT_PROT_READ;
    }
    if (rw[k].lz && rw_on <= 0) {   // lazy write-back: the hook fetches a cold page's bytes, then the page opens
        int cold = rw[k].lz == 2;
        plat_mutex_unlock(&frz_lock);
        if (rw_lz_hook) rw_lz_hook(p << 12, cold);
        mem_lazy_open(p << 12, PAGE);
        return 1;
    }
    uint64_t done = rw_done ? rw_done() : 0, fr = rw_frame ? rw_frame() : 0;
    int early = done < rw[k].fence;
    if (rw[k].guard && early && rw_wait && rw_guest && rw_guest()) {   // the guard: wait for the result, then retry
        uint64_t f = rw[k].fence, t0 = plat_now_ns();
        plat_mutex_unlock(&frz_lock);
        int ok = rw_wait(f);
        plat_mutex_lock(&frz_lock);
        rg_waits++, rg_wait_ns += plat_now_ns() - t0;
        {   // DEBUG-GUARDWHO
            int w = 0;
            while (w < rg_nwho && (rg_who[w].rva != rva || rg_who[w].page != p)) w++;
            if (w == rg_nwho && rg_nwho < 48) rg_who[rg_nwho++] = (typeof(rg_who[0])){rva, p, 0, 0};
            if (w < rg_nwho) rg_who[w].n++, rg_who[w].ns += plat_now_ns() - t0;
        }
        if (ok) {   // the completion released the page (or another thread opened it): the access goes again
            plat_mutex_unlock(&frz_lock);
            return 1;
        }
        k = rw_slot(p, 0);
        if (k < 0) {
            plat_mutex_unlock(&frz_lock);
            return 1;
        }
    }
    if (!rw_on) {   // guard only, a host thread or a result already written: open the page, no log
        plat_mem_protect(p << 12, PAGE, PLAT_PROT_RW);
        rw_remove(k);
        rg_opened++;
        plat_mutex_unlock(&frz_lock);
        return 1;
    }
    int i = 0;
    while (i < rw_nrd && rw_rd[i].rva != rva) i++;
    if (i == rw_nrd && rw_nrd < RW_READERS) rw_rd[rw_nrd++] = (typeof(rw_rd[0])){rva, 0, 0, 0, 0, 0, 0};
    if (i < rw_nrd) {
        rw_rd[i].n++, rw_rd[i].early += early, rw_rd[i].ready += !early, rw_rd[i].writes += kind == 1;
        rw_rd[i].frames += fr - rw[k].frame;
    }
    rw_faults++;
    if (rw_lines < 400 && i < rw_nrd && rw_rd[i].lines < 24) {   // up to 24 lines per instruction
        rw_lines++, rw_rd[i].lines++;
        rt_log("RESULTWATCH frame %llu %s at %#llx by rva %#llx: result of fence %llu (GPU done %llu: %s), recorded in frame "
               "%llu", (unsigned long long)fr, kind == 1 ? "write" : kind == 8 ? "exec" : "read", (unsigned long long)addr,
               (unsigned long long)rva, (unsigned long long)rw[k].fence, (unsigned long long)done,
               early ? "EARLY" : "ready, not written yet", (unsigned long long)rw[k].frame);
    }
    plat_mem_protect(p << 12, PAGE, PLAT_PROT_RW);
    rw_remove(k);
    plat_mutex_unlock(&frz_lock);
    return 1;
}

void mem_result_log(uint64_t frame) {
    if (rw_guard > 0) {
        plat_mutex_lock(&frz_lock);
        rt_log("memory: result guard frame %llu: pages closed %llu, opened by the completion %llu, waits %llu (%.2f ms in "
               "all), opened without a wait %llu, table full %llu", (unsigned long long)frame, (unsigned long long)rw_closed,
               (unsigned long long)rw_released, (unsigned long long)rg_waits, rg_wait_ns / 1e6, (unsigned long long)rg_opened,
               (unsigned long long)rw_full);
        rw_closed = rw_released = rg_waits = rg_wait_ns = rg_opened = rw_full = 0;
        for (int w = 0; w < rg_nwho; w++)   // DEBUG-GUARDWHO
            if (rg_who[w].n)
                rt_log("GUARDWHO rva %#llx page %#llx: %llu waits, %.2f ms each", (unsigned long long)rg_who[w].rva,
                       (unsigned long long)(rg_who[w].page << 12), (unsigned long long)rg_who[w].n,
                       rg_who[w].ns / 1e6 / (double)rg_who[w].n);
        rg_nwho = 0;
        plat_mutex_unlock(&frz_lock);
    }
    if (rw_on <= 0) return;
    plat_mutex_lock(&frz_lock);
    char line[1200];
    size_t o = 0;
    for (int i = 0; i < rw_nrd && o < sizeof line - 96; i++)
        o += (size_t)snprintf(line + o, sizeof line - o, " rva %#llx: %llu (%llu early, %llu writes, %.1f frames after)",
                              (unsigned long long)rw_rd[i].rva, (unsigned long long)rw_rd[i].n,
                              (unsigned long long)rw_rd[i].early, (unsigned long long)rw_rd[i].writes,
                              rw_rd[i].n ? (double)rw_rd[i].frames / (double)rw_rd[i].n : 0.0);
    rt_log("memory: result watch frame %llu: pages closed %llu, opened by the completion %llu, by an access %llu, table "
           "full %llu; accesses by instruction:%s", (unsigned long long)frame, (unsigned long long)rw_closed,
           (unsigned long long)rw_released, (unsigned long long)rw_faults, (unsigned long long)rw_full, o ? line : " none");
    plat_mutex_unlock(&frz_lock);
}

// A range inside one watched reservation (stack and heap block, or alias region) only.
static uint64_t written_pages(uint64_t addr, uint64_t size, uint64_t *pages, uint64_t max, int reset) {
    if (!max || !size) return 0;
    if (!watched(addr, size)) return UINT64_MAX;
    return plat_mem_written(addr, size, pages, max, reset);
}

uint64_t mem_written_pages(uint64_t addr, uint64_t size, uint64_t *pages, uint64_t max) {
    return written_pages(addr, size, pages, max, 1);
}

uint64_t mem_written_peek(uint64_t addr, uint64_t size, uint64_t *pages, uint64_t max) {
    return written_pages(addr, size, pages, max, 0);
}

static uint64_t image_bytes(void) {
    uint64_t n = 0;
    for (int i = 0; i < ldr_count(); i++) n += ldr_module(i)->size;
    return align_up(n, PAGE);
}

static int overlaps(uint64_t a, uint64_t n, uint64_t b, uint64_t m) { return a < b + m && b < a + n; }
static int inside(uint64_t a, uint64_t n, RtRange r) { return a >= r.base && n <= r.size && a - r.base <= r.size - n; }

// ---- 0x01 SetHeapSize
uint32_t mem_set_heap_size(uint64_t size, uint64_t *addr) {
    plat_mutex_lock(&lock);
    if (size > HEAP_CAP || size % 0x200000 || size + image_bytes() > L.pool) {
        plat_mutex_unlock(&lock);
        return R_INVALID_STATE;
    }
    if (size > heap_size) commit(L.heap.base + heap_size, size - heap_size, "heap");
    if (size < heap_size) decommit(L.heap.base + size, heap_size - size, "heap");
    heap_size = size;
    plat_mutex_unlock(&lock);
    if (addr) *addr = L.heap.base;
    return R_OK;
}

// ---- module data permissions (0x02, 0x03) and module pages for QueryMemory
static int module_index(uint64_t a) {
    for (int i = 0; i < ldr_count(); i++) {
        const RtLoaded *m = ldr_module(i);
        if (a - m->base < m->size) return i;
    }
    return -1;
}

static uint8_t *perm_table(int i) {   // under lock
    if (!module_perm[i]) {
        const RtLoaded *m = ldr_module(i);
        size_t n = (m->size - m->data_off) / PAGE;
        module_perm[i] = malloc(n);
        if (!module_perm[i]) rt_fatal("memory: module permission table");
        memset(module_perm[i], PERM_RW, n);
    }
    return module_perm[i];
}

static unsigned module_page_perm(int i, uint64_t off) {   // under lock
    const RtLoaded *m = ldr_module(i);
    if (off < m->data_off) return off < align_up(m->text_end, PAGE) ? PERM_RX : PERM_R;
    return module_perm[i] ? module_perm[i][(off - m->data_off) / PAGE] : PERM_RW;
}

// The module whose data segment holds [a, a+n), or -1.
static int module_data(uint64_t a, uint64_t n) {
    int i = module_index(a);
    if (i < 0) return -1;
    const RtLoaded *m = ldr_module(i);
    uint64_t off = a - m->base;
    return off >= m->data_off && n <= m->size - off ? i : -1;
}

uint32_t mem_set_permission(uint64_t a, uint64_t n, uint32_t perm) {
    if (a & (PAGE - 1)) return R_INVALID_ADDRESS;
    if (!n || (n & (PAGE - 1))) return R_INVALID_SIZE;
    if (perm != PERM_NONE && perm != PERM_R && perm != PERM_RW) return R_INVALID_NEW_PERM;
    int i = module_data(a, n);
    if (i < 0) return R_INVALID_STATE;
    const RtLoaded *m = ldr_module(i);
    plat_mutex_lock(&lock);
    uint8_t *t = perm_table(i) + (a - m->base - m->data_off) / PAGE;
    for (uint64_t p = 0; p < n / PAGE; p++)
        if (t[p] & ATTR_PERMISSION_LOCKED) {
            plat_mutex_unlock(&lock);
            return R_INVALID_STATE;
        }
    memset(t, (int)perm, n / PAGE);
    protect(a, n, perm == PERM_RW ? PLAT_PROT_RW : perm == PERM_R ? PLAT_PROT_READ : PLAT_PROT_NONE, "module data");
    plat_mutex_unlock(&lock);
    rt_log("%s data permission +%#llx size %#llx perm %u", m->name, (unsigned long long)(a - m->base),
           (unsigned long long)n, perm);
    return R_OK;
}

// The only module attribute the SDK sets is the irreversible RELRO lock.
uint32_t mem_set_attribute(uint64_t a, uint64_t n, uint32_t mask, uint32_t at) {
    if (a & (PAGE - 1)) return R_INVALID_ADDRESS;
    if (!n || (n & (PAGE - 1))) return R_INVALID_SIZE;
    if (mask != ATTR_PERMISSION_LOCKED || at != ATTR_PERMISSION_LOCKED) return R_INVALID_NEW_PERM;
    int i = module_data(a, n);
    if (i < 0) return R_INVALID_STATE;
    const RtLoaded *m = ldr_module(i);
    plat_mutex_lock(&lock);
    uint8_t *t = perm_table(i) + (a - m->base - m->data_off) / PAGE;
    for (uint64_t p = 0; p < n / PAGE; p++) t[p] |= ATTR_PERMISSION_LOCKED;
    plat_mutex_unlock(&lock);
    return R_OK;
}

// ---- 0x04/0x05 MapMemory, UnmapMemory: thread stacks move from heap or alias into the stack region by copy; the
// source is inaccessible while moved and gets the contents back on unmap.
static int alias_all_mapped(uint64_t a, uint64_t n) {   // under lock
    uint64_t first = (a - L.alias.base) / PAGE;
    for (uint64_t i = 0; i < n / PAGE; i++)
        if (!alias_pages[first + i]) return 0;
    return 1;
}

static uint32_t stack_move(int unmap, uint64_t dst, uint64_t src, uint64_t n) {
    if (!n || ((dst | src | n) & (PAGE - 1))) return R_INVALID_ADDRESS;
    if (!inside(dst, n, L.stack)) return R_INVALID_REGION;
    int in_heap = inside(src, n, (RtRange){L.heap.base, heap_size});
    int in_alias = inside(src, n, L.alias);
    if (!in_heap && !in_alias) return R_INVALID_REGION;
    plat_mutex_lock(&lock);
    uint32_t r = R_OK;
    if (in_alias && !alias_all_mapped(src, n)) {
        r = R_INVALID_REGION;
        goto out;
    }
    unsigned found = nstack_maps;
    for (unsigned i = 0; i < nstack_maps; i++)
        if (stack_maps[i].dst == dst && stack_maps[i].src == src && stack_maps[i].size == n) found = i;
    if (unmap) {
        if (found == nstack_maps) {
            r = R_INVALID_STATE;
            goto out;
        }
        protect(src, n, PLAT_PROT_RW, "moved stack source");
        memcpy((void *)(uintptr_t)src, (const void *)(uintptr_t)dst, n);
        decommit(dst, n, "moved stack");
        stack_maps[found] = stack_maps[--nstack_maps];
    } else {
        if (nstack_maps == MAX_STACK_MAPS) {
            r = R_INVALID_STATE;
            goto out;
        }
        for (unsigned i = 0; i < nstack_maps; i++)
            if (overlaps(dst, n, stack_maps[i].dst, stack_maps[i].size) ||
                overlaps(src, n, stack_maps[i].src, stack_maps[i].size)) {
                r = R_INVALID_REGION;
                goto out;
            }
        commit(dst, n, "moved stack");
        memcpy((void *)(uintptr_t)dst, (const void *)(uintptr_t)src, n);
        protect(src, n, PLAT_PROT_NONE, "moved stack source");
        stack_maps[nstack_maps++] = (StackMap){dst, src, n};
    }
out:
    plat_mutex_unlock(&lock);
    return r;
}

uint32_t mem_map(uint64_t dst, uint64_t src, uint64_t n) { return stack_move(0, dst, src, n); }
uint32_t mem_unmap(uint64_t dst, uint64_t src, uint64_t n) { return stack_move(1, dst, src, n); }

// ---- 0x2C/0x2D MapPhysicalMemory, UnmapPhysicalMemory. Horizon maps only the still-unmapped pages of the range and
// leaves mapped pages and their contents alone; TotK's heaps grow by re-mapping ranges they partly own.
static void alias_runs(uint64_t first, uint64_t count, uint8_t want, int do_commit) {   // under lock
    uint64_t i = 0;
    while (i < count) {
        if (alias_pages[first + i] != want) { i++; continue; }
        uint64_t j = i;
        while (j < count && alias_pages[first + j] == want) j++;
        uint64_t a = L.alias.base + (first + i) * PAGE, n = (j - i) * PAGE;
        if (do_commit) commit(a, n, "physical memory");
        else decommit(a, n, "physical memory");
        memset(alias_pages + first + i, !want, j - i);
        i = j;
    }
}

static uint32_t alias_map(int unmap, uint64_t a, uint64_t n) {
    if (!n || ((a | n) & (PAGE - 1))) return R_INVALID_ADDRESS;
    if (!inside(a, n, L.alias)) return R_INVALID_REGION;
    uint64_t first = (a - L.alias.base) / PAGE, count = n / PAGE;
    plat_mutex_lock(&lock);
    uint32_t r = R_OK;
    if (unmap) {
        for (unsigned i = 0; i < nstack_maps; i++)
            if (overlaps(a, n, stack_maps[i].src, stack_maps[i].size)) {
                r = R_INVALID_REGION;
                goto out;
            }
        uint64_t released = 0;
        for (uint64_t i = 0; i < count; i++) released += alias_pages[first + i];
        alias_runs(first, count, 1, 0);
        alias_bytes -= released * PAGE;
    } else {
        uint64_t fresh = 0;
        for (uint64_t i = 0; i < count; i++) fresh += !alias_pages[first + i];
        if (fresh * PAGE + alias_bytes + heap_size + image_bytes() > L.pool) {
            r = R_LIMIT_REACHED;   // the SDK maps LimitReached to its recoverable out-of-memory result
            goto out;
        }
        alias_runs(first, count, 0, 1);
        alias_bytes += fresh * PAGE;
    }
out:
    plat_mutex_unlock(&lock);
    if (r) rt_log("%sMapPhysicalMemory %#llx+%#llx failed %#x (alias %#llx used, heap %#llx)", unmap ? "Un" : "",
                  (unsigned long long)a, (unsigned long long)n, r, (unsigned long long)alias_bytes,
                  (unsigned long long)heap_size);
    return r;
}

uint32_t mem_map_physical(uint64_t a, uint64_t n) { return alias_map(0, a, n); }
uint32_t mem_unmap_physical(uint64_t a, uint64_t n) { return alias_map(1, a, n); }

// ---- 0x06 QueryMemory
// The region around a that no known region covers: everything else below 2^39 belongs to the host and is reported
// as taken (Code, RX), so the SDK never picks it.
static void taken_gap(uint64_t a, uint64_t *base, uint64_t *size) {
    uint64_t lo = 0, hi = RT_ADDRESS_LIMIT;
    RtRange known[3 + MOD_COUNT + 2];
    int n = 0;
    known[n++] = (RtRange){L.aslr.base, LOW_SIZE};
    known[n++] = L.alias;
    known[n++] = L.kernel;
    known[n++] = L.hostfn;
    for (int i = 0; i < ldr_count(); i++) known[n++] = (RtRange){ldr_module(i)->base, ldr_module(i)->size};
    for (int i = 0; i < n; i++) {
        uint64_t e = known[i].base + known[i].size;
        if (e <= a && e > lo) lo = e;
        if (known[i].base > a && known[i].base < hi) hi = known[i].base;
    }
    *base = lo;
    *size = hi - lo;
}

static uint64_t run_end(const uint8_t *p, uint64_t i, uint64_t n, uint8_t v) {
    while (i < n && (i & 7)) { if (p[i] != v) return i; i++; }
    uint64_t w = v ? 0x0101010101010101ull : 0;
    while (i + 8 <= n && *(const uint64_t *)(p + i) == w) i += 8;
    while (i < n && p[i] == v) i++;
    return i;
}

static uint64_t run_start(const uint8_t *p, uint64_t i, uint8_t v) {   // first index of the run that ends at i
    while (i > 0 && (i & 7)) { if (p[i - 1] != v) return i; i--; }
    uint64_t w = v ? 0x0101010101010101ull : 0;
    while (i >= 8 && *(const uint64_t *)(p + i - 8) == w) i -= 8;
    while (i > 0 && p[i - 1] == v) i--;
    return i;
}

uint32_t mem_query(uint64_t a, RtMemoryInfo *out) {
    RtMemoryInfo r = {.base = a & ~(PAGE - 1), .size = PAGE};
    uint64_t low = L.aslr.base;
    plat_mutex_lock_shared(&lock);
    int mi;
    if (a >= RT_ADDRESS_LIMIT) {
        r.base = RT_ADDRESS_LIMIT;
        r.size = 0 - RT_ADDRESS_LIMIT;
        r.state = ST_INACCESSIBLE;
    } else if (a < low) {
        r.base = 0;
        r.size = low;
        r.state = ST_FREE;
    } else if (a < low + LOW_SIZE) {
        uint64_t heap = L.heap.base;
        if (a >= heap && a < heap + heap_size) {
            r = (RtMemoryInfo){.base = heap, .size = heap_size, .state = ST_NORMAL, .perm = PERM_RW};
        } else if (a < heap) {
            r = (RtMemoryInfo){.base = low, .size = heap - low, .state = ST_FREE};
        } else {
            r = (RtMemoryInfo){.base = heap + heap_size, .size = low + LOW_SIZE - heap - heap_size, .state = ST_FREE};
        }
    } else if (a - L.alias.base < L.alias.size) {
        uint64_t p = (a - L.alias.base) / PAGE;
        uint8_t v = alias_pages[p];
        uint64_t s = run_start(alias_pages, p, v), e = run_end(alias_pages, p, ALIAS_PAGES, v);
        r = (RtMemoryInfo){.base = L.alias.base + s * PAGE, .size = (e - s) * PAGE, .state = v ? ST_NORMAL : ST_FREE,
                           .perm = v ? PERM_RW : PERM_NONE};
    } else if ((mi = module_index(a)) >= 0) {
        const RtLoaded *m = ldr_module(mi);
        uint64_t off = (a - m->base) & ~(PAGE - 1), lo = off, hi = off + PAGE;
        unsigned perm = module_page_perm(mi, off);
        int data = off >= m->data_off;
        uint64_t limit = data ? m->data_off : 0, end = data ? m->size : m->data_off;
        while (lo > limit && module_page_perm(mi, lo - PAGE) == perm) lo -= PAGE;
        while (hi < end && module_page_perm(mi, hi) == perm) hi += PAGE;
        r = (RtMemoryInfo){.base = m->base + lo, .size = hi - lo, .state = data ? ST_CODEDATA : ST_CODE,
                           .perm = perm & 7, .attr = perm & ATTR_PERMISSION_LOCKED};
    } else if (a - L.kernel.base < L.kernel.size) {
        r = (RtMemoryInfo){.base = L.kernel.base, .size = L.kernel.size, .state = ST_THREADLOCAL, .perm = PERM_RW};
    } else if (a - L.hostfn.base < L.hostfn.size) {
        r = (RtMemoryInfo){.base = L.hostfn.base, .size = L.hostfn.size, .state = ST_INACCESSIBLE};
    } else {
        taken_gap(a, &r.base, &r.size);
        r.state = ST_CODE;
        r.perm = PERM_RX;
#ifdef GAME_NS2
        // A gap between two modules (their 2 MiB alignment) is free on Horizon. Answered as code, the unwinder's
        // module search (nn::ro::detail::FindModuleHeader, behind every C++ throw) read its reserved, uncommitted
        // pages and faulted (OoT's first exception, from system_clock::now).
        if (ldr_count() && a > ldr_module(0)->base &&
            a < ldr_module(ldr_count() - 1)->base + ldr_module(ldr_count() - 1)->size)
            r.state = ST_FREE, r.perm = PERM_NONE;
#endif
    }
    // Moved stacks: the destination is Stack, the source Normal without access; free and normal regions around them
    // are clipped (MNG services.c).
    for (unsigned i = 0; i < nstack_maps; i++) {
        const StackMap *m = &stack_maps[i];
        uint64_t end = m->dst + m->size, send = m->src + m->size;
        if (a >= m->dst && a < end) {
            r = (RtMemoryInfo){.base = m->dst, .size = m->size, .state = ST_STACK, .perm = PERM_RW};
            break;
        }
        if (a >= m->src && a < send) {
            r = (RtMemoryInfo){.base = m->src, .size = m->size, .state = ST_NORMAL, .perm = PERM_NONE, .attr = 4};
            break;
        }
        uint64_t lo = r.base, hi = r.base + r.size;
        uint64_t b = r.state == ST_FREE ? m->dst : r.state == ST_NORMAL ? m->src : 0;
        uint64_t e = b + m->size;
        if (!b) continue;
        if (e <= a && e > lo) lo = e;
        if (b > a && b < hi) hi = b;
        r.base = lo;
        r.size = hi - lo;
    }
    plat_mutex_unlock_shared(&lock);
    if (out) *out = r;
    return R_OK;
}

// ---- GetInfo memory ids
int mem_info(uint32_t id, uint64_t *out) {
    uint64_t v;
    switch (id) {
    case 2: v = L.alias.base; break;
    case 3: v = L.alias.size; break;
    case 4: v = L.heap.base; break;
    case 5: v = L.heap.size; break;
    case 6: case 21: v = L.pool; break;
    case 7: case 22:
        plat_mutex_lock_shared(&lock);
        v = image_bytes() + heap_size + alias_bytes;
        plat_mutex_unlock_shared(&lock);
        break;
    case 12: v = L.aslr.base; break;
    case 13: v = L.aslr.size; break;
    case 14: v = L.stack.base; break;
    case 15: v = L.stack.size; break;
    case 16: v = SYSTEM_RESOURCE_BUDGET; break;
    case 17: v = sizeof stack_maps + ALIAS_PAGES; break;   // system resource in use: our bookkeeping
    case 28: v = 0; break;   // AliasRegionExtraSize: no kernel-owned extension
    default: return 0;
    }
    *out = v;
    return 1;
}

// ---- kernel region: power-of-two blocks, a free list per size, bump allocation with lazy commit.
#define KCLASSES 29          // 64 B .. 256 MiB
#define KMIN_SHIFT 6
#define KCOMMIT 0x10000ull
typedef struct KFree { struct KFree *next; } KFree;
static KFree *kfree_list[KCLASSES];
static uint64_t kbump, kcommitted;
static PlatMutex klock = PLAT_MUTEX_INIT;

static int kclass(uint64_t size) {
    int s = KMIN_SHIFT;
    while ((1ull << s) < size) s++;
    return s - KMIN_SHIFT;
}

void *mem_kernel_alloc(uint64_t size, uint64_t align) {
    if (!L.pool) rt_fatal("mem_kernel_alloc before mem_init");
    if (align > KCOMMIT || (align & (align - 1))) rt_fatal("mem_kernel_alloc: alignment %#llx", (unsigned long long)align);
    uint64_t want = size > align ? size : align;
    int k = kclass(want ? want : 1);
    if (k >= KCLASSES) rt_fatal("mem_kernel_alloc: %#llx bytes", (unsigned long long)size);
    uint64_t bytes = 1ull << (k + KMIN_SHIFT), al = bytes < KCOMMIT ? bytes : KCOMMIT;
    plat_mutex_lock(&klock);
    KFree *f = kfree_list[k];
    void *p;
    if (f) {
        kfree_list[k] = f->next;
        p = f;
    } else {
        uint64_t a = align_up(kbump ? kbump : RT_KERNEL_BASE, al);
        if (a + bytes > RT_KERNEL_BASE + RT_KERNEL_SIZE) rt_fatal("kernel region exhausted (%#llx bytes)",
                                                                  (unsigned long long)bytes);
        uint64_t need = align_up(a + bytes, KCOMMIT);
        if (!kcommitted) kcommitted = RT_KERNEL_BASE;
        if (need > kcommitted) {
            commit(kcommitted, need - kcommitted, "kernel region");
            kcommitted = need;
        }
        kbump = a + bytes;
        p = (void *)(uintptr_t)a;
    }
    plat_mutex_unlock(&klock);
    memset(p, 0, size ? size : 1);
    return p;
}

void mem_kernel_free(void *p, uint64_t size) {
    if (!p) return;
    uint64_t a = (uintptr_t)p;
    if (a - RT_KERNEL_BASE >= RT_KERNEL_SIZE) rt_fatal("mem_kernel_free(%p): not in the kernel region", p);
    int k = kclass(size ? size : 1);
    // A block allocated with a larger alignment sits in a larger class; freeing it into the smaller class only
    // wastes the rest.
    plat_mutex_lock(&klock);
    KFree *f = p;
    f->next = kfree_list[k];
    kfree_list[k] = f;
    plat_mutex_unlock(&klock);
}
