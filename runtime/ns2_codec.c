// The Switch 2 SDK's nn::codec video decoders, decoder image pools, native sync handles and sync point events, served
// on the host (GAME_NS2, Rekarina). The SDK sends every one of these calls to a decoder service and NVDEC, which the
// host does not have. AV1 decodes with dav1d (BSD-2-Clause, static), VP9 and VP8 with libvpx (BSD-3-Clause, static, as
// ov_movie.c); H.264 Initialize fails. The AAC decoder of the same library is not served: OoT's movies
// (Event/Movie/*.webm: AV1, 1920x1080, 60 fps) have no sound track, their sound is a separate .bwav.
//
// Behaviour ported from seedds/nxnative dm/host/hle_codec_video.c (MIT, @ 865cc35; its notes on the SDK 20.5 and
// SDK 22 protocol) and MacNativeGames games/totk/runtime/ns2_codec.c (MIT, @ 52185a9), written again for this host's
// calling convention. OoT's movie library (subsdk0, NintendoSDK_movie 22.2.8; offsets of game 1.1.1) uses
// the API this way, which decides the answers below:
//  - movie::VideoDecoder::RegisterVideoFrame (subsdk0+0x3a40) makes one image pool per frame (MakeDecodeImagePreference,
//    InitializeDecoderImagePool with one picture), registers it under an index, acquires an output buffer at once and
//    keeps its sync handle as the frame's fence. Release (+0x46b0) gives a buffer back and acquires again the same way.
//    So a buffer is the decoder's to fill from Register or Release on, and the handle the next Acquire returns signals
//    when a picture is in it. (nxnative signals a registered buffer's handle at once. This library then takes the
//    buffer as filled: before Start it gives it straight back, which works; once started it reads the properties of
//    an empty buffer, gets 3604 and keeps that error at VideoDecoder+0xf80. Which one happens depends on whether its
//    event thread runs before the game calls Start.)
//  - Its event thread (VideoDecoderEventManager) waits on a sync point event requested for the oldest such fence. When
//    the event fires, nn::gs::TryWaitNativeSync on that handle must be true (+0x45d0 aborts otherwise); then it reads
//    the buffer's properties and hands the frame to the game, or, while the decoder is not started, gives the buffer
//    straight back. Flush gives back the buffers whose fence is reached and every buffer the game holds.
//  - The game reads every frame on the CPU (VideoFrame::SetPixelStorageType 1, main+0x3b5f11c): ReadDecoderOutputImageNv12
//    of the whole picture into its own buffer, tight (strides = width, UV right after Y; subsdk0+0xf86c). Native image
//    handles (the GPU route) are asked for only with storage type 3, which the game does not use.
//  - DecodeAsync (+0x3850) clears the submission-ready event, then submits one packet: the decoder signals the event
//    again whenever it can take another. End of stream is a packet with eos set (size 0 at the end of the file).
//  - Every Initialize error makes the library abort, except result 2003 from the ByRemoteMemory forms (it returns its
//    own error 0x11 then).
// On the host: Submit copies the packet into a queue and returns; one worker thread per decoder (dav1d adds its own
// frame threads) decodes in order into tight 8-bit NV12 pictures, which go into the buffers in the order they were
// handed to the decoder. Pictures waiting for a buffer stop the worker at HOLD_MAX; the submission-ready event is up
// while fewer than IN_READY packets wait, and packets are refused (3603) only at IN_MAX. End of stream drains the
// codec and comes as a buffer of its own after the last picture, with the eos flag and the last picture in it.
// The SDK's opaque types hold a host pointer at +0 (decoder type 0x400 bytes, pool type 0x20 bytes); the decoder's
// submission-ready SystemEventType lives in its type at +0x100 and is destroyed with the decoder, as in the SDK (kept,
// it stayed bound and a decoder made again in the same cleared memory got none: nxnative, Donkey Kong Bananza).
// The sync handles (nn::gs::NativeSyncHandle, 16 bytes) are the host's: magic, decoder serial, buffer, generation.
// TOTK_AV1_THREADS sets dav1d's thread count (default by the machine, 2 to 6).
#ifdef GAME_NS2
#include <emmintrin.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dav1d/dav1d.h>
#include <vpx/vp8dx.h>
#include <vpx/vpx_decoder.h>

#include "cfg.h"
#include "host.h"
#include "platform/platform.h"
#include "nvn_api.h"

void rt_write_png(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h);   // boot.c (declared in vk.h)

#define CV_R(desc) RT_RESULT(111, desc)   // nn::codec results (module 111)
#define CV_BAD CV_R(2004)                 // not a decoder or buffer the host knows, or not served
#define CV_NO_MEMORY CV_R(2003)           // the one Initialize error the movie library survives (ByRemoteMemory)
#define CV_BUSY CV_R(3603)                // the decoder cannot take the packet now
#define CV_EMPTY CV_R(3604)               // no buffer to acquire / no picture in the buffer yet
#define DEC_MAGIC 0x43644e43u             // host objects and handles
#define POOL_MAGIC 0x50644e43u
#define PREF_MAGIC 0x52644e43u
#define SYNC_MAGIC 0x53644e43u
#define SPT_MAGIC 0x45644e43u
#define CV_EVENT_OFF 0x100                // the submission-ready SystemEventType inside the decoder type
#define SPT_OFF 0x40                      // host state inside a SyncPointEventType (0x80 bytes; SystemEventType at +0)

enum { CV_H264 = 0, CV_VP9 = 1, CV_AV1 = 3, CV_VP8 = 4 };   // the movie library's codec numbers (VideoDecoderTypes+0x400)
enum { FMT_RGBA = 2, FMT_NV12 = 3, FMT_RGBA_HDR = 4 };       // VideoDecoderOutputFormat (identical in the movie library)
enum { CV_BUFS = 32, CV_DECS = 16, CV_POOLS = 256, IN_MAX = 16, IN_READY = 4, HOLD_MAX = 4, HOLD_CAP = 32, SPT_MAX = 64,
       PIC_CACHE = 8 };

static const char *codec_name(int codec) {
    return codec == CV_AV1 ? "AV1" : codec == CV_VP9 ? "VP9" : codec == CV_VP8 ? "VP8" : codec == CV_H264 ? "H.264" : "?";
}

// ---- guest side: SystemEvents of the SDK's nn::os. The probe replaces these with stand-ins (probe_codec).
typedef struct {
    void (*bind)(Cpu *c, uint64_t ev);                  // makes ev a manual-clear SystemEvent the host can signal
    void (*signal)(uint64_t ev);                        // any thread
    void (*clear)(Cpu *c, uint64_t ev);                 // guest thread
    void (*destroy)(Cpu *c, uint64_t ev);               // guest thread
    void (*holder)(Cpu *c, uint64_t holder, uint64_t ev);   // nn::os::InitializeMultiWaitHolder(holder, SystemEventType *)
} CvOps;

static uint64_t need(const char *name) {
    uint64_t a = ldr_symbol(name);
    if (!a) rt_fatal("codec: guest symbol %s is not loaded", name);
    return a;
}
static void g_bind(Cpu *c, uint64_t ev) { kern_bind_system_event(c, ev, 0); }
static void g_signal(uint64_t ev) { kern_signal_system_event(ev); }
static void g_clear(Cpu *c, uint64_t ev) {
    static uint64_t f;
    if (!f) f = need("_ZN2nn2os16ClearSystemEventEPNS0_15SystemEventTypeE");
    rt_call(c, f, 1, ev);
}
static void g_destroy(Cpu *c, uint64_t ev) {
    static uint64_t f;
    if (!f) f = need("_ZN2nn2os18DestroySystemEventEPNS0_15SystemEventTypeE");
    rt_call(c, f, 1, ev);
}
static void g_holder(Cpu *c, uint64_t holder, uint64_t ev) {
    static uint64_t f;
    if (!f) f = need("_ZN2nn2os25InitializeMultiWaitHolderEPNS0_19MultiWaitHolderTypeEPNS0_15SystemEventTypeE");
    rt_call(c, f, 2, holder, ev);
}
static const CvOps guest_ops = {g_bind, g_signal, g_clear, g_destroy, g_holder};
static const CvOps *ops = &guest_ops;

// ---- decoded pictures: tight 8-bit NV12 (luma rows of w bytes, then (h+1)/2 rows of 2*((w+1)/2) bytes), shared by
// reference between the decoder, the pools and a readback in progress.
typedef struct Pic {
    volatile int32_t refs;
    int w, h, uvs;
    int matrix, full;                     // matrix: 601, 709 or 2020; full: full-range samples
    int64_t ts;                           // TimeSpan (ns) of the packet that carried it
    size_t cap;
    uint8_t *y, *uv;
} Pic;

static Pic *g_pic_cache[PIC_CACHE];       // freed pictures kept for the next ones of the same size
static int g_npic_cache;
static PlatMutex g_pic_mu = PLAT_MUTEX_INIT;   // the cache, the pool table and every pool's pictures

static Pic *pic_new(int w, int h) {
    int uvs = 2 * ((w + 1) / 2);
    size_t bytes = (size_t)w * h + (size_t)uvs * ((h + 1) / 2);
    Pic *p = NULL;
    plat_mutex_lock(&g_pic_mu);
    for (int i = 0; i < g_npic_cache && !p; i++)
        if (g_pic_cache[i]->cap == bytes) {
            p = g_pic_cache[i];
            g_pic_cache[i] = g_pic_cache[--g_npic_cache];
        }
    plat_mutex_unlock(&g_pic_mu);
    if (!p) {
        if (!(p = malloc(sizeof *p + bytes + 64))) rt_fatal("codec: picture allocation (%dx%d)", w, h);
        p->cap = bytes;
    }
    p->refs = 1;
    p->w = w, p->h = h, p->uvs = uvs;
    p->matrix = 709, p->full = 0, p->ts = 0;
    p->y = (uint8_t *)(((uintptr_t)(p + 1) + 63) & ~(uintptr_t)63);
    p->uv = p->y + (size_t)w * h;
    return p;
}
static Pic *pic_ref(Pic *p) {
    if (p) plat_inc32(&p->refs);
    return p;
}
static void pic_unref(Pic *p) {
    if (!p || plat_dec32(&p->refs) > 0) return;
    plat_mutex_lock(&g_pic_mu);
    if (g_npic_cache < PIC_CACHE) {
        g_pic_cache[g_npic_cache++] = p;
        p = NULL;
    }
    plat_mutex_unlock(&g_pic_mu);
    free(p);
}

// The matrix a stream declares; an undeclared one is BT.601 below 720 lines and BT.709 from there (as nxnative).
static int pick_matrix(int declared, int h) { return declared ? declared : h >= 720 ? 709 : 601; }

// Planar 4:2:0, 4:2:2, 4:4:4 or 4:0:0 samples of 8 to 12 bits into the picture's NV12, 8 bits (higher depths shifted).
// ssx, ssy: chroma subsampled horizontally, vertically.
static void planar_to_nv12(Pic *o, const void *py, ptrdiff_t ys, const void *pu, const void *pv, ptrdiff_t cs, int bpc,
                           int ssx, int ssy, int mono) {
    int w = o->w, h = o->h, sh = bpc - 8, cw = (w + 1) / 2, ch = (h + 1) / 2;
    for (int y = 0; y < h; y++) {
        uint8_t *d = o->y + (size_t)y * w;
        if (bpc == 8) memcpy(d, (const uint8_t *)py + y * ys, (size_t)w);
        else {
            const uint16_t *s = (const uint16_t *)(const void *)((const uint8_t *)py + y * ys);
            for (int x = 0; x < w; x++) d[x] = (uint8_t)(s[x] >> sh > 255 ? 255 : s[x] >> sh);
        }
    }
    for (int y = 0; y < ch; y++) {
        uint8_t *d = o->uv + (size_t)y * o->uvs;
        if (mono) {
            memset(d, 128, (size_t)o->uvs);
            continue;
        }
        ptrdiff_t row = (ptrdiff_t)(ssy ? y : 2 * y) * cs;
        if (bpc == 8 && ssx) {   // the common case: 8-bit 4:2:0 (or 4:2:2), interleaved 16 pairs at a time
            const uint8_t *u = (const uint8_t *)pu + row, *v = (const uint8_t *)pv + row;
            int x = 0;
            for (; x + 16 <= cw; x += 16) {
                __m128i a = _mm_loadu_si128((const __m128i *)(const void *)(u + x));
                __m128i b = _mm_loadu_si128((const __m128i *)(const void *)(v + x));
                _mm_storeu_si128((__m128i *)(void *)(d + 2 * x), _mm_unpacklo_epi8(a, b));
                _mm_storeu_si128((__m128i *)(void *)(d + 2 * x + 16), _mm_unpackhi_epi8(a, b));
            }
            for (; x < cw; x++) d[2 * x] = u[x], d[2 * x + 1] = v[x];
            continue;
        }
        for (int x = 0; x < cw; x++) {
            int sx = ssx ? x : 2 * x;
            unsigned u, v;
            if (bpc == 8) u = ((const uint8_t *)pu + row)[sx], v = ((const uint8_t *)pv + row)[sx];
            else {
                u = ((const uint16_t *)(const void *)((const uint8_t *)pu + row))[sx] >> sh;
                v = ((const uint16_t *)(const void *)((const uint8_t *)pv + row))[sx] >> sh;
            }
            d[2 * x] = (uint8_t)(u > 255 ? 255 : u);
            d[2 * x + 1] = (uint8_t)(v > 255 ? 255 : v);
        }
    }
}

