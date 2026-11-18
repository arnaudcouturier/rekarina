// The game's zstd decoder on the host's libzstd. Most RomFS files of TotK 1.4.3 are zstd frames, many with a
// dictionary from Pack/ZsDic.pack.zs; the decoder is BSD zstd linked into main. The patch set "zstd"
// (recomp/patches-v143.txt) sends every call of ZSTD_decompressMultiFrame (main+0x3a1f0) and of ZSTD_decompress_usingDDict
// (main+0x3a1e0) to the two host functions below, on the patch page. Lifted, the decoder ran its literal and match
// copies as chains of small functions with the registers in Cpu; here the same frames decode in native code.
//
// - Results match the guest's: libzstd decodes the same format, and every result that is an error goes to the guest
//   decoder again (it writes its own error code; a frame libzstd rejects may still be one the game's version accepts).
// - A guest ZSTD_DDict (zstd's struct: dictBuffer, dictContent, dictSize first) becomes a host ZSTD_DDict made from a
//   copy of its content, cached by guest address, content address, size and a fingerprint of the content.
// - Inputs of at least AWAY_BYTES, or outputs of at least AWAY_OUT_BYTES, give the virtual core up while they decode
//   (other guest threads run on it; decode_is_long).
// REKARINA_ZSTD_NATIVE=0 keeps the guest decoder (the patch set switch); REKARINA_ZSTD_CHECK=1 decodes each call both ways,
// keeps the guest's result and logs every call where the two differ.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>

#include "cfg.h"
#include "host.h"

#if __has_include("patches.h")
#include "patches.h"
#include "platform/platform.h"
#endif

#ifdef RT_PATCH_PAGE

#ifdef GAME_OOT
#define GUEST_MULTIFRAME 0x70000eb380ull   // OoT 1.1.1 main: ZSTD_decompressMultiFrame (recomp/patches-oot-v111.txt)
#define GUEST_USING_DDICT 0x70000eb380ull  // no wrapper of its own in that build: no site uses the second entry
#else
#define GUEST_MULTIFRAME 0x700003a1f0ull   // 1.4.3 main: ZSTD_decompressMultiFrame (only 1.4.3 has the patch sites)
#define GUEST_USING_DDICT 0x700003a1e0ull  // ZSTD_decompress_usingDDict
#endif
#define HOST_MULTIFRAME (RT_PATCH_PAGE + 0xff0)
#define HOST_USING_DDICT (RT_PATCH_PAGE + 0xfe0)
#define AWAY_BYTES (64u << 10)
#define MAX_DICT (64u << 20)
#define NDICT 32

static void *gp(uint64_t a) { return (void *)(uintptr_t)a; }

typedef struct {
    uint64_t guest, content, size, print;
    ZSTD_DDict *dd;
} Dict;
static Dict dicts[NDICT];
static int ndicts;
static PlatMutex dict_lock = PLAT_MUTEX_INIT;

static struct {
    uint64_t calls, in, out, ns, guest_ns, fallbacks, away, inplace;
} S;

static int check_mode(void) {
    static int v = -1;
    if (v < 0) v = cfg_bool("REKARINA_ZSTD_CHECK");
    return v;
}

// Size, both ends and a sample of the middle: what tells a new dictionary at a reused address from the old one.
static uint64_t fingerprint(const uint8_t *p, uint64_t n) {
    uint64_t h = n * 0x9E3779B97F4A7C15ull;
    uint64_t at[3] = {0, n / 2, n > 64 ? n - 64 : 0};
    for (int k = 0; k < 3; k++)
        for (uint64_t i = at[k]; i < at[k] + 64 && i < n; i += 8) {
            uint64_t v = 0;
            memcpy(&v, p + i, n - i < 8 ? n - i : 8);
            h = (h ^ v) * 0xC2B2AE3D27D4EB4Full;
            h ^= h >> 29;
        }
    return h;
}