static Pic *pic_from_dav1d(const Dav1dPicture *p) {
    Pic *o = pic_new(p->p.w, p->p.h);
    enum Dav1dPixelLayout l = p->p.layout;
    planar_to_nv12(o, p->data[0], p->stride[0], p->data[1], p->data[2], p->stride[1], p->p.bpc,
                   l == DAV1D_PIXEL_LAYOUT_I420 || l == DAV1D_PIXEL_LAYOUT_I422, l == DAV1D_PIXEL_LAYOUT_I420,
                   l == DAV1D_PIXEL_LAYOUT_I400);
    const Dav1dSequenceHeader *s = p->seq_hdr;
    int m = !s ? 0 : s->mtrx == DAV1D_MC_BT709 ? 709 : s->mtrx == DAV1D_MC_BT2020_NCL || s->mtrx == DAV1D_MC_BT2020_CL ? 2020
          : s->mtrx == DAV1D_MC_BT601 || s->mtrx == DAV1D_MC_BT470BG ? 601 : 0;
    o->matrix = pick_matrix(m, o->h);
    o->full = s && s->color_range;
    o->ts = p->m.timestamp;
    return o;
}

static Pic *pic_from_vpx(const vpx_image_t *im, int64_t ts) {
    Pic *o = pic_new((int)im->d_w, (int)im->d_h);
    int bpc = im->fmt & VPX_IMG_FMT_HIGHBITDEPTH ? (int)im->bit_depth : 8;
    planar_to_nv12(o, im->planes[VPX_PLANE_Y], im->stride[VPX_PLANE_Y], im->planes[VPX_PLANE_U], im->planes[VPX_PLANE_V],
                   im->stride[VPX_PLANE_U], bpc, im->x_chroma_shift != 0, im->y_chroma_shift != 0, 0);
    int m = im->cs == VPX_CS_BT_709 ? 709 : im->cs == VPX_CS_BT_2020 ? 2020
          : im->cs == VPX_CS_BT_601 || im->cs == VPX_CS_SMPTE_170 ? 601 : 0;
    o->matrix = pick_matrix(m, o->h);
    o->full = im->range == VPX_CR_FULL_RANGE;
    o->ts = ts;
    return o;
}

// ---- image pools: one host Pool per VideoDecoderImagePoolType, its pointer at +0
typedef struct { uint32_t magic; uint16_t w, h; uint32_t fmt, tiling; } Pref;   // VideoDecoderImagePreference
typedef struct {
    uint32_t magic;
    uint16_t w, h;
    uint32_t fmt, tiling, count;
    Pic **pic;                            // the picture of each image (g_pic_mu)
} Pool;
static Pool *g_pools[CV_POOLS];           // live pools (g_pic_mu)

static Pool *pool_find(uint64_t t) {      // g_pic_mu held
    Pool *p = t ? (Pool *)(uintptr_t)LD64(t) : NULL;
    for (int i = 0; p && i < CV_POOLS; i++)
        if (g_pools[i] == p) return p;
    return NULL;
}
static Pool *pool_of(uint64_t t) {
    plat_mutex_lock(&g_pic_mu);
    Pool *p = pool_find(t);
    plat_mutex_unlock(&g_pic_mu);
    return p;
}
static void pool_put(Pool *p, uint32_t i, Pic *pic) {   // takes the reference
    Pic *old = NULL;
    plat_mutex_lock(&g_pic_mu);
    if (p && i < p->count) {
        old = p->pic[i];
        p->pic[i] = pic;
        pic = NULL;
    }
    plat_mutex_unlock(&g_pic_mu);
    pic_unref(old);
    pic_unref(pic);
}
static Pic *pool_get(uint64_t t, uint32_t i, Pool **pool) {   // a reference to image i's picture, NULL if none
    plat_mutex_lock(&g_pic_mu);
    Pool *p = pool_find(t);
    Pic *pic = p && i < p->count ? pic_ref(p->pic[i]) : NULL;
    plat_mutex_unlock(&g_pic_mu);
    if (pool) *pool = p;
    return pic;
}

// ---- decoders
typedef struct { uint8_t *data; size_t size; int64_t ts; int eos; } Packet;
typedef struct { uint32_t magic, dec, idx, gen; } CvSync;   // nn::gs::NativeSyncHandle as the host makes it
typedef struct {
    Pool *pool;
    uint32_t pidx;
    int reg, filled, in_fifo, in_pend, eos;
    uint32_t gen, filled_gen;             // gen: of the current hand-over; filled_gen: of the last picture put in
    uint16_t w, h;
    int64_t ts;
} Buf;
typedef struct {
    uint32_t magic, serial;
    int codec;
    uint16_t w, h;
    uint64_t type, event;                 // the guest decoder type and its submission-ready event
    PlatMutex mu;
    PlatCond work, idle;
    PlatThread *thread;
    int quit, busy, started;
    uint64_t gen;                         // bumped by Flush, Stop and Finalize: a decode in flight is dropped
    Packet in[IN_MAX];                    // submitted, not decoded yet
    int in_head, nin;
    Pic *held[HOLD_CAP];                  // decoded, waiting for a buffer
    int nheld, eos_pending;
    int live;                             // counted in g_movies_live: from Initialize or a sample after the end, to the end of stream or Finalize
    int64_t eos_ts;
    Pic *last;                            // the last picture put in a buffer (end of stream shows it again)
    Buf buf[CV_BUFS];
    uint32_t fifo[CV_BUFS], fhead, fcount;   // acquirable buffers, oldest first
    uint32_t pend[CV_BUFS], phead, pcount;   // buffers handed to the decoder, filled in this order
    Dav1dContext *dav1d;
    vpx_codec_ctx_t vpx;
    int vpx_live;
    uint64_t submits, refused, packets, pictures, errors, decode_ns, max_ns;
} Dec;

static Dec *g_dec[CV_DECS];
static uint32_t g_serial;
static PlatMutex g_dec_mu = PLAT_MUTEX_INIT;   // lock order: g_dec_mu, a decoder's mu, g_spt_mu, g_pic_mu

static Dec *dec_of(uint64_t type) {       // the decoder whose pointer the guest type holds, NULL if none
    Dec *d = type ? (Dec *)(uintptr_t)LD64(type) : NULL, *r = NULL;
    plat_mutex_lock_shared(&g_dec_mu);
    for (int i = 0; d && i < CV_DECS && !r; i++)
        if (g_dec[i] == d) r = d;
    plat_mutex_unlock_shared(&g_dec_mu);
    return r;
}

static int ring_push(uint32_t *q, uint32_t head, uint32_t *count, uint32_t v) {
    if (*count >= CV_BUFS) return 0;
    q[(head + (*count)++) % CV_BUFS] = v;
    return 1;
}
static uint32_t ring_pop(uint32_t *q, uint32_t *head, uint32_t *count) {
    uint32_t v = q[*head];
    *head = (*head + 1) % CV_BUFS;
    --*count;
    return v;
}

// ---- sync point events (nn::syncpt::sdk). Their library is set up by the graphics initialisation the host replaces,
// so the SDK's own calls would jump through its empty tables (nxnative found CreateSyncPointEvent at pc 0). An event is
// a SystemEvent the host binds at +0 with the awaited handle at +0x40; an armed one is signalled when its buffer is
// filled with a picture of that hand-over or a later one.
typedef struct { uint32_t magic, armed; CvSync want; } Spt;
#define SPT(e) ((Spt *)(uintptr_t)((e) + SPT_OFF))
static uint64_t g_spt[SPT_MAX];           // armed events (g_spt_mu)
static PlatMutex g_spt_mu = PLAT_MUTEX_INIT;

static void spt_disarm(uint64_t e) {
    plat_mutex_lock(&g_spt_mu);
    for (int k = 0; k < SPT_MAX; k++)
        if (g_spt[k] == e) g_spt[k] = 0;
    SPT(e)->armed = 0;
    plat_mutex_unlock(&g_spt_mu);
}
// Buffer idx of decoder serial now holds the picture of hand-over gen (all = every event waiting on the decoder).
static void spt_fire(uint32_t serial, uint32_t idx, uint32_t gen, int all) {
    plat_mutex_lock(&g_spt_mu);
    for (int k = 0; k < SPT_MAX; k++) {
        uint64_t e = g_spt[k];
        if (!e) continue;
        const CvSync *w = &SPT(e)->want;
        if (w->dec == serial && (all || (w->idx == idx && gen >= w->gen))) {
            g_spt[k] = 0;
            SPT(e)->armed = 0;
            ops->signal(e);
        }
    }
    plat_mutex_unlock(&g_spt_mu);
}

// Whether the picture a handle waits for is in its buffer. A handle the host did not make (another nn::gs user, the
// empty handle), or one of a decoder or buffer that is gone, reads as reached: nothing would ever signal it.
static int sync_reached(const CvSync *s) {
    if (s->magic != SYNC_MAGIC) {
        static int once;
        if (!once++ && (s->magic || s->dec)) rt_log("codec: a sync handle the host did not make (%08x): reached", s->magic);
        return 1;
    }
    int r = 1;
    plat_mutex_lock_shared(&g_dec_mu);
    for (int i = 0; i < CV_DECS; i++) {
        Dec *d = g_dec[i];
        if (!d || d->serial != s->dec) continue;
        plat_mutex_lock_shared(&d->mu);
        if (s->idx < CV_BUFS && d->buf[s->idx].reg) r = d->buf[s->idx].filled_gen >= s->gen;
        plat_mutex_unlock_shared(&d->mu);
    }
    plat_mutex_unlock_shared(&g_dec_mu);
    return r;
}

// Pictures, then end of stream, into the buffers handed over, in hand-over order (d->mu held).
static void fill(Dec *d) {
    while (d->pcount && (d->nheld || d->eos_pending)) {
        uint32_t i = ring_pop(d->pend, &d->phead, &d->pcount);
        Buf *b = &d->buf[i];
        b->in_pend = 0;
        Pic *pic;
        if (d->nheld) {
            pic = d->held[0];
            memmove(d->held, d->held + 1, (size_t)--d->nheld * sizeof *d->held);
            b->w = (uint16_t)pic->w, b->h = (uint16_t)pic->h, b->ts = pic->ts, b->eos = 0;
            pic_unref(d->last);
            d->last = pic_ref(pic);
            if (!d->pictures++)
                rt_log("codec: %s decoder %u: first picture %dx%d (BT.%d, %s range) into buffer %u", codec_name(d->codec),
                       d->serial, pic->w, pic->h, pic->matrix, pic->full ? "full" : "limited", i);
        } else {   // end of stream: a buffer of its own, the last picture again in it
            pic = pic_ref(d->last);
            b->w = pic ? (uint16_t)pic->w : d->w, b->h = pic ? (uint16_t)pic->h : d->h;
            b->ts = d->eos_ts, b->eos = 1;
            d->eos_pending = 0;
            rt_log("codec: %s decoder %u: end of stream after %llu pictures (decode %.2f ms per packet, max %.2f, "
                   "%llu errors, %llu packets refused)", codec_name(d->codec), d->serial,
                   (unsigned long long)d->pictures, d->packets ? d->decode_ns / 1e6 / (double)d->packets : 0.0,
                   d->max_ns / 1e6, (unsigned long long)d->errors, (unsigned long long)d->refused);
        }
        pool_put(b->pool, b->pidx, pic);
        b->filled = 1;
        b->filled_gen = b->gen;
        spt_fire(d->serial, i, b->gen, 0);
    }
}

static void free_packet(const uint8_t *buf, void *cookie) {
    (void)cookie;
    free((void *)buf);
}

static int av1_threads(void) {
    const char *e = cfg_get("TOTK_AV1_THREADS");
    int n = e ? atoi(e) : 0;
    if (n > 0) return n > 32 ? 32 : n;
    uint32_t cpus = plat_cpu_count();
    return cpus >= 16 ? 6 : cpus >= 8 ? 4 : 2;
}

// Opens or resets the codec: a new stream starts at the next key frame. Only with the worker idle.
static int codec_open(Dec *d) {
    if (d->codec == CV_AV1) {
        if (d->dav1d) {
            dav1d_flush(d->dav1d);
            return 1;
        }
        Dav1dSettings s;
        dav1d_default_settings(&s);
        s.n_threads = av1_threads();
        if (dav1d_open(&d->dav1d, &s) < 0) {
            d->dav1d = NULL;
            return 0;
        }
        static int once;
        if (!once++) rt_log("codec: dav1d %s, %d threads, frame delay %d", dav1d_version(), s.n_threads,
                            dav1d_get_frame_delay(&s));
        return 1;
    }
    if (d->vpx_live) vpx_codec_destroy(&d->vpx);
    vpx_codec_dec_cfg_t cfg = {.threads = 4, .w = d->w, .h = d->h};
    d->vpx_live = vpx_codec_dec_init(&d->vpx, d->codec == CV_VP8 ? vpx_codec_vp8_dx() : vpx_codec_vp9_dx(), &cfg, 0) ==
                  VPX_CODEC_OK;
    if (d->vpx_live && d->codec == CV_VP9) vpx_codec_control(&d->vpx, VP9D_SET_ROW_MT, 1);
    return d->vpx_live;
}
static void codec_close(Dec *d) {
    if (d->dav1d) dav1d_close(&d->dav1d);
    if (d->vpx_live) vpx_codec_destroy(&d->vpx);
    d->vpx_live = 0;
}

static void decode_error(Dec *d, const char *what, int64_t ts) {
    if (++d->errors <= 4) rt_log("codec: %s decoder %u: %s at %lld us", codec_name(d->codec), d->serial, what,
                                 (long long)(ts / 1000));
}

// One packet through the codec (worker thread, d->mu not held); its pictures into out. An end-of-stream packet also
// drains the pictures the codec still holds.
static int decode(Dec *d, Packet *p, Pic **out, int max) {
    int n = 0;
    if (d->codec == CV_AV1) {
        Dav1dPicture pic;
        int r;
        if (p->size) {
            Dav1dData data;
            if (dav1d_data_wrap(&data, p->data, p->size, free_packet, NULL) < 0) {
                decode_error(d, "dav1d_data_wrap failed", p->ts);
                return 0;
            }
            p->data = NULL;   // dav1d frees it
            data.m.timestamp = p->ts;
            for (int guard = 0; data.sz && guard < 10000; guard++) {
                r = dav1d_send_data(d->dav1d, &data);
                if (r < 0 && r != DAV1D_ERR(EAGAIN)) {
                    decode_error(d, "dav1d_send_data failed", p->ts);
                    dav1d_data_unref(&data);
                    break;
                }
                memset(&pic, 0, sizeof pic);
                r = dav1d_get_picture(d->dav1d, &pic);
                if (!r) {
                    if (n < max) out[n++] = pic_from_dav1d(&pic);
                    dav1d_picture_unref(&pic);
                } else if (r != DAV1D_ERR(EAGAIN)) decode_error(d, "dav1d_get_picture failed", p->ts);
            }
            if (data.sz) dav1d_data_unref(&data);
        }
        if (p->eos) {
            for (;;) {
                memset(&pic, 0, sizeof pic);
                if ((r = dav1d_get_picture(d->dav1d, &pic)) < 0) {
                    if (r != DAV1D_ERR(EAGAIN)) decode_error(d, "dav1d drain failed", p->ts);
                    break;
                }
                if (n < max) out[n++] = pic_from_dav1d(&pic);
                dav1d_picture_unref(&pic);
            }
        }
        return n;
    }
    if (!p->size || !d->vpx_live) return 0;
    if (vpx_codec_decode(&d->vpx, p->data, (unsigned)p->size, NULL, 0) != VPX_CODEC_OK) {
        const char *e = vpx_codec_error_detail(&d->vpx);
        decode_error(d, e ? e : vpx_codec_error(&d->vpx), p->ts);
    }
    vpx_codec_iter_t it = NULL;
    const vpx_image_t *im;
    while ((im = vpx_codec_get_frame(&d->vpx, &it)) != NULL)
        if (n < max) out[n++] = pic_from_vpx(im, p->ts);
    return n;
}

// The worker: takes packets in order while fewer than HOLD_MAX pictures wait for a buffer, decodes outside the lock.
static void set_live(Dec *d, int live);

static void worker(void *arg) {
    Dec *d = arg;
    plat_mutex_lock(&d->mu);
    for (;;) {
        while (!d->quit && !(d->started && d->nin && d->nheld < HOLD_MAX)) plat_cond_wait(&d->work, &d->mu);
        if (d->quit) break;
        Packet p = d->in[d->in_head];
        d->in_head = (d->in_head + 1) % IN_MAX;
        d->nin--;
        uint64_t gen = d->gen;
        d->busy = 1;
        plat_mutex_unlock(&d->mu);
        Pic *out[HOLD_CAP];
        uint64_t t0 = plat_now_ns();
        int n = decode(d, &p, out, HOLD_CAP);
        uint64_t dt = plat_now_ns() - t0;
        free(p.data);
        plat_mutex_lock(&d->mu);
        d->busy = 0;
        plat_cond_broadcast(&d->idle);
        if (gen != d->gen) {   // flushed, stopped or finalized meanwhile
            for (int i = 0; i < n; i++) pic_unref(out[i]);
            continue;
        }
        d->packets++;
        d->decode_ns += dt;
        if (dt > d->max_ns) d->max_ns = dt;
        for (int i = 0; i < n; i++) {
            if (d->nheld < HOLD_CAP) d->held[d->nheld++] = out[i];
            else {
                decode_error(d, "more pictures than the host holds: one dropped", out[i]->ts);
                pic_unref(out[i]);
            }
        }
        if (p.eos) {
            d->eos_pending = 1;
            d->eos_ts = p.ts;
            set_live(d, 0);
        } else {
            set_live(d, 1);
        }
        fill(d);
        if (d->pictures && d->pictures % 1800 == 0 && n)
            rt_log("codec: %s decoder %u: %llu pictures, decode %.2f ms per packet (max %.2f)", codec_name(d->codec),
                   d->serial, (unsigned long long)d->pictures, d->decode_ns / 1e6 / (double)d->packets, d->max_ns / 1e6);
        if (d->nin < IN_READY) ops->signal(d->event);   // room for another packet
    }
    plat_mutex_unlock(&d->mu);
}

// Decoders playing a movie: frame_rate.c holds the frame rate meanwhile.
static int g_movies_live;
int codec_movie_live(void) { return __atomic_load_n(&g_movies_live, __ATOMIC_RELAXED) > 0; }
static void set_live(Dec *d, int live) {
    if (__atomic_exchange_n(&d->live, live, __ATOMIC_RELAXED) != live)
        __atomic_add_fetch(&g_movies_live, live ? 1 : -1, __ATOMIC_RELAXED);
}

// With d->mu held: drop a decode in flight and wait for the worker to leave it. Returns whether the calling guest
// thread gave its core up; pass it to kern_core_back after releasing d->mu.
static int quiesce(Dec *d) {
    int away = 0;
    d->gen++;
    while (d->busy) {
        if (!away) away = kern_core_away();
        plat_cond_wait(&d->idle, &d->mu);
    }
    return away;
}
static void drop_queued(Dec *d) {        // d->mu held, worker quiesced
    for (int i = 0; i < d->nin; i++) free(d->in[(d->in_head + i) % IN_MAX].data);
    d->in_head = d->nin = 0;
    for (int i = 0; i < d->nheld; i++) pic_unref(d->held[i]);
    d->nheld = 0;
    d->eos_pending = 0;
}

// ---- entries. Result values go to w0; arguments the SDK declares 16 bits wide are masked, as the guest does not
// extend them.
static void ok(Cpu *c) { ret_x(c, 0); }

// VideoDecoderImagePreference MakeDecodeImagePreference(u16 w, u16 h, VideoDecoderOutputFormat, ImageTilingMode):
// 0x40 bytes returned through x8, as the SDK's (sdk+0x331c30) writes them; only the host's pool functions read it.
static void make_pref(Cpu *c) {
    uint8_t *out = (uint8_t *)(uintptr_t)c->x[8];
    if (!out) return;
    Pref p = {PREF_MAGIC, (uint16_t)arg_x(c, 0), (uint16_t)arg_x(c, 1), (uint32_t)arg_x(c, 2), (uint32_t)arg_x(c, 3)};
    memset(out, 0, 0x40);
    memcpy(out, &p, sizeof p);
}

static uint64_t image_bytes(const Pref *p) {   // what one picture takes in the format, rounded to 4 KiB
    uint64_t px = (uint64_t)p->w * p->h, n = p->fmt == FMT_RGBA ? px * 4 : p->fmt == FMT_RGBA_HDR ? px * 8 : px * 3 / 2;
    return (n + 0xfff) & ~0xfffull;
}
// Result CalculateDecoderImagePoolBuffer(u64 *size, u64 *align, const VideoDecoderImagePreference &, u32 count). The
// host keeps the pictures itself; the size is what the pictures would take, so a game's memory budget stays as planned.
static void pool_size(Cpu *c) {
    uint64_t *size = arg_p(c, 0), *align = arg_p(c, 1);
    const Pref *p = arg_p(c, 2);
    uint32_t n = (uint32_t)arg_x(c, 3);
    if (!size || !align || !p) {
        ret_x(c, CV_BAD);
        return;
    }
    *size = image_bytes(p) * (n ? n : 1);
    *align = 0x1000;
    ret_x(c, 0);
}

// Result InitializeDecoderImagePool(VideoDecoderImagePoolType *, void *mem, u64 size, const Preference &, u32 count)
static void pool_init(Cpu *c) {
    uint64_t t = arg_x(c, 0);
    const Pref *pr = arg_p(c, 3);
    uint32_t n = (uint32_t)arg_x(c, 4);
    if (!t || !pr) {
        ret_x(c, CV_BAD);
        return;
    }
    if (pr->magic != PREF_MAGIC) rt_log("codec: image pool from a preference the host did not make");
    Pool *p = calloc(1, sizeof *p);
    if (!p || !(p->pic = calloc(n ? n : 1, sizeof *p->pic))) rt_fatal("codec: image pool allocation");
    *p = (Pool){POOL_MAGIC, pr->w, pr->h, pr->fmt, pr->tiling, n ? n : 1, p->pic};
    plat_mutex_lock(&g_pic_mu);
    int i = 0;
    while (i < CV_POOLS && g_pools[i]) i++;
    if (i < CV_POOLS) g_pools[i] = p;
    plat_mutex_unlock(&g_pic_mu);
    if (i == CV_POOLS) rt_fatal("codec: more than %d image pools", CV_POOLS);
    ST64(t, (uint64_t)(uintptr_t)p);
    static int once;
    if (!once++) rt_log("codec: image pool %ux%u, output format %u, tiling %u, %u picture(s): pictures kept on the host",
                        p->w, p->h, p->fmt, p->tiling, p->count);
    ret_x(c, 0);
}

// void FinalizeDecoderImagePool(VideoDecoderImagePoolType *). A decoder still holding one of its images keeps the
// buffer, with nowhere to put its picture.
static void pool_fini(Cpu *c) {
    uint64_t t = arg_x(c, 0);
    plat_mutex_lock(&g_pic_mu);
    Pool *p = pool_find(t);
    for (int i = 0; p && i < CV_POOLS; i++)
        if (g_pools[i] == p) g_pools[i] = NULL;
    plat_mutex_unlock(&g_pic_mu);
    ret_x(c, 0);
    if (!p) return;
    plat_mutex_lock_shared(&g_dec_mu);
    for (int k = 0; k < CV_DECS; k++) {
        Dec *d = g_dec[k];
        if (!d) continue;
        plat_mutex_lock(&d->mu);
        for (int i = 0; i < CV_BUFS; i++)
            if (d->buf[i].pool == p) d->buf[i].pool = NULL;
        plat_mutex_unlock(&d->mu);
    }
    plat_mutex_unlock_shared(&g_dec_mu);
    for (uint32_t i = 0; i < p->count; i++) pic_unref(p->pic[i]);
    free(p->pic);
    p->magic = 0;
    free(p);
    ST64(t, 0);
}