// The host dictionary for a guest ZSTD_DDict or raw dictionary bytes (guest == 0); NULL when its fields look wrong.
static ZSTD_DDict *dict_for(uint64_t guest, uint64_t content, uint64_t size) {
    if (!content || !size || size > MAX_DICT || content >= RT_ADDRESS_LIMIT || content + size > RT_ADDRESS_LIMIT)
        return NULL;
    uint64_t print = fingerprint(gp(content), size);
    ZSTD_DDict *dd = NULL;
    plat_mutex_lock_shared(&dict_lock);
    for (int i = 0; i < ndicts && !dd; i++)
        if (dicts[i].guest == guest && dicts[i].content == content && dicts[i].size == size && dicts[i].print == print)
            dd = dicts[i].dd;
    plat_mutex_unlock_shared(&dict_lock);
    if (dd) return dd;
    // A copy: the guest may free or reuse its buffer. Entries are never freed (another thread may be decoding with
    // one); a full table sends further dictionaries to the guest decoder.
    ZSTD_DDict *made = ZSTD_createDDict(gp(content), (size_t)size);
    if (!made) return NULL;
    int slot = -1;
    plat_mutex_lock(&dict_lock);
    for (int i = 0; i < ndicts && !dd; i++)   // another thread may have made it meanwhile
        if (dicts[i].guest == guest && dicts[i].content == content && dicts[i].size == size && dicts[i].print == print)
            dd = dicts[i].dd;
    if (!dd && ndicts < NDICT) slot = ndicts, dicts[ndicts++] = (Dict){guest, content, size, print, made}, dd = made;
    plat_mutex_unlock(&dict_lock);
    if (dd != made) {
        ZSTD_freeDDict(made);
        if (!dd) rt_log("zstd: more than %d dictionaries: further ones decode in the guest", NDICT);
        return dd;
    }
    rt_log("zstd: host dictionary %d for %s %#llx: %llu bytes at %#llx, id %u", slot, guest ? "DDict" : "raw dict",
           (unsigned long long)(guest ? guest : content), (unsigned long long)size, (unsigned long long)content,
           ZSTD_getDictID_fromDDict(dd));
    return dd;
}

// Decodes on this thread's host context. Returns libzstd's result; *ok 0 when the inputs cannot be taken.
static size_t host_decode(uint64_t dst, uint64_t cap, uint64_t src, uint64_t size, uint64_t dict, uint64_t dict_size,
                          uint64_t ddict, int *ok) {
    static __thread ZSTD_DCtx *dctx;
    *ok = 0;
    if ((cap && !dst) || !src || src + size > RT_ADDRESS_LIMIT) return 0;   // dst: decode() checks a guest one
    ZSTD_DDict *dd = NULL;
    if (ddict) {
        if (ddict >= RT_ADDRESS_LIMIT - 24) return 0;
        uint64_t f[3];   // ZSTD_DDict: dictBuffer, dictContent, dictSize
        memcpy(f, gp(ddict), sizeof f);
        if (!(dd = dict_for(ddict, f[1], f[2]))) {
            static int logged;
            if (logged < 4) {
                uint64_t w[8];
                memcpy(w, gp(ddict), sizeof w);
                logged++;
                rt_log("zstd: DDict %#llx not taken: %llx %llx %llx %llx %llx %llx %llx %llx", (unsigned long long)ddict,
                       (unsigned long long)w[0], (unsigned long long)w[1], (unsigned long long)w[2], (unsigned long long)w[3],
                       (unsigned long long)w[4], (unsigned long long)w[5], (unsigned long long)w[6], (unsigned long long)w[7]);
            }
            return 0;
        }
    } else if (dict && dict_size) {
        if (!(dd = dict_for(0, dict, dict_size))) return 0;
    }
    if (!dctx && !(dctx = ZSTD_createDCtx())) return 0;
    *ok = 1;
    return dd ? ZSTD_decompress_usingDDict(dctx, gp(dst), (size_t)cap, gp(src), (size_t)size, dd)
              : ZSTD_decompressDCtx(dctx, gp(dst), (size_t)cap, gp(src), (size_t)size);
}