// NativeImageHandle GetDecoderImagePoolNativeImageHandle(const VideoDecoderImagePoolType *, u32): the GPU route
// (nvnTextureInitializeFromNativeImage over the pool's memory) is not served; 0.
static void pool_native(Cpu *c) {
    static int once;
    if (!once++) rt_log("codec: GetDecoderImagePoolNativeImageHandle: native images are not served (handle 0)");
    ret_x(c, 0);
}

// Copies rows of n bytes, at most rows of them, from src (stride ss, sw bytes valid per row, sh rows valid) to dst
// (stride ds); what lies outside the source is filled with fillv.
static void copy_plane(uint8_t *dst, uint64_t ds, const uint8_t *src, size_t ss, int sw, int sh, int x0, int y0, int n,
                       int rows, uint8_t fillv) {
    for (int y = 0; y < rows; y++) {
        uint8_t *d = dst + (size_t)y * ds;
        int sy = y0 + y, valid = src && sy < sh ? sw - x0 : 0;
        if (valid < 0) valid = 0;
        if (valid > n) valid = n;
        if (valid) memcpy(d, src + (size_t)sy * ss + x0, (size_t)valid);
        if (valid < n) memset(d + valid, fillv, (size_t)(n - valid));
    }
}

// Result ReadDecoderOutputImageNv12(const VideoDecoderImagePoolType *, u32 index, u16 x, u16 y, u16 w, u16 h,
//                                   void *y_dst, u64 y_stride, u64 y_size, void *uv_dst, u64 uv_stride, u64 uv_size)
// The region of image index as NV12; an image with no picture yet reads as black. Rows that would not fit the sizes
// given are left out (red line once).
static void read_nv12(Cpu *c) {
    uint64_t t = arg_x(c, 0);
    uint32_t i = (uint32_t)arg_x(c, 1);
    int x0 = (uint16_t)arg_x(c, 2) & ~1, y0 = (uint16_t)arg_x(c, 3) & ~1, w = (uint16_t)arg_x(c, 4),
        h = (uint16_t)arg_x(c, 5);
    uint8_t *yd = arg_p(c, 6), *uvd = arg_p(c, 9);
    uint64_t ys = arg_x(c, 7), ysize = arg_x(c, 8), uvs = arg_x(c, 10), uvsize = arg_x(c, 11);
    int cw = 2 * ((w + 1) / 2), ch = (h + 1) / 2;
    int yrows = !yd || ys < (uint64_t)w ? 0 : h, uvrows = !uvd || uvs < (uint64_t)cw ? 0 : ch;
    while (yrows && ys * (uint64_t)(yrows - 1) + (uint64_t)w > ysize) yrows--;
    while (uvrows && uvs * (uint64_t)(uvrows - 1) + (uint64_t)cw > uvsize) uvrows--;
    if (yrows < h || uvrows < ch) {
        static int once;
        if (!once++) rt_log("[RED] codec: ReadDecoderOutputImageNv12 %dx%d: strides %llu / %llu, sizes %llu / %llu too "
                            "small: rows left out", w, h, (unsigned long long)ys, (unsigned long long)uvs,
                            (unsigned long long)ysize, (unsigned long long)uvsize);
    }
    Pic *p = pool_get(t, i, NULL);
    copy_plane(yd, ys, p ? p->y : NULL, p ? (size_t)p->w : 0, p ? p->w : 0, p ? p->h : 0, x0, y0, w, yrows, 16);
    copy_plane(uvd, uvs, p ? p->uv : NULL, p ? (size_t)p->uvs : 0, p ? p->uvs : 0, p ? (p->h + 1) / 2 : 0, x0, y0 / 2, cw,
               uvrows, 128);
    pic_unref(p);
    // A host write into pool storage: without this the page tracking may not see it in time and a mirror keeps older bytes.
    if (yd && yrows) nvn_guest_written((uint64_t)(uintptr_t)yd, ys * (uint64_t)yrows);
    if (uvd && uvrows) nvn_guest_written((uint64_t)(uintptr_t)uvd, uvs * (uint64_t)uvrows);
    ret_x(c, 0);
}

// NV12 to RGBA8 with the picture's matrix and range, fixed point (16 fraction bits).
static void nv12_rgba_rows(const Pic *p, int x0, int y0, int w, int h, uint8_t *dst, uint64_t stride, int rows) {
    double kr = p->matrix == 709 ? 0.2126 : p->matrix == 2020 ? 0.2627 : 0.299;
    double kb = p->matrix == 709 ? 0.0722 : p->matrix == 2020 ? 0.0593 : 0.114, kg = 1.0 - kr - kb;
    double sy = p->full ? 1.0 : 255.0 / 219.0, sc = p->full ? 1.0 : 255.0 / 224.0;
    int Y = (int)(sy * 65536 + 0.5), RV = (int)(2 * (1 - kr) * sc * 65536 + 0.5), BU = (int)(2 * (1 - kb) * sc * 65536 + 0.5);
    int GU = (int)(2 * kb * (1 - kb) / kg * sc * 65536 + 0.5), GV = (int)(2 * kr * (1 - kr) / kg * sc * 65536 + 0.5);
    int y16 = p->full ? 0 : 16;
    for (int y = 0; y < rows; y++) {
        uint8_t *o = dst + (size_t)y * stride;
        int sy_ = y0 + y;
        for (int x = 0; x < w; x++) {
            int sx = x0 + x;
            if (sy_ >= p->h || sx >= p->w) {
                o[4 * x] = o[4 * x + 1] = o[4 * x + 2] = 0;
                o[4 * x + 3] = 255;
                continue;
            }
            const uint8_t *uv = p->uv + (size_t)(sy_ >> 1) * p->uvs + (size_t)(sx & ~1);
            int l = (p->y[(size_t)sy_ * p->w + sx] - y16) * Y, u = uv[0] - 128, v = uv[1] - 128;
            int r = (l + RV * v + 32768) >> 16, g = (l - GU * u - GV * v + 32768) >> 16, b = (l + BU * u + 32768) >> 16;
            o[4 * x] = (uint8_t)(r < 0 ? 0 : r > 255 ? 255 : r);
            o[4 * x + 1] = (uint8_t)(g < 0 ? 0 : g > 255 ? 255 : g);
            o[4 * x + 2] = (uint8_t)(b < 0 ? 0 : b > 255 ? 255 : b);
            o[4 * x + 3] = 255;
        }
    }
}

// Result ReadDecoderOutputImageRgba(const VideoDecoderImagePoolType *, u32 index, u16 x, u16 y, u16 w, u16 h,
//                                   void *dst, u64 stride, u64 size): the region as RGBA8; no picture yet reads black.
static void read_rgba(Cpu *c) {
    uint64_t t = arg_x(c, 0);
    uint32_t i = (uint32_t)arg_x(c, 1);
    int x0 = (uint16_t)arg_x(c, 2), y0 = (uint16_t)arg_x(c, 3), w = (uint16_t)arg_x(c, 4), h = (uint16_t)arg_x(c, 5);
    uint8_t *dst = arg_p(c, 6);
    uint64_t stride = arg_x(c, 7), size = arg_x(c, 8);
    int rows = !dst || stride < (uint64_t)w * 4 ? 0 : h;
    while (rows && stride * (uint64_t)(rows - 1) + (uint64_t)w * 4 > size) rows--;
    if (rows < h) {
        static int once;
        if (!once++) rt_log("[RED] codec: ReadDecoderOutputImageRgba %dx%d: stride %llu, size %llu too small: rows left "
                            "out", w, h, (unsigned long long)stride, (unsigned long long)size);
    }
    Pic *p = pool_get(t, i, NULL);
    if (p) nv12_rgba_rows(p, x0, y0, w, h, dst, stride, rows);
    else
        for (int y = 0; y < rows; y++) memset(dst + (size_t)y * stride, 0, (size_t)w * 4);
    pic_unref(p);
    ret_x(c, 0);
}

// Result ReadDecoderOutputImageRgbaHdr(same arguments): the HDR format's layout is not known here; black (zeros).
static void read_rgba_hdr(Cpu *c) {
    uint8_t *dst = arg_p(c, 6);
    uint64_t size = arg_x(c, 8);
    static int once;
    if (!once++) rt_log("[RED] codec: ReadDecoderOutputImageRgbaHdr: HDR pictures are not served, zeros");
    if (dst && size) memset(dst, 0, size);
    ret_x(c, 0);
}

// One decoder for every codec. Initialize puts the host decoder's pointer at +0 of the type and binds the type's
// submission-ready event, signalled: the decoder takes packets.
static void dec_init(Cpu *c, int codec, uint64_t type, uint16_t w, uint16_t h, uint32_t in, uint32_t out, uint32_t mode,
                     const char *form) {
    if (!type) {
        ret_x(c, CV_BAD);
        return;
    }
    if (codec == CV_H264) {
        static int once;
        if (!once++) rt_log("[RED] codec: H.264 decoder %ux%u (%s): no H.264 decoder on the host, Initialize fails", w, h,
                            form);
        ret_x(c, CV_NO_MEMORY);
        return;
    }
    Dec *d = calloc(1, sizeof *d);
    if (!d) rt_fatal("codec: decoder allocation");
    d->magic = DEC_MAGIC, d->codec = codec, d->w = w, d->h = h, d->type = type, d->event = type + CV_EVENT_OFF;
    plat_mutex_init(&d->mu);
    plat_cond_init(&d->work);
    plat_cond_init(&d->idle);
    if (!codec_open(d)) {
        rt_log("[RED] codec: %s decoder %ux%u: the codec does not open", codec_name(codec), w, h);
        free(d);
        ret_x(c, CV_NO_MEMORY);
        return;
    }
    plat_mutex_lock(&g_dec_mu);
    int i = 0;
    while (i < CV_DECS && g_dec[i]) i++;
    if (i < CV_DECS) {
        g_dec[i] = d;
        d->serial = ++g_serial;
    }
    plat_mutex_unlock(&g_dec_mu);
    if (i == CV_DECS) rt_fatal("codec: more than %d video decoders", CV_DECS);
    set_live(d, 1);
    ops->bind(c, d->event);
    ops->signal(d->event);
    char name[32];
    snprintf(name, sizeof name, "codec %s %u", codec_name(codec), d->serial);
    if (!(d->thread = plat_thread_new(worker, d, name))) rt_fatal("codec: cannot start the decoder thread");
    ST64(type, (uint64_t)(uintptr_t)d);
    rt_log("codec: %s decoder %u: %ux%u, input format %u, output format %u, output mode %u (%s; %s on the host)",
           codec_name(codec), d->serial, w, h, in, out, mode, form, codec == CV_AV1 ? "dav1d" : "libvpx");
    ret_x(c, 0);
}
// Result Initialize<C>DecoderByRemoteMemory(<C>DecoderType *, u64 bitstream size, u16 w, u16 h, u32 references,
//                                          VideoDecoderInputFormat, VideoDecoderOutputFormat, VideoDecoderOutputMode)
static void init_remote(Cpu *c, int codec) {
    dec_init(c, codec, arg_x(c, 0), (uint16_t)arg_x(c, 2), (uint16_t)arg_x(c, 3), (uint32_t)arg_x(c, 5),
             (uint32_t)arg_x(c, 6), (uint32_t)arg_x(c, 7), "by remote memory");
}
// Result Initialize<C>DecoderByTransferMemory(<C>DecoderType *, void *work, u64 work size, u64 bitstream size, u16 w,
//                                            u16 h, u32 references, input format, output format, output mode): ten
// arguments, the last two in the guest's stack slots.
static void init_transfer(Cpu *c, int codec) {
    dec_init(c, codec, arg_x(c, 0), (uint16_t)arg_x(c, 4), (uint16_t)arg_x(c, 5), (uint32_t)arg_x(c, 7),
             (uint32_t)arg_x(c, 8), (uint32_t)arg_x(c, 9), "by transfer memory");
}
static void av1_init_rm(Cpu *c) { init_remote(c, CV_AV1); }
static void av1_init_tm(Cpu *c) { init_transfer(c, CV_AV1); }
static void vp9_init_rm(Cpu *c) { init_remote(c, CV_VP9); }
static void vp9_init_tm(Cpu *c) { init_transfer(c, CV_VP9); }
static void vp8_init_rm(Cpu *c) { init_remote(c, CV_VP8); }
static void vp8_init_tm(Cpu *c) { init_transfer(c, CV_VP8); }
static void h264_init_rm(Cpu *c) { init_remote(c, CV_H264); }
static void h264_init_tm(Cpu *c) { init_transfer(c, CV_H264); }

// Result Finalize<C>Decoder(<C>DecoderType *)
static void dec_fini(Cpu *c) {
    uint64_t type = arg_x(c, 0);
    Dec *d = dec_of(type);
    ret_x(c, 0);
    if (!d) return;
    plat_mutex_lock(&g_dec_mu);   // out of the table first: its handles read as reached from here on
    for (int i = 0; i < CV_DECS; i++)
        if (g_dec[i] == d) g_dec[i] = NULL;
    plat_mutex_unlock(&g_dec_mu);
    set_live(d, 0);
    plat_mutex_lock(&d->mu);
    d->quit = 1;
    d->gen++;
    plat_cond_broadcast(&d->work);
    plat_mutex_unlock(&d->mu);
    int away = kern_core_away();
    plat_thread_join(d->thread);
    kern_core_back(&away);
    spt_fire(d->serial, 0, 0, 1);   // nothing will fill its buffers any more
    rt_log("codec: %s decoder %u finalized: %llu packets, %llu pictures, decode %.2f ms per packet (max %.2f), %llu "
           "errors, %llu packets refused", codec_name(d->codec), d->serial, (unsigned long long)d->packets,
           (unsigned long long)d->pictures, d->packets ? d->decode_ns / 1e6 / (double)d->packets : 0.0, d->max_ns / 1e6,
           (unsigned long long)d->errors, (unsigned long long)d->refused);
    drop_queued(d);
    pic_unref(d->last);
    codec_close(d);
    // The event goes with the decoder, as in the SDK: the next decoder made in this memory binds a new one.
    ops->destroy(c, d->event);
    ST64(type, 0);
    d->magic = 0;
    free(d);
}

// Result Register<C>DecoderBuffer(<C>DecoderType *, u32 index, VideoDecoderImagePoolType *, u32 image): the buffer is
// acquirable and the decoder's to fill (see the top of the file).
static void dec_register(Cpu *c) {
    Dec *d = dec_of(arg_x(c, 0));
    uint32_t i = (uint32_t)arg_x(c, 1), pidx = (uint32_t)arg_x(c, 3);
    Pool *pool = pool_of(arg_x(c, 2));
    if (!d || i >= CV_BUFS) {
        rt_log("[RED] codec: Register...DecoderBuffer: buffer %u of %s", i, d ? "a host decoder" : "no host decoder");
        ret_x(c, CV_BAD);
        return;
    }
    if (!pool) rt_log("[RED] codec: buffer %u registered with an image pool the host does not know", i);
    plat_mutex_lock(&d->mu);
    Buf *b = &d->buf[i];
    b->pool = pool, b->pidx = pidx, b->reg = 1, b->filled = 0, b->eos = 0;
    b->gen++;   // handles of an earlier registration stay reached; this one's are not until a picture comes
    if (!b->in_fifo) b->in_fifo = ring_push(d->fifo, d->fhead, &d->fcount, i);
    if (!b->in_pend) b->in_pend = ring_push(d->pend, d->phead, &d->pcount, i);
    fill(d);
    plat_cond_signal(&d->work);
    plat_mutex_unlock(&d->mu);
    ret_x(c, 0);
}

// Result Start<C>Decoder(<C>DecoderType *): decoding begins; the event says packets can come (nxnative: Mario Tennis
// Fever's player drops the signal Initialize gave while not started and waits for a new one).
static void dec_start(Cpu *c) {
    Dec *d = dec_of(arg_x(c, 0));
    if (!d) {
        ret_x(c, CV_BAD);
        return;
    }
    plat_mutex_lock(&d->mu);
    d->started = 1;
    plat_cond_signal(&d->work);
    if (d->nin < IN_READY) ops->signal(d->event);
    plat_mutex_unlock(&d->mu);
    ret_x(c, 0);
}

// Stop and Flush: packets not decoded, pictures not in a buffer yet and a pending end of stream are dropped, the codec
// starts over at the next key frame. Buffers handed over stay the decoder's: the next pictures go into them.
static void reset(Cpu *c, int stop) {
    Dec *d = dec_of(arg_x(c, 0));
    if (!d) {
        ret_x(c, CV_BAD);
        return;
    }
    plat_mutex_lock(&d->mu);
    int away = quiesce(d);
    drop_queued(d);
    if (!codec_open(d)) rt_log("[RED] codec: %s decoder %u: the codec does not reopen", codec_name(d->codec), d->serial);
    if (stop) d->started = 0;
    ops->signal(d->event);
    plat_mutex_unlock(&d->mu);
    kern_core_back(&away);
    ret_x(c, 0);
}
static void dec_stop(Cpu *c) { reset(c, 1); }
static void dec_flush(Cpu *c) { reset(c, 0); }

// os::SystemEventType *Get<C>DecoderSubmissionReadyEvent(<C>DecoderType *) (SDK 22 returns it)
static void dec_event(Cpu *c) {
    Dec *d = dec_of(arg_x(c, 0));
    ret_x(c, d ? d->event : 0);
}

// Result Submit<C>DecoderInputBuffer(u64 *id, <C>DecoderType *, const void *data, u64 size, TimeSpan time, bool eos):
// the packet is copied and queued. While IN_READY or more wait, the event is cleared; at IN_MAX the packet is refused.
static void dec_submit(Cpu *c) {
    uint64_t *id = arg_p(c, 0);
    Dec *d = dec_of(arg_x(c, 1));
    const uint8_t *data = arg_p(c, 2);
    uint64_t size = arg_x(c, 3);
    int64_t ts = (int64_t)arg_x(c, 4);
    int eos = (arg_x(c, 5) & 0xff) != 0;
    if (!d || (size && !data) || size > (256u << 20)) {
        ret_x(c, CV_BAD);
        return;
    }
    uint8_t *copy = NULL;
    if (size) {
        if (!(copy = malloc((size_t)size))) rt_fatal("codec: packet allocation (%llu bytes)", (unsigned long long)size);
        memcpy(copy, data, (size_t)size);
    }
    plat_mutex_lock(&d->mu);
    int full = d->nin >= IN_MAX;
    if (full) d->refused++;
    else {
        d->in[(d->in_head + d->nin++) % IN_MAX] = (Packet){copy, (size_t)size, ts, eos};
        copy = NULL;
        if (id) *id = ++d->submits;
        plat_cond_signal(&d->work);
    }
    int room = d->nin < IN_READY;
    if (room) ops->signal(d->event);
    plat_mutex_unlock(&d->mu);
    free(copy);
    if (!room) {   // cleared on this guest thread; the worker may have made room in between, so look again
        ops->clear(c, d->event);
        plat_mutex_lock(&d->mu);
        if (d->nin < IN_READY) ops->signal(d->event);
        plat_mutex_unlock(&d->mu);
    }
    if (full && d->refused <= 4)
        rt_log("codec: %s decoder %u: a packet refused with %d waiting", codec_name(d->codec), d->serial, IN_MAX);
    ret_x(c, full ? CV_BUSY : 0);
}

// Result Acquire<C>DecoderOutputBuffer(u32 *index, gs::NativeSyncHandle *, <C>DecoderType *): the oldest acquirable
// buffer and the handle that signals when the picture of its current hand-over is in it.
static void dec_acquire(Cpu *c) {
    uint32_t *idx = arg_p(c, 0);
    CvSync *sync = arg_p(c, 1);
    Dec *d = dec_of(arg_x(c, 2));
    if (!d || !idx || !sync) {
        ret_x(c, CV_BAD);
        return;
    }
    plat_mutex_lock(&d->mu);
    int st = CV_EMPTY;
    if (d->fcount) {
        uint32_t i = ring_pop(d->fifo, &d->fhead, &d->fcount);
        d->buf[i].in_fifo = 0;
        *idx = i;
        *sync = (CvSync){SYNC_MAGIC, d->serial, i, d->buf[i].gen};
        st = 0;
    }
    plat_mutex_unlock(&d->mu);
    ret_x(c, (uint32_t)st);
}

// Release<C>DecoderOutputBuffer(<C>DecoderType *, u32 index, gs::NativeSyncHandle ready): the client is done with the
// buffer (the handle, in x2 and x3, would say when its GPU is; the client read the picture on the CPU already); the
// buffer is the decoder's to fill again and acquirable at once.
static void dec_release(Cpu *c) {
    Dec *d = dec_of(arg_x(c, 0));
    uint32_t i = (uint32_t)arg_x(c, 1);
    if (!d || i >= CV_BUFS) {
        rt_log("[RED] codec: Release...DecoderOutputBuffer: buffer %u of %s", i, d ? "a host decoder" : "no host decoder");
        ret_x(c, CV_BAD);
        return;
    }
    plat_mutex_lock(&d->mu);
    Buf *b = &d->buf[i];
    int st = CV_BAD;
    if (b->reg) {
        b->gen++;
        b->filled = 0;
        if (!b->in_pend) b->in_pend = ring_push(d->pend, d->phead, &d->pcount, i);
        if (!b->in_fifo) b->in_fifo = ring_push(d->fifo, d->fhead, &d->fcount, i);
        fill(d);
        plat_cond_signal(&d->work);
        st = 0;
    }
    plat_mutex_unlock(&d->mu);
    if (st) rt_log("[RED] codec: Release...DecoderOutputBuffer: buffer %u is not registered", i);
    ret_x(c, (uint32_t)st);
}

// Result Get<C>DecoderOutputBufferProperties(u16 *w, u16 *h, u16 *crop_w, u16 *crop_h, TimeSpan *, bool *eos,
//                                            <C>DecoderType *, u32 index): 3604 until the buffer holds its picture.
static void dec_props(Cpu *c) {
    uint16_t *w = arg_p(c, 0), *h = arg_p(c, 1), *cw = arg_p(c, 2), *ch = arg_p(c, 3);
    int64_t *ts = arg_p(c, 4);
    uint8_t *eos = arg_p(c, 5);
    Dec *d = dec_of(arg_x(c, 6));
    uint32_t i = (uint32_t)arg_x(c, 7);
    if (!d || i >= CV_BUFS) {
        ret_x(c, CV_BAD);
        return;
    }
    plat_mutex_lock_shared(&d->mu);
    const Buf *b = &d->buf[i];
    int st = b->filled ? 0 : CV_EMPTY;
    if (!st) {
        if (w) *w = b->w;
        if (h) *h = b->h;
        if (cw) *cw = b->w;
        if (ch) *ch = b->h;
        if (ts) *ts = b->ts;
        if (eos) *eos = (uint8_t)b->eos;
    }
    plat_mutex_unlock_shared(&d->mu);
    ret_x(c, (uint32_t)st);
}

// bool nn::gs::TryWaitNativeSync(const NativeSyncHandle &)
static void gs_try_wait(Cpu *c) {
    const CvSync *s = arg_p(c, 0);
    ret_x(c, s ? (uint64_t)sync_reached(s) : 1);
}

// Result nn::syncpt::sdk::CreateSyncPointEvent(SyncPointEventType *)
static void spt_create(Cpu *c) {
    uint64_t e = arg_x(c, 0);
    if (!e) {
        ret_x(c, CV_BAD);
        return;
    }
    memset(SPT(e), 0, sizeof(Spt));
    SPT(e)->magic = SPT_MAGIC;
    ops->bind(c, e);
    ops->clear(c, e);
    ret_x(c, 0);
}
// void nn::syncpt::sdk::InitializeMultiWaitHolder(os::MultiWaitHolderType *, SyncPointEventType *): the event's
// SystemEvent goes into the client's MultiWait through the SDK's own holder.
static void spt_holder(Cpu *c) { ops->holder(c, arg_x(c, 0), arg_x(c, 1)); }
// Result nn::syncpt::sdk::RequestSyncPointEvent(SyncPointEventType *, const SyncPointHandle &): signalled when the
// handle is reached; at once if it already is.
static void spt_request(Cpu *c) {
    uint64_t e = arg_x(c, 0);
    const CvSync *h = arg_p(c, 1);
    if (!e || !h) {
        ret_x(c, CV_BAD);
        return;
    }
    CvSync want = *h;
    spt_disarm(e);
    ops->clear(c, e);
    plat_mutex_lock(&g_spt_mu);
    int k = 0;
    while (k < SPT_MAX && g_spt[k]) k++;
    if (k < SPT_MAX) {
        g_spt[k] = e;
        SPT(e)->want = want;
        SPT(e)->armed = 1;
    }
    plat_mutex_unlock(&g_spt_mu);
    if (k == SPT_MAX) {
        static int once;
        if (!once++) rt_log("[RED] codec: more than %d armed sync point events: signalled at once", SPT_MAX);
        ops->signal(e);
    } else if (sync_reached(&want)) {   // a fill that came before the arming
        spt_disarm(e);
        ops->signal(e);
    }
    ret_x(c, 0);
}
// void nn::syncpt::sdk::CancelSyncPointEvent(SyncPointEventType *)
static void spt_cancel(Cpu *c) {
    uint64_t e = arg_x(c, 0);
    if (!e) return;
    spt_disarm(e);
    ops->clear(c, e);
}
// void nn::syncpt::sdk::DestroySyncPointEvent(SyncPointEventType *)
static void spt_destroy(Cpu *c) {
    uint64_t e = arg_x(c, 0);
    if (!e) return;
    spt_disarm(e);
    ops->destroy(c, e);
    SPT(e)->magic = 0;
}