// Whether a decode gives the virtual core up: one that takes more than a few tens of microseconds. Its time follows
// its output (libzstd decodes 1-2 GB/s on one core), and an input under AWAY_BYTES can still decode megabytes: the
// game's priority-47 loader threads held their cores through 2.7 to 4.7 ms decodes of such inputs while priority-32 and
// -44 threads waited for those cores (REKARINA_CORE_DIAG, 2026-11-17). So the first frame's content size (the output
// capacity when the header does not say) of AWAY_OUT_BYTES or more gives it up too, about 40 us of decoding, against a
// handover of a few microseconds. REKARINA_WAIT_OFF_CORE=0: the input size alone, as before.
#define AWAY_OUT_BYTES (64u << 10)
static int decode_is_long(uint64_t src, uint64_t size, uint64_t cap) {
    if (size >= AWAY_BYTES) return 1;
    if (!kern_wait_off_core() || !src || src + size > RT_ADDRESS_LIMIT) return 0;
    unsigned long long out = ZSTD_getFrameContentSize(gp(src), (size_t)size);
    if (out == ZSTD_CONTENTSIZE_UNKNOWN || out == ZSTD_CONTENTSIZE_ERROR) out = cap;
    return out >= AWAY_OUT_BYTES;
}

// The host call inside the thread's host depth, as hostfn.c counts it for asynchronous preemption.
static size_t tracked_decode(Cpu *c, uint64_t dst, uint64_t cap, uint64_t src, uint64_t size, uint64_t dict,
                             uint64_t dict_size, uint64_t ddict, int *ok) {
    int *d = kern_host_track && c->thread ? kern_host_depth(c->thread) : NULL;
    if (d) __atomic_store_n(d, *d + 1, __ATOMIC_RELAXED);
    int away = decode_is_long(src, size, cap) ? kern_core_away() : 0;
    if (away) __atomic_add_fetch(&S.away, 1, __ATOMIC_RELAXED);
    size_t r = host_decode(dst, cap, src, size, dict, dict_size, ddict, ok);
    kern_core_back(&away);
    if (d) {
        int n = *d - 1;
        __atomic_store_n(d, n, __ATOMIC_RELAXED);
        if (!n && kern_preempt_pending(c->thread)) kern_host_left(c->thread);
    }
    return r;
}

static void count(uint64_t in, uint64_t out, uint64_t ns) {
    uint64_t n = __atomic_add_fetch(&S.calls, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&S.in, in, __ATOMIC_RELAXED);
    __atomic_add_fetch(&S.out, out, __ATOMIC_RELAXED);
    __atomic_add_fetch(&S.ns, ns, __ATOMIC_RELAXED);
    if (n == 1 || n == 256 || !(n & 0xfff))
        rt_log("zstd: %llu calls on the host, %.1f MiB in, %.1f MiB out, %.0f ms (check mode: the guest decoder %.0f ms); "
               "%llu to the guest decoder, %llu in place in the guest, %llu with the core given up", (unsigned long long)n,
               S.in / 1048576.0, S.out / 1048576.0, S.ns / 1e6, S.guest_ns / 1e6, (unsigned long long)S.fallbacks,
               (unsigned long long)S.inplace, (unsigned long long)S.away);
}