// ---- symbols. The four decoder types share one layout and every entry but Initialize; their names differ only in
// the codec (Av1, Vp9 and Vp8 have three letters, H264 four, hence its own lengths).
#define N_INIT_RM(C) "_ZN2nn5codec34Initialize" C "DecoderByRemoteMemoryEPNS0_14" C "DecoderTypeEmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeE"
#define N_INIT_TM(C) "_ZN2nn5codec36Initialize" C "DecoderByTransferMemoryEPNS0_14" C "DecoderTypeEPvmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeE"
#define N_FINI(C) "_ZN2nn5codec18Finalize" C "DecoderEPNS0_14" C "DecoderTypeE"
#define N_REG(C) "_ZN2nn5codec24Register" C "DecoderBufferEPNS0_14" C "DecoderTypeEjPNS0_25VideoDecoderImagePoolTypeEj"
#define N_START(C) "_ZN2nn5codec15Start" C "DecoderEPNS0_14" C "DecoderTypeE"
#define N_STOP(C) "_ZN2nn5codec14Stop" C "DecoderEPNS0_14" C "DecoderTypeE"
#define N_FLUSH(C) "_ZN2nn5codec15Flush" C "DecoderEPNS0_14" C "DecoderTypeE"
#define N_EVENT(C) "_ZN2nn5codec33Get" C "DecoderSubmissionReadyEventEPNS0_14" C "DecoderTypeE"
#define N_SUBMIT(C) "_ZN2nn5codec27Submit" C "DecoderInputBufferEPmPNS0_14" C "DecoderTypeEPKvmNS_8TimeSpanEb"
#define N_ACQ(C) "_ZN2nn5codec29Acquire" C "DecoderOutputBufferEPjPNS_2gs16NativeSyncHandleEPNS0_14" C "DecoderTypeE"
#define N_REL(C) "_ZN2nn5codec29Release" C "DecoderOutputBufferEPNS0_14" C "DecoderTypeEjNS_2gs16NativeSyncHandleE"
#define N_PROPS(C) "_ZN2nn5codec35Get" C "DecoderOutputBufferPropertiesEPtS1_S1_S1_PNS_8TimeSpanEPbPNS0_14" C "DecoderTypeEj"
#define N_PREF "_ZN2nn5codec25MakeDecodeImagePreferenceEttNS0_24VideoDecoderOutputFormatENS0_15ImageTilingModeE"
#define N_POOL_SIZE "_ZN2nn5codec31CalculateDecoderImagePoolBufferEPmS1_RKNS0_27VideoDecoderImagePreferenceEj"
#define N_POOL_INIT "_ZN2nn5codec26InitializeDecoderImagePoolEPNS0_25VideoDecoderImagePoolTypeEPvmRKNS0_27VideoDecoderImagePreferenceEj"
#define N_POOL_FINI "_ZN2nn5codec24FinalizeDecoderImagePoolEPNS0_25VideoDecoderImagePoolTypeE"
#define N_POOL_NATIVE "_ZN2nn5codec36GetDecoderImagePoolNativeImageHandleEPKNS0_25VideoDecoderImagePoolTypeEj"
#define N_READ_NV12 "_ZN2nn5codec26ReadDecoderOutputImageNv12EPKNS0_25VideoDecoderImagePoolTypeEjttttPvmmS4_mm"
#define N_READ_RGBA "_ZN2nn5codec26ReadDecoderOutputImageRgbaEPKNS0_25VideoDecoderImagePoolTypeEjttttPvmm"
#define N_READ_HDR "_ZN2nn5codec29ReadDecoderOutputImageRgbaHdrEPKNS0_25VideoDecoderImagePoolTypeEjttttPvmm"
#define N_VIDEO_INIT "_ZN2nn5codec5video10InitializeEv"
#define N_VIDEO_FINI "_ZN2nn5codec5video8FinalizeEv"
#define N_TRY_WAIT "_ZN2nn2gs17TryWaitNativeSyncERKNS0_16NativeSyncHandleE"
#define N_SPT_CREATE "_ZN2nn6syncpt3sdk20CreateSyncPointEventEPNS1_18SyncPointEventTypeE"
#define N_SPT_HOLDER "_ZN2nn6syncpt3sdk25InitializeMultiWaitHolderEPNS_2os19MultiWaitHolderTypeEPNS1_18SyncPointEventTypeE"
#define N_SPT_REQUEST "_ZN2nn6syncpt3sdk21RequestSyncPointEventEPNS1_18SyncPointEventTypeERKNS1_15SyncPointHandleE"
#define N_SPT_CANCEL "_ZN2nn6syncpt3sdk20CancelSyncPointEventEPNS1_18SyncPointEventTypeE"
#define N_SPT_DESTROY "_ZN2nn6syncpt3sdk21DestroySyncPointEventEPNS1_18SyncPointEventTypeE"