// x0 dctx, x1 dst, x2 capacity, x3 src, x4 size, then (dict, dict size, ddict) as the entry has them. guest: the
// guest function with the same arguments, n of them.
static void decode(Cpu *c, uint64_t dict, uint64_t dict_size, uint64_t ddict, uint64_t guest, int n) {
    uint64_t a[8];
    memcpy(a, c->x, sizeof a);
    uint64_t dst = a[1], cap = a[2], src = a[3], size = a[4];
    // In place (the input inside the output buffer): the result depends on how the decoder's writes overtake its
    // reads, which libzstd does not promise to do as the game's version does, and a host decode that failed half way
    // would leave the guest decoder a damaged input. Such calls stay in the guest.
    if (cap && src < dst + cap && dst < src + size) {
        __atomic_add_fetch(&S.inplace, 1, __ATOMIC_RELAXED);
        rt_call(c, guest, n, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
        return;
    }
    if (check_mode()) {
        uint8_t *mine = cap ? malloc((size_t)cap) : NULL;
        int ok = 0;
        uint64_t t0 = kern_now_ns();
        size_t r = cap && !mine ? 0 : tracked_decode(c, (uint64_t)(uintptr_t)mine, cap, src, size, dict, dict_size, ddict, &ok);
        uint64_t t1 = kern_now_ns();
        uint64_t g = rt_call(c, guest, n, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
        __atomic_add_fetch(&S.guest_ns, kern_now_ns() - t1, __ATOMIC_RELAXED);
        __atomic_add_fetch(&S.ns, t1 - t0, __ATOMIC_RELAXED);
        static int logged;
        if (logged < 6 && (!ok || ZSTD_isError(r))) {
            logged++;
            rt_log("zstd check: host %s, guest %llu; dctx %#llx dst %#llx cap %llu src %#llx size %llu dict %#llx/%llu "
                   "ddict %#llx", !ok ? "inputs not taken" : ZSTD_getErrorName(r), (unsigned long long)g,
                   (unsigned long long)a[0], (unsigned long long)dst, (unsigned long long)cap, (unsigned long long)src,
                   (unsigned long long)size, (unsigned long long)dict, (unsigned long long)dict_size,
                   (unsigned long long)ddict);
        }
        if (ok && !ZSTD_isError(r) && (g != r || (r && memcmp(mine, gp(dst), r)))) {
            const uint8_t *p = gp(src);
            size_t fs = ZSTD_findFrameCompressedSize(p, (size_t)size);
            unsigned long long cs = ZSTD_getFrameContentSize(p, (size_t)size);
            ZSTD_DCtx *fresh = ZSTD_createDCtx();
            size_t r2 = ZSTD_decompressDCtx(fresh, mine, (size_t)cap, p, (size_t)size);
            ZSTD_freeDCtx(fresh);
            static int nlog;
            if (nlog++ < 8)
                rt_log("zstd check: MISMATCH %llu bytes from %#llx (cap %llu, first frame %lld bytes, content %lld, magic "
                       "%02x%02x%02x%02x, header %02x, dict %#llx/%llu, ddict %#llx, entry %s, lr %#llx): the host decoded "
                       "%llu bytes, the guest %lld%s; a fresh context without a dictionary: %s", (unsigned long long)size,
                       (unsigned long long)src, (unsigned long long)cap, ZSTD_isError(fs) ? -1ll : (long long)fs,
                       (long long)cs, p[3], p[2], p[1], p[0], p[4], (unsigned long long)dict,
                       (unsigned long long)dict_size, (unsigned long long)ddict, n == 8 ? "MultiFrame" : "usingDDict",
                       (unsigned long long)c->x[30], (unsigned long long)r, (long long)g, g == r ? " (other contents)" : "",
                       ZSTD_isError(r2) ? ZSTD_getErrorName(r2) : "decodes");
        }
        free(mine);
        count(size, ok && !ZSTD_isError(r) ? r : 0, 0);
        return;
    }
    uint64_t t0 = kern_now_ns();
    int ok = 0;
    size_t r = dst + cap > RT_ADDRESS_LIMIT ? 0 : tracked_decode(c, dst, cap, src, size, dict, dict_size, ddict, &ok);
    if (!ok || ZSTD_isError(r)) {   // the guest decoder decides (and writes its own error code)
        __atomic_add_fetch(&S.fallbacks, 1, __ATOMIC_RELAXED);
        static int logged;
        if (logged < 8) {
            logged++;
            rt_log("zstd: %llu bytes at %#llx go to the guest decoder: %s", (unsigned long long)size,
                   (unsigned long long)src, !ok ? "inputs not taken" : ZSTD_getErrorName(r));
        }
        rt_call(c, guest, n, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
        return;
    }
    c->x[0] = r;
    count(size, r, kern_now_ns() - t0);
}

static void host_multiframe(Cpu *c) { decode(c, c->x[5], c->x[6], c->x[7], GUEST_MULTIFRAME, 8); }
static void host_using_ddict(Cpu *c) { decode(c, 0, 0, c->x[5], GUEST_USING_DDICT, 6); }

#ifdef GAME_OOT
// ---- The game's parallel block decompression on the host (REKARINA_ZSTD_JOBS, OoT)
// While a scene loads, the threads "ParallelDecompThread" (priority 47, pinned to virtual cores 4 and 5) decode the
// archive's zstd frames in jobs, and the decoder they run is the game's own, lifted: 348 of 428 samples of those
// threads in the user's logs were in its sequence loop (main+0xf0370), against libzstd's speed of a few times that.
// A job (main+0xf5bf0, reached only by an indirect call) feeds the next piece of the compressed file to a streaming
// decoder of the game's (main+0xf5c80): the pieces come from two read buffers in turn, and each call decodes what the
// piece completes into the output buffer. Those calls also held their virtual core for milliseconds at a time, and
// the threads pinned behind them (the ones that draw the loading picture among them) waited: the loading picture's
// frames of 60-116 ms.
// Here the job runs as the game's does, and the decoding step goes to a host stream with the same results: the
// wrapper's fields the game reads (state byte, output cursor, the two counters, the frame's content size) move as the
// game's step moves them, and the decode itself runs with the virtual core given up. A wrapper is the host's from
// its first piece (state 1) to its end; one the host cannot take (a dictionary it cannot read, the table full) stays
// the game's for its whole frame. REKARINA_ZSTD_CHECK=1 runs the game's step on the real wrapper and the host stream on
// a copy of its output beside it, and logs every piece where the two differ: the save's load decoded 416 pieces of
// 64+ frames (982 MiB) the same way. Measured on the save's load (2026-11-18, one exe, ABBA): from the first frame to
// the arrival 12.35 / 11.63 s against 13.12 / 13.36 s with three busy cores and a disk loop beside the game, 11.50
// against 12.09 s on a quiet machine; the game's own decoder took about 2 s for what the host decodes in 1.2 s.
// The wrapper, at the job + 0x10: +0 state (1 new, 2 decoding, 3 failed, 4 done), +0x8 output cursor, +0x10 output
// room (int32), +0x14 content bytes still to come (uint32, the return value), +0x18 the game's context memory, +0x20 /
// +0x28 / +0x2c its staging buffer, capacity and fill, +0x30 its context.
void f_70000f5bf0(Cpu *c);   // lifted: the job
void f_70000f5c80(Cpu *c);   // lifted: the streaming step (wrapper, piece, piece size, ZSTD_DDict or 0)
void f_7003c42cd0(Cpu *c);   // lifted: what the job calls with job + 0x68 and job + 0x98 (its signals)
#define GUEST_JOB 0x70000f5bf0ull
#define JW_N 64
typedef struct {
    uint64_t w;                 // the wrapper's guest address, 0 free
    int host;                   // 1: the host decodes this frame; 0: the game's step does
    ZSTD_DCtx *dctx;
    uint8_t *shadow;            // check mode: the host's output
    uint64_t shadow_cap, shadow_pos, guest_start;
} JobWrap;
static JobWrap jw[JW_N];
static PlatMutex jw_lock = PLAT_MUTEX_INIT;
static struct { uint64_t pieces, frames, in, out, ns, guest_frames, errors, diffs, away; } JS;

static int jobs_on(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = cfg_get("REKARINA_ZSTD_JOBS");   // on by default (0: the game's decoder)
        v = e && *e ? atoi(e) != 0 : 1;
        rt_log("zstd: the game's block decompression jobs decode %s (REKARINA_ZSTD_JOBS)", v ? "on the host" : "in the game's decoder");
    }
    return v;
}

static uint64_t ld64(uint64_t a) { uint64_t v; memcpy(&v, gp(a), 8); return v; }
static uint32_t ld32(uint64_t a) { uint32_t v; memcpy(&v, gp(a), 4); return v; }
static void st64(uint64_t a, uint64_t v) { memcpy(gp(a), &v, 8); }
static void st32(uint64_t a, uint32_t v) { memcpy(gp(a), &v, 4); }

static JobWrap *jw_find(uint64_t w, int make) {   // jw_lock held
    JobWrap *free_one = NULL;
    for (int i = 0; i < JW_N; i++) {
        if (jw[i].w == w) return &jw[i];
        if (!jw[i].w && !free_one) free_one = &jw[i];
    }
    if (!make || !free_one) return NULL;
    free_one->w = w, free_one->host = 0;
    return free_one;
}

static void jw_release(JobWrap *e) {   // the frame ended: the entry (and its context) serves the next one
    plat_mutex_lock(&jw_lock);
    e->w = 0;
    plat_mutex_unlock(&jw_lock);
}

static void log_jobs(void) {
    uint64_t n = __atomic_add_fetch(&JS.frames, 1, __ATOMIC_RELAXED);
    if (n == 1 || n == 64 || !(n & 1023))
        rt_log("zstd jobs: %llu frames on the host (%llu pieces, %.1f MiB in, %.1f MiB out, %.0f ms, %llu with the core "
               "given up), %llu frames left to the game's decoder, %llu failed%s", (unsigned long long)n,
               (unsigned long long)JS.pieces, JS.in / 1048576.0, JS.out / 1048576.0, JS.ns / 1e6, (unsigned long long)JS.away,
               (unsigned long long)JS.guest_frames, (unsigned long long)JS.errors, check_mode() ? " (check mode)" : "");
}

// One piece through the host stream of e: out is where it writes (the guest's cursor, or the shadow in check mode).
// Returns libzstd's last result; *made the bytes written, *left the piece's bytes not taken.
static size_t stream_piece(Cpu *c, JobWrap *e, uint64_t src, uint32_t size, uint8_t *out, uint64_t room, uint64_t *made,
                           uint64_t *left) {
    ZSTD_inBuffer in = {gp(src), size, 0};
    ZSTD_outBuffer o = {out, (size_t)room, 0};
    int *d = kern_host_track && c->thread ? kern_host_depth(c->thread) : NULL;
    if (d) __atomic_store_n(d, *d + 1, __ATOMIC_RELAXED);
    int away = size >= (16u << 10) ? kern_core_away() : 0;
    if (away) __atomic_add_fetch(&JS.away, 1, __ATOMIC_RELAXED);
    size_t r = 0;
    for (;;) {   // until the piece is taken, the frame ends, or a call makes no progress (no room left)
        size_t ip = in.pos, op = o.pos;
        r = ZSTD_decompressStream(e->dctx, &o, &in);
        if (ZSTD_isError(r) || !r || in.pos == in.size || (in.pos == ip && o.pos == op)) break;
    }
    kern_core_back(&away);
    if (d) {
        int n = *d - 1;
        __atomic_store_n(d, n, __ATOMIC_RELAXED);
        if (!n && kern_preempt_pending(c->thread)) kern_host_left(c->thread);
    }
    *made = o.pos, *left = in.size - in.pos;
    return r;
}

// The game's streaming step on the wrapper w for one piece, done by the host when the wrapper is the host's.
// Returns x0 as the game's step would.
static uint64_t job_step(Cpu *c, uint64_t w, uint64_t src, uint32_t size, uint64_t ddict) {
    uint8_t state = *(uint8_t *)gp(w);
    int check = check_mode();
    plat_mutex_lock(&jw_lock);
    JobWrap *e = jw_find(w, state == 1);
    if (e && state == 1) {   // a new frame: the host's when it can read the dictionary (if any)
        ZSTD_DDict *dd = NULL;
        e->host = 0;
        if (ddict && ddict < RT_ADDRESS_LIMIT - 24) {
            uint64_t f[3];
            memcpy(f, gp(ddict), sizeof f);
            dd = dict_for(ddict, f[1], f[2]);
        }
        if ((!ddict || dd) && (e->dctx || (e->dctx = ZSTD_createDCtx()))) {
            ZSTD_DCtx_reset(e->dctx, ZSTD_reset_session_and_parameters);
            if (!dd || !ZSTD_isError(ZSTD_DCtx_refDDict(e->dctx, dd))) e->host = 1;
        }
        if (!e->host) {   // the game's frame: no entry stands for that
            __atomic_add_fetch(&JS.guest_frames, 1, __ATOMIC_RELAXED);
            e->w = 0;
        }
    } else if (!e && state == 1) {
        static int logged;
        if (!logged++) rt_log("zstd jobs: more than %d frames decoding at once: further ones in the game's decoder", JW_N);
    }
    int host = e && e->host;
    plat_mutex_unlock(&jw_lock);
    if (!host || (!check && state != 1 && state != 2)) {   // the game's (and its own answers for states 3 and 4)
        c->x[0] = w, c->x[1] = src, c->x[2] = size, c->x[3] = ddict;
        f_70000f5c80(c);
        return c->x[0];
    }
    __atomic_add_fetch(&JS.pieces, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&JS.in, size, __ATOMIC_RELAXED);
    uint64_t t0 = kern_now_ns();
    if (check) {   // the game's step on the real wrapper, the host stream on the shadow
        uint64_t cur0 = ld64(w + 8);
        if (state == 1) {
            ZSTD_frameHeader zfh;
            free(e->shadow);
            e->shadow = NULL, e->shadow_cap = e->shadow_pos = 0, e->guest_start = cur0;
            if (!ZSTD_getFrameHeader(&zfh, gp(src), size) && zfh.frameContentSize != ZSTD_CONTENTSIZE_UNKNOWN &&
                zfh.frameContentSize < (1ull << 31))
                e->shadow_cap = zfh.frameType == ZSTD_skippableFrame ? 0 : zfh.frameContentSize,
                e->shadow = malloc(e->shadow_cap ? (size_t)e->shadow_cap : 1);
        }
        c->x[0] = w, c->x[1] = src, c->x[2] = size, c->x[3] = ddict;
        f_70000f5c80(c);
        uint64_t g = c->x[0], cur1 = ld64(w + 8);
        uint64_t made = 0, left = 0;
        size_t r = 0;
        if (e->shadow && (state == 1 || state == 2))
            r = stream_piece(c, e, src, size, e->shadow + e->shadow_pos, e->shadow_cap - e->shadow_pos, &made, &left);
        int bad = !e->shadow ? 1 : ZSTD_isError(r) ? 2 : cur1 - cur0 != made ? 3 :
                  memcmp(e->shadow + e->shadow_pos, gp(cur0), (size_t)made) ? 4 : (r == 0) != (*(uint8_t *)gp(w) == 4) ? 5 : 0;
        if ((state == 1 || state == 2) && bad && __atomic_add_fetch(&JS.diffs, 1, __ATOMIC_RELAXED) <= 12)
            rt_log("zstd jobs check: DIFFERENCE %d at a piece of %u bytes (wrapper %#llx, state %u -> %u, game wrote %llu, "
                   "host %llu, host left %llu, host result %s, game result %#llx)", bad, size, (unsigned long long)w, state,
                   *(uint8_t *)gp(w), (unsigned long long)(cur1 - cur0), (unsigned long long)made, (unsigned long long)left,
                   ZSTD_isError(r) ? ZSTD_getErrorName(r) : r ? "frame goes on" : "frame done", (unsigned long long)g);
        e->shadow_pos += made;
        __atomic_add_fetch(&JS.out, made, __ATOMIC_RELAXED);
        __atomic_add_fetch(&JS.ns, kern_now_ns() - t0, __ATOMIC_RELAXED);
        if (*(uint8_t *)gp(w) == 4 || *(uint8_t *)gp(w) == 3) {
            free(e->shadow);
            e->shadow = NULL;
            jw_release(e);
            log_jobs();
        }
        return g;
    }
    if (state == 1) {   // the game reads the frame's content size first: unknown or unreadable fails the frame
        ZSTD_frameHeader zfh;
        if (ZSTD_getFrameHeader(&zfh, gp(src), size) ||
            (zfh.frameType != ZSTD_skippableFrame && zfh.frameContentSize == ZSTD_CONTENTSIZE_UNKNOWN)) {
            *(uint8_t *)gp(w) = 3;
            __atomic_add_fetch(&JS.errors, 1, __ATOMIC_RELAXED);
            return 0xffffffffull;
        }
        st32(w + 0x14, zfh.frameType == ZSTD_skippableFrame ? 0 : (uint32_t)zfh.frameContentSize);
        *(uint8_t *)gp(w) = 2;
    }
    uint64_t dst = ld64(w + 8);
    int32_t room = (int32_t)ld32(w + 0x10);
    uint64_t made = 0, left = 0;
    size_t r = dst && room >= 0 && dst + (uint64_t)room <= RT_ADDRESS_LIMIT
                   ? stream_piece(c, e, src, size, gp(dst), (uint64_t)room, &made, &left)
                   : (size_t)-1;
    st64(w + 8, dst + made);
    st32(w + 0x10, (uint32_t)(room - (int32_t)made));
    st32(w + 0x14, ld32(w + 0x14) - (uint32_t)made);
    __atomic_add_fetch(&JS.out, made, __ATOMIC_RELAXED);
    __atomic_add_fetch(&JS.ns, kern_now_ns() - t0, __ATOMIC_RELAXED);
    if (ZSTD_isError(r) || (r && left) || (!r && left)) {   // a damaged frame, no room, or bytes past the frame's end
        static int logged;
        if (logged++ < 8)
            rt_log("zstd jobs: a frame failed on the host (%s; piece of %u bytes, %llu not taken, room %d)",
                   ZSTD_isError(r) ? ZSTD_getErrorName(r) : r ? "no room for the output" : "bytes past the frame's end", size,
                   (unsigned long long)left, room);
        *(uint8_t *)gp(w) = 3;
        __atomic_add_fetch(&JS.errors, 1, __ATOMIC_RELAXED);
        jw_release(e);
        log_jobs();
        return r && !ZSTD_isError(r) ? 0xffffffecull : !r ? 0xfffffffdull : 0xffffffffull;
    }
    if (!r) {
        *(uint8_t *)gp(w) = 4;
        jw_release(e);
        log_jobs();
    }
    return ld32(w + 0x14);
}

// The job, as main+0xf5bf0 runs it, with the decoding step above.
static void host_job(Cpu *c) {
    if (!jobs_on()) {
        f_70000f5bf0(c);
        return;
    }
    uint64_t job = c->x[0];
    if ((int32_t)ld32(job + 0xbc) < 0) return;   // a failed job: nothing more
    uint64_t base = ld64(job + 0xa8);
    uint32_t off = ld32(job + 0xb0), turn = ld32(job + 0xb4), size = ld32(job + 0xb8);
    uint64_t r = job_step(c, job + 0x10, (turn & 1) ? base + off : base, size, ld64(job + 0xc0));
    st32(job + 0xb4, ld32(job + 0xb4) + 1);
    uint32_t r32 = (uint32_t)r;
    if (!r32 || (r32 >> 31)) {
        if (r32) st32(job + 0xbc, r32);
        c->x[0] = job + 0x68;
        f_7003c42cd0(c);
    }
    c->x[0] = job + 0x98;
    f_7003c42cd0(c);
}
#endif

__attribute__((constructor)) static void zstd_register(void) {
    rt_register_host(HOST_MULTIFRAME, host_multiframe);
    rt_register_host(HOST_USING_DDICT, host_using_ddict);
#ifdef GAME_OOT
    rt_dispatch_replace(GUEST_JOB, host_job);
#endif
}

#endif