#define CV_FAMILY(C, tag)                                                                                              \
    HOST_OVERRIDE(N_INIT_RM(C), tag##_init_rm);                                                                        \
    HOST_OVERRIDE(N_INIT_TM(C), tag##_init_tm);                                                                        \
    HOST_OVERRIDE(N_FINI(C), dec_fini);                                                                                \
    HOST_OVERRIDE(N_REG(C), dec_register);                                                                             \
    HOST_OVERRIDE(N_START(C), dec_start);                                                                              \
    HOST_OVERRIDE(N_STOP(C), dec_stop);                                                                                \
    HOST_OVERRIDE(N_FLUSH(C), dec_flush);                                                                              \
    HOST_OVERRIDE(N_EVENT(C), dec_event);                                                                              \
    HOST_OVERRIDE(N_SUBMIT(C), dec_submit);                                                                            \
    HOST_OVERRIDE(N_ACQ(C), dec_acquire);                                                                              \
    HOST_OVERRIDE(N_REL(C), dec_release);                                                                              \
    HOST_OVERRIDE(N_PROPS(C), dec_props)
CV_FAMILY("Av1", av1);
CV_FAMILY("Vp9", vp9);
CV_FAMILY("Vp8", vp8);
#undef CV_FAMILY
// H.264: Initialize fails, so the other entries only ever see no decoder.
HOST_OVERRIDE("_ZN2nn5codec35InitializeH264DecoderByRemoteMemoryEPNS0_15H264DecoderTypeEmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeEPKvm", h264_init_rm);
HOST_OVERRIDE("_ZN2nn5codec37InitializeH264DecoderByTransferMemoryEPNS0_15H264DecoderTypeEPvmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeEPKvm", h264_init_tm);
HOST_OVERRIDE("_ZN2nn5codec19FinalizeH264DecoderEPNS0_15H264DecoderTypeE", dec_fini);
HOST_OVERRIDE("_ZN2nn5codec25RegisterH264DecoderBufferEPNS0_15H264DecoderTypeEjPNS0_25VideoDecoderImagePoolTypeEj", dec_register);
HOST_OVERRIDE("_ZN2nn5codec16StartH264DecoderEPNS0_15H264DecoderTypeE", dec_start);
HOST_OVERRIDE("_ZN2nn5codec15StopH264DecoderEPNS0_15H264DecoderTypeE", dec_stop);
HOST_OVERRIDE("_ZN2nn5codec16FlushH264DecoderEPNS0_15H264DecoderTypeE", dec_flush);
HOST_OVERRIDE("_ZN2nn5codec34GetH264DecoderSubmissionReadyEventEPNS0_15H264DecoderTypeE", dec_event);
HOST_OVERRIDE("_ZN2nn5codec28SubmitH264DecoderInputBufferEPmPNS0_15H264DecoderTypeEPKvmNS_8TimeSpanEb", dec_submit);
HOST_OVERRIDE("_ZN2nn5codec30AcquireH264DecoderOutputBufferEPjPNS_2gs16NativeSyncHandleEPNS0_15H264DecoderTypeE", dec_acquire);
HOST_OVERRIDE("_ZN2nn5codec30ReleaseH264DecoderOutputBufferEPNS0_15H264DecoderTypeEjNS_2gs16NativeSyncHandleE", dec_release);
HOST_OVERRIDE("_ZN2nn5codec36GetH264DecoderOutputBufferPropertiesEPtS1_S1_S1_PNS_8TimeSpanEPbPNS0_15H264DecoderTypeEj", dec_props);
HOST_OVERRIDE(N_VIDEO_INIT, ok);
HOST_OVERRIDE(N_VIDEO_FINI, ok);
HOST_OVERRIDE(N_PREF, make_pref);
HOST_OVERRIDE(N_POOL_SIZE, pool_size);
HOST_OVERRIDE(N_POOL_INIT, pool_init);
HOST_OVERRIDE(N_POOL_FINI, pool_fini);
HOST_OVERRIDE(N_POOL_NATIVE, pool_native);
HOST_OVERRIDE(N_READ_NV12, read_nv12);
HOST_OVERRIDE(N_READ_RGBA, read_rgba);
HOST_OVERRIDE(N_READ_HDR, read_rgba_hdr);
HOST_OVERRIDE(N_TRY_WAIT, gs_try_wait);
HOST_OVERRIDE(N_SPT_CREATE, spt_create);
HOST_OVERRIDE(N_SPT_HOLDER, spt_holder);
HOST_OVERRIDE(N_SPT_REQUEST, spt_request);
HOST_OVERRIDE(N_SPT_CANCEL, spt_cancel);
HOST_OVERRIDE(N_SPT_DESTROY, spt_destroy);

// ---- totk.exe --probe-codec FILE.webm: one AV1, VP9 or VP8 WebM through the entries above, in the order OoT's
// movie library calls them (see the top of the file), with a Cpu of its own. The SystemEvents are host
// stand-ins (a flag per event); the probe plays the library's event thread by polling them.
//   1. every packet in, every picture out through the registered buffers, the sync point event and TryWaitNativeSync;
//      the count, size and time of each picture against the container, NV12 readback of each, RGBA of the middle one;
//   2. a Flush, then the first 120 packets again: the pictures start over at the first time;
//   3. Stop, packets sent without waiting: the submission-ready event goes down, the one past IN_MAX is refused (3603),
//      Flush empties the queue; an image with no picture reads as black;
//   4. Finalize: the decoder's event is destroyed, pools and the sync point event finalized.
// TOTK_PROBE_CODEC_SHOT=<file.png> writes the middle picture (RGBA readback). Exit code 0 when every check passed.
#define CHECK(cond, ...)                                                                                              \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("probe-codec: FAILED: ");                                                                           \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (0)
#define U(x) ((uint64_t)(uintptr_t)(x))

typedef struct { uint32_t off, size; int64_t pts_ns; } PFrame;
typedef struct { PFrame *f; int n, cap, w, h; char codec[16]; } PTrack;

static int pvint(const uint8_t *p, const uint8_t *end, int id, uint64_t *v, int *len, int *unknown) {
    if (p >= end) return 0;
    int n = 1;
    uint8_t mask = 0x80;
    while (n <= 8 && !(p[0] & mask)) mask >>= 1, n++;
    if (n > 8 || p + n > end) return 0;
    uint64_t x = id ? p[0] : (uint64_t)(p[0] & (mask - 1));
    for (int i = 1; i < n; i++) x = x << 8 | p[i];
    *v = x, *len = n;
    if (unknown) *unknown = !id && x == (1ull << (7 * n)) - 1;
    return 1;
}
static uint64_t pbe(const uint8_t *p, uint64_t n) {
    uint64_t x = 0;
    for (uint64_t i = 0; i < n && i < 8; i++) x = x << 8 | p[i];
    return x;
}
// The first video track's blocks of a Matroska/WebM file, walking it linearly (masters entered, the rest skipped).
static const char *pdemux(const uint8_t *d, size_t size, PTrack *t) {
    const uint8_t *end = d + size, *p = d;
    uint64_t scale = 1000000, cluster = 0, cur = 0, vtrack = 0, ttype = 0;
    char codec[16] = "";
    while (p < end) {
        uint64_t id, n;
        int il, sl, unknown;
        if (!pvint(p, end, 1, &id, &il, NULL) || !pvint(p + il, end, 0, &n, &sl, &unknown)) return "bad element header";
        const uint8_t *body = p + il + sl;
        if (id == 0x18538067 || id == 0x1549A966 || id == 0x1654AE6B || id == 0xE0 || id == 0xA0 || id == 0x1F43B675) {
            p = body;   // Segment, Info, Tracks, Video, BlockGroup, Cluster
            continue;
        }
        if (id == 0xAE) {   // TrackEntry: read it whole, keep the first video track
            cur = ttype = 0;
            codec[0] = 0;
            p = body;
            continue;
        }
        if (unknown || n > (uint64_t)(end - body)) return "element past the end of the file";
        switch (id) {
        case 0x2AD7B1: scale = pbe(body, n); break;     // TimecodeScale (ns)
        case 0xD7: cur = pbe(body, n); break;            // TrackNumber
        case 0x83:                                       // TrackType: 1 video
            ttype = pbe(body, n);
            if (ttype == 1 && !vtrack && cur) vtrack = cur, snprintf(t->codec, sizeof t->codec, "%s", codec);
            break;
        case 0x86:                                       // CodecID
            if (n < sizeof codec) memcpy(codec, body, n), codec[n] = 0;
            if (ttype == 1 && !vtrack && cur) vtrack = cur, snprintf(t->codec, sizeof t->codec, "%s", codec);
            break;
        case 0xB0: if (!t->w) t->w = (int)pbe(body, n); break;   // PixelWidth
        case 0xBA: if (!t->h) t->h = (int)pbe(body, n); break;   // PixelHeight
        case 0xE7: cluster = pbe(body, n); break;                // Cluster Timecode
        case 0xA3: case 0xA1: {                                  // SimpleBlock, Block
            uint64_t track;
            int tl;
            if (!pvint(body, body + n, 0, &track, &tl, NULL) || n < (uint64_t)tl + 3) return "bad block";
            if (track != vtrack) break;
            if (body[tl + 2] & 0x06) return "laced video block";
            if (t->n == t->cap && !(t->f = realloc(t->f, (size_t)(t->cap = t->cap ? 2 * t->cap : 1024) * sizeof *t->f)))
                return "frame list allocation";
            int64_t tc = (int64_t)cluster + (int16_t)(body[tl] << 8 | body[tl + 1]);
            t->f[t->n++] = (PFrame){(uint32_t)(body + tl + 3 - d), (uint32_t)(n - tl - 3), tc * (int64_t)scale};
            break;
        }
        }
        p = body + n;
    }
    return NULL;
}

// SystemEvent stand-ins: word 0 is the signal, word 1 the state (1 bound, 2 destroyed); holders are recorded.
static uint64_t p_holder_of;
static void p_bind(Cpu *c, uint64_t ev) {
    (void)c;
    __atomic_store_n((int32_t *)(uintptr_t)ev + 1, 1, __ATOMIC_SEQ_CST);
}
static void p_signal(uint64_t ev) {
    if (__atomic_load_n((int32_t *)(uintptr_t)ev + 1, __ATOMIC_SEQ_CST) == 1)
        __atomic_store_n((int32_t *)(uintptr_t)ev, 1, __ATOMIC_SEQ_CST);
}
static void p_clear(Cpu *c, uint64_t ev) {
    (void)c;
    __atomic_store_n((int32_t *)(uintptr_t)ev, 0, __ATOMIC_SEQ_CST);
}
static void p_destroy(Cpu *c, uint64_t ev) {
    (void)c;
    __atomic_store_n((int32_t *)(uintptr_t)ev + 1, 2, __ATOMIC_SEQ_CST);
}
static void p_holder(Cpu *c, uint64_t holder, uint64_t ev) {
    (void)c;
    (void)holder;
    p_holder_of = ev;
}
static const CvOps probe_ops = {p_bind, p_signal, p_clear, p_destroy, p_holder};
static int p_set(uint64_t ev) { return __atomic_load_n((int32_t *)(uintptr_t)ev, __ATOMIC_SEQ_CST); }

static Cpu *PC;
static uint64_t pstack[8];
static uint64_t pcall(const char *name, int n, ...) {
    GuestFn f = hostfn_override(name);
    if (!f) rt_fatal("probe-codec: no override for %s", name);
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n; i++) {
        uint64_t v = va_arg(ap, uint64_t);
        if (i < 8) PC->x[i] = v;
        else ST64(PC->sp + 8 * (uint64_t)(i - 8), v);
    }
    va_end(ap);
    f(PC);
    return PC->x[0];
}

typedef struct {
    const char *init_rm, *fini, *reg, *start, *stop, *flush, *event, *submit, *acq, *rel, *props;
} PNames;
#define PNAMES(C) {N_INIT_RM(C), N_FINI(C), N_REG(C), N_START(C), N_STOP(C), N_FLUSH(C), N_EVENT(C), N_SUBMIT(C), N_ACQ(C), N_REL(C), N_PROPS(C)}

enum { P_FRAMES = 4 };   // frames the probe registers, as RegisterVideoFrame does for each of the game's
typedef struct {
    const PNames *nm;
    uint64_t type, spe;
    uint64_t pool[P_FRAMES];
    CvSync post[P_FRAMES];
    int ring[P_FRAMES], head, count;     // frames handed to the decoder, oldest first (the library's pending ring)
} PState;

// The library's +0x46b0: the frame back to the decoder and a new fence for it.
static int p_recycle(PState *s, int k) {
    CvSync pre = {0};
    uint64_t r[2];
    memcpy(r, &pre, sizeof r);
    pcall(s->nm->rel, 4, s->type, (uint64_t)k, r[0], r[1]);
    CHECK((uint32_t)PC->x[0] == 0, "Release buffer %d: %#x", k, (unsigned)PC->x[0]);
    uint32_t idx = 99;
    CvSync sync = {0};
    CHECK((uint32_t)pcall(s->nm->acq, 3, U(&idx), U(&sync), s->type) == 0 && idx == (uint32_t)k,
          "Acquire after Release of %d gave %u", k, idx);
    s->post[k] = sync;
    s->ring[(s->head + s->count++) % P_FRAMES] = k;
    return 0;
}

// Feeds packets [*next, last) and, at last, the end of stream, while the submission-ready event is up; then takes the
// pictures the sync point event reports. Returns 0 when the end of stream came out; got counts the pictures.
static int p_run(PState *s, const uint8_t *data, const PTrack *t, int first, int last, uint64_t ev, int *got,
                 uint8_t *frame, const char *shot, double *luma_lo, double *luma_hi) {
    int next = first, eos_in = 0, eos_out = 0, idle = 0, w = t->w, h = t->h, mid = first + (last - first) / 2;
    int64_t prev = INT64_MIN;
    uint64_t refused = 0;
    *got = 0;
    pcall(N_SPT_REQUEST, 2, s->spe, U(&s->post[s->ring[s->head]]));
    while (!eos_out) {
        int progress = 0;
        if (!eos_in && p_set(ev)) {   // DecodeAsync: clear, then one packet
            p_clear(NULL, ev);
            uint64_t id = 0;
            uint32_t r;
            if (next < last) {
                const PFrame *f = &t->f[next];
                r = (uint32_t)pcall(s->nm->submit, 6, U(&id), s->type, U(data + f->off), (uint64_t)f->size,
                                    (uint64_t)f->pts_ns, (uint64_t)0);
            } else r = (uint32_t)pcall(s->nm->submit, 6, U(&id), s->type, (uint64_t)0, (uint64_t)0,
                                       (uint64_t)t->f[last - 1].pts_ns, (uint64_t)1);
            CHECK(r == 0 || r == CV_BUSY, "Submit packet %d: %#x", next, r);
            if (r == CV_BUSY) refused++;
            else if (next < last) next++;
            else eos_in = 1;
            progress = 1;
        }
        if (p_set(s->spe)) {   // the event thread's callback (+0x4480)
            int k = s->ring[s->head];
            CHECK(pcall(N_TRY_WAIT, 1, U(&s->post[k])) & 1, "sync point event fired, TryWaitNativeSync false (buffer %d)", k);
            uint16_t pw = 0, ph = 0, pcw = 0, pch = 0;
            int64_t ts = 0;
            uint8_t eos = 0;
            uint32_t r = (uint32_t)pcall(s->nm->props, 8, U(&pw), U(&ph), U(&pcw), U(&pch), U(&ts), U(&eos), s->type,
                                         (uint64_t)k);
            CHECK(r == 0, "properties of buffer %d: %#x", k, r);
            s->head = (s->head + 1) % P_FRAMES;
            s->count--;
            if (eos) eos_out = 1;
            else {
                int n = first + *got;
                CHECK(n < last, "more pictures than packets");
                CHECK(pw == w && ph == h && pcw == w && pch == h, "picture %d is %ux%u (crop %ux%u), want %dx%d", n, pw, ph,
                      pcw, pch, w, h);
                CHECK(ts == t->f[n].pts_ns && ts > prev, "picture %d at %lld ns, the container says %lld", n,
                      (long long)ts, (long long)t->f[n].pts_ns);
                prev = ts;
                // ReadBuffer (+0xf810): the whole picture, tight NV12
                r = (uint32_t)pcall(N_READ_NV12, 12, s->pool[k], (uint64_t)0, (uint64_t)0, (uint64_t)0, (uint64_t)w,
                                    (uint64_t)h, U(frame), (uint64_t)w, (uint64_t)w * h, U(frame + (size_t)w * h),
                                    (uint64_t)w, (uint64_t)w * h / 2);
                CHECK(r == 0, "ReadDecoderOutputImageNv12: %#x", r);
                if (n % 60 == 0 || n == mid) {   // a decoded picture, not a blank buffer: luma of the centre band
                    uint64_t sum = 0, sq = 0;
                    for (int y = h / 2 - 8; y < h / 2 + 8; y++)
                        for (int x = 0; x < w; x++) {
                            unsigned v = frame[(size_t)y * w + x];
                            sum += v, sq += v * v;
                        }
                    double cnt = 16.0 * w, mean = sum / cnt;
                    if (mean < *luma_lo) *luma_lo = mean;
                    if (mean > *luma_hi) *luma_hi = mean;
                    if (n == mid) {
                        double var = sq / cnt - mean * mean;
                        printf("probe-codec: picture %d (%.3f s): centre band luma mean %.1f, deviation %.1f\n", n,
                               ts / 1e9, mean, var > 0 ? __builtin_sqrt(var) : 0.0);
                        CHECK(var > 4.0, "picture %d: a flat centre band (deviation %.2f)", n, var);
                        uint8_t *rgba = malloc((size_t)w * h * 4);
                        CHECK(rgba, "RGBA buffer");
                        r = (uint32_t)pcall(N_READ_RGBA, 9, s->pool[k], (uint64_t)0, (uint64_t)0, (uint64_t)0,
                                            (uint64_t)w, (uint64_t)h, U(rgba), (uint64_t)w * 4, (uint64_t)w * h * 4);
                        CHECK(r == 0, "ReadDecoderOutputImageRgba: %#x", r);
                        // The RGBA readback agrees with the NV12 one (BT.709, limited range by hand) at a grid of points.
                        int worst = 0;
                        for (int y = 1; y < h; y += h / 9)
                            for (int x = 1; x < w; x += w / 15) {
                                const uint8_t *uv = frame + (size_t)w * h + (size_t)(y / 2) * w + (x & ~1);
                                double Y = 1.164384 * (frame[(size_t)y * w + x] - 16), u = uv[0] - 128.0, v = uv[1] - 128.0;
                                double ref[3] = {Y + 1.792741 * v, Y - 0.213249 * u - 0.532909 * v, Y + 2.112402 * u};
                                for (int k3 = 0; k3 < 3; k3++) {
                                    int e = ref[k3] < 0 ? 0 : ref[k3] > 255 ? 255 : (int)(ref[k3] + 0.5);
                                    int dlt = abs(e - rgba[((size_t)y * w + x) * 4 + k3]);
                                    if (dlt > worst) worst = dlt;
                                }
                                CHECK(rgba[((size_t)y * w + x) * 4 + 3] == 255, "RGBA alpha at %d,%d", x, y);
                            }
                        printf("probe-codec: RGBA readback of picture %d: largest difference to the NV12 one %d\n", n, worst);
                        CHECK(worst <= 2, "RGBA and NV12 readbacks differ by %d", worst);
                        if (shot) {
                            rt_write_png(shot, rgba, (uint32_t)w, (uint32_t)h);
                            printf("probe-codec: picture %d written to %s\n", n, shot);
                        }
                        free(rgba);
                    }
                }
                ++*got;
            }
            if (p_recycle(s, k)) return 1;
            pcall(N_SPT_CANCEL, 1, s->spe);   // SetFence: cancel, then request the next oldest
            CHECK(!p_set(s->spe), "CancelSyncPointEvent left the event signalled");
            pcall(N_SPT_REQUEST, 2, s->spe, U(&s->post[s->ring[s->head]]));
            progress = 1;
        }
        if (progress) idle = 0;
        else {
            CHECK(++idle < 20000, "no progress for 20 s (%d pictures out, %d of %d packets in)", *got, next - first,
                  last - first);
            plat_sleep_ms(1);
        }
    }
    if (refused) printf("probe-codec: %llu packets refused (3603) and sent again\n", (unsigned long long)refused);
    return 0;
}

int probe_codec(const char *path) {
    uint64_t fsize = 0;
    uint8_t *data = path ? plat_file_read(path, &fsize) : NULL;
    CHECK(data, "cannot read %s", path ? path : "(no file)");
    PTrack t = {0};
    const char *err = pdemux(data, (size_t)fsize, &t);
    CHECK(!err, "WebM: %s", err);
    int codec = !strcmp(t.codec, "V_AV1") ? CV_AV1 : !strcmp(t.codec, "V_VP9") ? CV_VP9 : !strcmp(t.codec, "V_VP8") ? CV_VP8 : -1;
    CHECK(codec >= 0 && t.n > 1 && t.w > 0 && t.h > 0 && !(t.w & 1) && !(t.h & 1), "WebM: codec %s, %d frames, %dx%d",
          t.codec, t.n, t.w, t.h);
    printf("probe-codec: %s: %s %dx%d, %d frames, %.3f s\n", path, t.codec, t.w, t.h, t.n,
           (t.f[t.n - 1].pts_ns - t.f[0].pts_ns) / 1e9);
    static const PNames names_av1 = PNAMES("Av1"), names_vp9 = PNAMES("Vp9"), names_vp8 = PNAMES("Vp8");
    PState s = {.nm = codec == CV_AV1 ? &names_av1 : codec == CV_VP9 ? &names_vp9 : &names_vp8};
    ops = &probe_ops;
    PC = plat_aligned_alloc(sizeof(Cpu), 64);
    CHECK(PC, "Cpu allocation");
    memset(PC, 0, sizeof *PC);
    PC->sp = U(pstack);
    int w = t.w, h = t.h;

    // VideoDecoder::Initialize and Configure
    CHECK((uint32_t)pcall(N_VIDEO_INIT, 0) == 0, "video::Initialize");
    uint8_t pref[0x40];
    memset(pref, 0xcc, sizeof pref);
    PC->x[8] = U(pref);
    pcall(N_PREF, 4, (uint64_t)w | 0xabcd0000ull, (uint64_t)h, (uint64_t)FMT_NV12, (uint64_t)1);   // high bits: garbage
    uint64_t psize = 0, palign = 0;
    CHECK((uint32_t)pcall(N_POOL_SIZE, 4, U(&psize), U(&palign), U(pref), (uint64_t)1) == 0 &&
              psize >= (uint64_t)w * h * 3 / 2 && palign,
          "CalculateDecoderImagePoolBuffer: %llu bytes, alignment %llu", (unsigned long long)psize,
          (unsigned long long)palign);
    _Alignas(16) static uint8_t dtype[0x410];
    memset(dtype, 0, sizeof dtype);
    s.type = U(dtype);
    CHECK((uint32_t)pcall(s.nm->init_rm, 8, s.type, (uint64_t)(4 << 20), (uint64_t)w, (uint64_t)h, (uint64_t)8,
                          (uint64_t)0, (uint64_t)FMT_NV12, (uint64_t)1) == 0, "Initialize...DecoderByRemoteMemory");
    uint64_t ev = pcall(s.nm->event, 1, s.type);
    CHECK(ev == s.type + CV_EVENT_OFF && p_set(ev), "submission-ready event %#llx (signalled %d)", (unsigned long long)ev,
          ev ? p_set(ev) : -1);

    // RegisterVideoFrame: a pool per frame, registered, acquired at once; nothing reached before a picture comes
    static _Alignas(16) uint8_t pools[P_FRAMES][0x20];
    uint8_t *pmem[P_FRAMES];
    for (int k = 0; k < P_FRAMES; k++) {
        CHECK((pmem[k] = plat_aligned_alloc((size_t)psize, (size_t)palign)), "pool memory");
        s.pool[k] = U(pools[k]);
        memset(pools[k], 0, sizeof pools[k]);
        CHECK((uint32_t)pcall(N_POOL_INIT, 5, s.pool[k], U(pmem[k]), psize, U(pref), (uint64_t)1) == 0,
              "InitializeDecoderImagePool %d", k);
        CHECK((uint32_t)pcall(s.nm->reg, 4, s.type, (uint64_t)k, s.pool[k], (uint64_t)0) == 0, "Register buffer %d", k);
        uint32_t idx = 99;
        CvSync sync = {0};
        CHECK((uint32_t)pcall(s.nm->acq, 3, U(&idx), U(&sync), s.type) == 0 && idx == (uint32_t)k,
              "Acquire after Register %d gave %u", k, idx);
        s.post[k] = sync;
        s.ring[(s.head + s.count++) % P_FRAMES] = k;
        CHECK(!(pcall(N_TRY_WAIT, 1, U(&s.post[k])) & 1), "a registered buffer's handle is reached before a picture");
    }
    uint32_t idx = 0;
    CvSync sync;
    CHECK((uint32_t)pcall(s.nm->acq, 3, U(&idx), U(&sync), s.type) == CV_EMPTY, "Acquire with every buffer out");
    CvSync foreign = {0};
    CHECK(pcall(N_TRY_WAIT, 1, U(&foreign)) & 1, "the empty handle reads as reached");
    _Alignas(16) static uint8_t spe[0x80], holder[0x40];
    s.spe = U(spe);
    CHECK((uint32_t)pcall(N_SPT_CREATE, 1, s.spe) == 0 && !p_set(s.spe), "CreateSyncPointEvent");
    pcall(N_SPT_HOLDER, 2, U(holder), s.spe);
    CHECK(p_holder_of == s.spe, "InitializeMultiWaitHolder did not link the event");
    pcall(N_SPT_REQUEST, 2, s.spe, U(&s.post[0]));
    CHECK(!p_set(s.spe), "the sync point event fired before a picture");
    CHECK((uint32_t)pcall(s.nm->start, 1, s.type) == 0, "Start");

    // 1. the whole stream
    uint8_t *frame = malloc((size_t)w * h * 3 / 2);
    CHECK(frame, "frame buffer");
    double lo = 255, hi = 0;
    int got = 0;
    uint64_t t0 = plat_now_ns();
    if (p_run(&s, data, &t, 0, t.n, ev, &got, frame, cfg_get("TOTK_PROBE_CODEC_SHOT"), &lo, &hi)) return 1;
    double secs = (plat_now_ns() - t0) / 1e9;
    CHECK(got == t.n, "%d pictures out of %d packets", got, t.n);
    Dec *d = dec_of(s.type);
    CHECK(d, "decoder lost");
    printf("probe-codec: %d pictures in %.2f s (%.0f per second; the movie runs %.1f s), decode %.2f ms per packet on the "
           "worker (max %.2f), %llu errors\n", got, secs, got / secs, (t.f[t.n - 1].pts_ns - t.f[0].pts_ns) / 1e9,
           d->decode_ns / 1e6 / (double)(d->packets ? d->packets : 1), d->max_ns / 1e6, (unsigned long long)d->errors);
    printf("probe-codec: centre band luma means %.1f to %.1f over the sampled pictures\n", lo, hi);
    CHECK(d->errors == 0, "%llu decode errors", (unsigned long long)d->errors);
    CHECK(hi - lo > 1.0 && hi > 16.5, "the pictures look blank (luma %.1f to %.1f)", lo, hi);

    // 2. Flush, then the start of the stream again
    int again = t.n < 120 ? t.n : 120;
    CHECK((uint32_t)pcall(s.nm->flush, 1, s.type) == 0, "Flush");
    CHECK(p_set(ev), "the event is down after Flush");
    if (p_run(&s, data, &t, 0, again, ev, &got, frame, NULL, &lo, &hi)) return 1;
    CHECK(got == again, "after Flush: %d pictures of %d", got, again);
    printf("probe-codec: after Flush, %d pictures of the first %d packets again\n", got, again);

    // 3. Stop; packets sent without waiting for the event: it goes down at IN_READY waiting, the packet past IN_MAX is
    // refused, Flush drops them and raises it again. An image with no picture reads as black.
    CHECK((uint32_t)pcall(s.nm->stop, 1, s.type) == 0, "Stop");
    for (int k = 0; k < IN_MAX; k++) {
        uint64_t id = 0;
        const PFrame *f = &t.f[k % t.n];
        CHECK((uint32_t)pcall(s.nm->submit, 6, U(&id), s.type, U(data + f->off), (uint64_t)f->size, (uint64_t)f->pts_ns,
                              (uint64_t)0) == 0, "stopped decoder: packet %d refused", k);
        CHECK(p_set(ev) == (k + 1 < IN_READY), "event %d with %d packets waiting", p_set(ev), k + 1);
    }
    uint64_t id = 0;
    CHECK((uint32_t)pcall(s.nm->submit, 6, U(&id), s.type, U(data + t.f[0].off), (uint64_t)t.f[0].size, (uint64_t)0,
                          (uint64_t)0) == CV_BUSY, "packet %d taken", IN_MAX + 1);
    CHECK((uint32_t)pcall(s.nm->flush, 1, s.type) == 0 && p_set(ev) && d->nin == 0, "Flush of a stopped decoder");
    static _Alignas(16) uint8_t blank_pool[0x20];
    CHECK((uint32_t)pcall(N_POOL_INIT, 5, U(blank_pool), U(pmem[0]), psize, U(pref), (uint64_t)1) == 0, "blank pool");
    memset(frame, 0xee, (size_t)w * h * 3 / 2);
    CHECK((uint32_t)pcall(N_READ_NV12, 12, U(blank_pool), (uint64_t)0, (uint64_t)0, (uint64_t)0, (uint64_t)w, (uint64_t)h,
                          U(frame), (uint64_t)w, (uint64_t)w * h, U(frame + (size_t)w * h), (uint64_t)w,
                          (uint64_t)w * h / 2) == 0 && frame[0] == 16 && frame[(size_t)w * h - 1] == 16 &&
              frame[(size_t)w * h] == 128 && frame[(size_t)w * h * 3 / 2 - 1] == 128,
          "an image with no picture does not read as black");
    pcall(N_POOL_FINI, 1, U(blank_pool));
    printf("probe-codec: stopped decoder: event down from %d packets waiting, packet %d refused, Flush emptied it\n",
           IN_READY, IN_MAX + 1);

    // 4. tear-down in the library's order
    pcall(N_SPT_CANCEL, 1, s.spe);
    pcall(N_SPT_DESTROY, 1, s.spe);
    CHECK(((int32_t *)spe)[1] == 2, "DestroySyncPointEvent did not destroy its SystemEvent");
    CHECK((uint32_t)pcall(s.nm->fini, 1, s.type) == 0 && !LD64(s.type), "Finalize");
    CHECK(((int32_t *)(void *)(dtype + CV_EVENT_OFF))[1] == 2, "Finalize did not destroy the submission-ready event");
    CHECK(pcall(N_TRY_WAIT, 1, U(&s.post[s.ring[s.head]])) & 1, "a finalized decoder's handle is not reached");
    for (int k = 0; k < P_FRAMES; k++) {
        pcall(N_POOL_FINI, 1, s.pool[k]);
        CHECK(!LD64(s.pool[k]), "FinalizeDecoderImagePool %d", k);
        plat_aligned_free(pmem[k]);
    }
    CHECK((uint32_t)pcall(N_VIDEO_FINI, 0) == 0, "video::Finalize");
    free(frame);
    free(t.f);
    free(data);
    ops = &guest_ops;
    printf("probe-codec: passed\n");
    return 0;
}
#endif
