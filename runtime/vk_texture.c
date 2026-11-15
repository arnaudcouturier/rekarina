// Vulkan textures of the graphics layer: a VkImage cache keyed by storage
// GPU address plus descriptor, uploads through staging buffers with block-linear deswizzle and ASTC decode (vk_astc.c),
// re-uploads when page write generations (vk_memory.c) show the storage written, ReadTexels / WriteTexels, the texture
// and sampler descriptor slots, and VkSamplers.
//
// Behaviour ported from MacNativeGames games/totk/third_party/d2r_gfx/gfx/nvn @ b75be8cf (MIT, Copyright (c) 2026
// jhaizhou; origin d2r-mobile): which textures mirror their pool memory (nvn_mtl.m tex_mirrored, 3347-3362), the
// "GPU-owned" rule that stops re-uploads once the GPU rendered into a mirrored texture (nvn_mtl.m tex_mtl, 3366-3372),
// the live texture at a storage address that a slot of a finalized texture follows (hle_nvn.c:373-393), and
// ReadTexels / WriteTexels (nvn_mtl.m:10105-10195). Image creation, upload regions and samplers are ported from LAS
// Recomp runtime/nvn/vulkan/textures.c:235-330, 778-844 @ da35b83a (MIT, Copyright (c) 2026 Peiikai).
//
// Every image lives in VK_IMAGE_LAYOUT_GENERAL. Uploads are recorded into the upload batch of vk_memory.c; the replay
// submits it (vk_upload_flush) before a submission that samples the textures.
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>

#include "cfg.h"
#include "gfx.h"
#include "host.h"
#include "nvn_api.h"
#include "nvn_hmap.h"
#include "nvn_tex.h"
#include "tex.h"
#include "vk.h"
#include "vk_dlaa.h"
#include "vk_exec.h"
#include "platform/platform.h"

typedef struct {
    uint64_t addr;
    nvn_texdesc d;
    uint8_t has_layout, layout[8], pad[7];
} TexKey;

typedef struct Tex {
    VkTexImage pub;               // first: vk_texture_get hands out &pub
    TexKey key;
    tex_t t;                      // the newest texture object seen with this key
    TexVkFormat f;
    VmaAllocation alloc;          // own memory; NULL when bound into pool memory
    uint64_t bytes;               // the size of alloc (0 without own memory)
    uint32_t use_round;           // look_round when a lookup last reached it (touch)
    uint64_t use_frame;           // gfx_counters.frames at that lookup
    uint32_t draw_round;          // draw_round when a draw's binding last named it (drawn), or when it was made
    uint64_t draw_frame;          // gfx_counters.frames then
    uint8_t ever_drawn;           // a draw's binding named it at least once: what uses it shows in bindings (evictable)
    VmaAllocation bound_to;       // the pool memory it is bound into
    uint32_t wgen;                // page generation of the last upload check
    uint32_t checked;             // submit_epoch of the last check from a draw binding
    int uploaded, released;
    uint32_t n_up;                // uploads of this entry (vk_texture_debug)
    int last_ch;                  // last vk_mem_changed result of make_fresh: 1 written, 0 clean, -1 not tracked
    struct Tex *next_at;          // other entries at the same storage address
    uint8_t gpu_dirty;            // mirrored, and the GPU wrote the image after the last write-back to the storage
    uint32_t gpu_gen;             // page generation when the GPU write that set gpu_dirty was recorded
    uint64_t gpu_seq;             // order of that GPU write among all of them (gpu_writes)
    uint64_t gpu_fence;           // fence of the submission that made that write (0: not recorded in one)
    uint64_t rb_hold;             // readback: its write-back waits until gfx_counters.frames reaches this
    uint8_t lagged;               // a small target whose bytes reach storage a frame late (frame-latency readbacks)
    uint64_t run_frame;           // mirrored: the frame of the newest GPU write
    uint8_t run;                  // frames in a row up to run_frame with a GPU write, counted up to WB_RUN
    uint64_t wr_fence;            // any entry: newest submission recorded to write its image (render, copy, clear,
                                  // storage binding); ReadTexels waits for it instead of draining the replay queue
    uint8_t listed;               // in owned[]
    uint8_t *snap;                // a texture that fills no page: its storage bytes when the GPU write was recorded
    uint32_t wt_n;                // WriteTexels into the storage since the last upload
    struct Tex *rel_prev, *rel_next;   // while released: the list of released entries, oldest first
    uint64_t rel_frame;           // while released: the frame it was released in (sweep_pressure's age rule)
    uint64_t wr_serial;           // order of the newest GPU write recorded into its image among all entries (0: none)
    uint64_t wr_old[4];           // earlier values of wr_serial, newest first ("lookups as of a place" below)
    VkImageCreateFlags ci_flags;  // the image's creation flags (recycle_take)
    uint64_t chash;               // has_chash: content_hash of the storage bytes its image was first filled from
    uint64_t cprint;              // has_chash: content_print of those bytes (adopt_released, "moved storage")
    uint64_t texq_base[3];        // "texture quality": the storage addresses of the textures this smaller one stands for (0: none)
    uint8_t has_chash;            // cleared by a later upload (adopt_released)
    uint8_t *edge;                // its bytes in the storage's first and last page when it was last uploaded (edge_keep)
} Tex;

static void destroy_tex(Tex *e);
static void edge_keep(Tex *e);
static VkImage recycle_take(const VkImageCreateInfo *ci, VmaAllocation *out);
struct Tex;
static int released_in_use(const struct Tex *e, uint64_t now);   // above sweep_released
static int adopt_released(Tex *e, const tex_t *t);
static int park_image(Tex *e);
static void park_clear(void);

// A slot keeps what vk_texture_by_id resolved for it, so a draw binding skips the key lookup and the address chain: the
// entry (ce), valid while the generation of its address (agen) is still cgen, the view and has_view handed out with it,
// and the submit_epoch of the last freshness check through the slot (cepoch). Hot fields first: a lookup that hits
// touches one or two cache lines.
typedef struct {
    struct Tex *ce;               // NULL: nothing cached (registered since, or never resolved)
    uint32_t cgen;                // agen of the slot's address when ce was resolved
    uint32_t cepoch;
    uint32_t depoch;              // submit_epoch of the last lookup through the slot that a draw made (drawn)
    view_t cview;
    uint8_t chas_view, used, has_view;
    view_t v;
    tex_t t;
} TexSlot;
typedef struct { samp_par p; VkSampler s, sp, sc; uint8_t used; } SampSlot;   // s: vk_sampler_for(&p) once looked up; sp:
                                                                           // its point-sampled form (vk_sampler_point_by_id_locked)

static PlatRMutex tex_lock;
static PlatOnce tex_once = PLAT_ONCE_INIT;
static nvn_hmap by_key = {.klen = sizeof(TexKey)};
static nvn_hmap by_addr = {.klen = 8};
static TexSlot *tslots;
static int ntslots;
static SampSlot *sslots;
static int nsslots;
static nvn_hmap samplers = {.klen = sizeof(samp_par)};
static int nreleased;
static Tex *rel_head, *rel_tail;   // released entries in release order (sweep_released drops from the head)
static uint64_t n_uploads, n_reuploads;
// Re-uploads since the last perf line (tex_lock): all, and those of textures a WriteTexels wrote since their last upload.
static uint64_t ru_n, ru_bytes, ru_ns, ru_wt_n, ru_wt_bytes, ru_wt_ns;
// REKARINA_TEX_ALIAS (default on; =0: a new image over live storage starts empty): a new descriptor over the storage of a
// live image that the GPU wrote gets that image's content (alias_copy). Counts since the last log line.
static int tex_alias = -1;
static uint64_t n_alias_copied, n_alias_upload, n_alias_uninit;
static uint32_t submit_epoch = 1;   // bumped per queue submit: draw bindings check a texture's storage once per submit
// Mirrored textures the GPU wrote into, sorted by address (vk_texture_cpu_stale, vk_texture_write_back). Changed under
// tex_lock and owned_lock; read under either.
static PlatMutex owned_lock = PLAT_MUTEX_INIT;
static Tex **owned;
static int nowned, cowned;
static uint64_t max_owned;   // largest storage size in owned[]
static uint64_t gpu_writes;  // GPU writes into mirrored textures so far: Tex.gpu_seq
static uint64_t wr_serials;  // GPU writes recorded into any entry so far: Tex.wr_serial (record order)
static void note_writer(struct Tex *e);
// Bumped whenever the entry a slot resolves to can change: an entry created or destroyed, released or taken back into
// use (the slot lookup and the follow of a finalized slot read only by_key, by_addr and the released flags).
static uint32_t tex_gen = 1;
// Bumped with tex_gen and whenever a slot is registered: what vk_texture_by_id returns for any slot may have changed.
static uint32_t slots_gen = 1;
static uint32_t samp_gen = 1;   // bumped whenever a sampler slot is registered: vk_sampler_by_id may answer otherwise
static uint64_t n_checks;    // draw-binding storage checks (make_fresh_snap) since the last perf line
// tex_gen per storage address (hashed): a slot resolves through the entries at its own address only (slot_resolve), so
// a slot's resolution stays valid while its address's generation does. TotK finalizes and initializes transient
// textures every frame: about 90 tex_gen and 160 slots_gen bumps per frame in the monsters fight, one every 16 draws,
// which emptied every cache keyed on the global generations.
#define AGEN_N 4096
static uint32_t agen[AGEN_N];
// Per slot id, for vk_texture_slot_token without the lock: the registration serial (bits 16-47; 0: never registered)
// and the address generation index (bits 0-15), one word so a reader gets a consistent pair. Fixed-size (slot ids are
// 20 bits), unlike tslots, which moves when it grows.
static uint64_t *sinfo;
static uint32_t reg_serial;

static uint32_t agen_idx(uint64_t addr) { return (uint32_t)(addr * 0x9E3779B97F4A7C15ull >> 52); }

// ---- lookups as of a place in the command stream (REKARINA_CHUNK_ASOF=0 turns it off)
// A slot whose own texture is released, or was never rendered, follows the entry at its storage that the GPU wrote
// last (slot_resolve, "content follows the memory"), and a new image over live storage takes that entry's content
// (entry_of). "Last" is the order in which the replay thread records writes. The threads that record the chunks of a
// draw stretch (vk_exec.c) look textures up while the replay thread is already recording later passes: a chunk then
// found a target that a later pass renders at the same storage, an image the GPU has not written at the chunk's place
// in the queue, and the objects it drew read nothing: black for as long as the game kept that layout of its transient
// targets (the forest while the camera turns, after a frame in which every transient target moves: 1 to 19 dark
// frames, 10 of 49 such frames; none with the stretches off).
// A stretch notes the write count when it is handed out (vk_texture_asof_mark) and its chunks look up under that limit
// (vk_texture_asof): writes recorded later do not count. An entry keeps the serials of its earlier writes that a
// stretch still out may need (wr_old; those not above asof_floor, the newest stretch's mark), so an entry written
// before the stretch and again after it still counts with its earlier write. What a limited lookup resolves is not
// kept in the slot (the replay thread is past it), and the caller keeps it in no memo (vk_texture_asof_clean).
static uint64_t asof_floor;            // wr_serials when the newest draw stretch was handed out
static _Thread_local uint64_t asof;    // this thread's lookups count GPU writes up to this serial; 0: all of them
static uint64_t awr[AGEN_N];           // per address bucket (agen_idx): the serial of the newest GPU write there
static uint64_t n_asof;                // lookups resolved under a limit so far

// p's newest GPU write as this thread's lookups count it: under a limit, the newest one not after it. 0: none.
static inline uint64_t wr_at(const Tex *p) {
    if (__builtin_expect(!asof, 1) || p->wr_serial <= asof) return p->wr_serial;
    for (int i = 0; i < 4; i++)
        if (p->wr_old[i] <= asof) return p->wr_old[i];
    return p->wr_serial;   // written more often since the limit than is kept: as without one
}

static void tex_gen_bump(uint64_t addr) {
    if (!++tex_gen) tex_gen = 1;
    slots_gen++;
    uint32_t *g = &agen[agen_idx(addr)];
    __atomic_store_n(g, *g + 1, __ATOMIC_RELEASE);
}
// The same for an entry that went, was released or was taken back. A slot's kept resolution and its token stand on the
// generation of the slot's own storage address. Under "texture quality" the slot resolves to a smaller texture whose
// storage starts at a later level, another address: when that entry went, nothing told the slot, which kept a pointer
// to the dead record, and the pool update made a view of its destroyed image (the driver never returned from
// vkCreateImageView: quarter quality hung at the entrance of the Market, 2026-11-13). Such an entry carries the
// slot's address and both generations move.
static void tex_gen_bump_entry(uint64_t addr, const uint64_t texq_base[3]) {
    tex_gen_bump(addr);
    for (int i = 0; i < 3; i++)
        if (texq_base[i]) tex_gen_bump(texq_base[i]);
}

static volatile int32_t tex_ready;
static void tex_init(void *p) {
    (void)p;
    __atomic_store_n(&tex_ready, 1, __ATOMIC_RELEASE);
    return;
}
static void lock(void) {
    if (!__atomic_load_n(&tex_ready, __ATOMIC_ACQUIRE)) plat_once(&tex_once, tex_init, NULL);
    plat_rmutex_lock(&tex_lock);
}
static void unlock(void) { plat_rmutex_unlock(&tex_lock); }

// ---- uploads ahead of first use (REKARINA_TEX_PREP; "up_main" below)
// up_worker: this thread is the one that uploads ahead. Inside upload() it fills staging memory of its own with the
// texture lock released (fill_lock held instead: vk_texture_range_gone waits on it before a pool's memory goes), so
// the replay thread is not held up by the copy.
static _Thread_local int up_worker;
// The worker hashed this storage with the texture lock released (up_main): adopt_released takes the values.
static _Thread_local struct { uint64_t addr, size, cprint, chash; int valid; } up_pre;
static PlatMutex fill_lock = PLAT_MUTEX_INIT;
typedef struct { VkBuffer buf; VmaAllocation alloc; } OwnedStaging;
static void owned_staging_free(void *arg) {
    OwnedStaging *o = arg;
    VkCtx *c = vk_ctx();
    if (c) vmaDestroyBuffer(c->vma, o->buf, o->alloc);
    free(o);
}
static void owned_staging_later(VkBuffer buf, VmaAllocation alloc) {   // once the GPU is past the batch that copies from it
    OwnedStaging *o = malloc(sizeof *o);
    if (!o) rt_fatal("vk: out of memory");
    o->buf = buf, o->alloc = alloc;
    vk_defer_call(owned_staging_free, o);
}
// The worker's fill of staging memory from an entry's storage, with the texture lock released.
static void fill_enter(void) {
    plat_mutex_lock(&fill_lock);
    unlock();
}
static void up_range_gone(void);
static void up_queue(int id, uint32_t serial);

static uint32_t umax(uint32_t a, uint32_t b) { return a > b ? a : b; }
static uint32_t mip(uint32_t v, unsigned lv) { return umax(v >> lv, 1); }

static void make_key(const tex_t *t, TexKey *k) {
    memset(k, 0, sizeof *k);
    k->addr = (uintptr_t)t->mem;
    k->d = t->d;
    k->has_layout = t->has_layout;
    memcpy(k->layout, t->layout, 8);
}

static int texq_mode;                                   // "texture quality", above slot_resolve
static int texq_shape(const tex_t *t, tex_t *out);

// Keys equal apart from the descriptor's swizzle selectors: the same storage, format and geometry.
static int same_but_swizzle(const TexKey *a, const TexKey *b) {
    TexKey x = *a, y = *b;
    x.d.swizzle_r = x.d.swizzle_g = x.d.swizzle_b = x.d.swizzle_a = 0;
    y.d.swizzle_r = y.d.swizzle_g = y.d.swizzle_b = y.d.swizzle_a = 0;
    return !memcmp(&x, &y, sizeof x);
}

// MNG nvn_mtl.m tex_mirrored: textures whose content the CPU provides in pool memory. Display, compressible, zcull,
// depth, multisample and GPU-only storage stays GPU-owned.
static int tex_mirrored(const tex_t *t) {
    if (!t->mem || nvn_gpu_only((uintptr_t)t->mem)) return 0;
    if (t->has_layout) return 1;
    if (!(t->pool_flags & (NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_CPU_CACHED))) return 0;
    if (t->d.flags & (NVN_TEXTURE_FLAGS_DISPLAY | NVN_TEXTURE_FLAGS_COMPRESSIBLE | NVN_TEXTURE_FLAGS_ADAPTIVE_ZCULL |
                      NVN_TEXTURE_FLAGS_DEPTH16_PREFER_FAST_CLEAR | NVN_TEXTURE_FLAGS_ZCULL_SUPPORT_STENCIL))
        return 0;
    if (t->d.samples > 1 || t->d.target == NVN_TEXTURE_TARGET_BUFFER) return 0;
    if (t->d.format >= 0x31 && t->d.format <= 0x36) return 0;   // stencil / depth formats
    return 1;
}

static void log_limited(volatile int32_t *n, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void log_limited(volatile int32_t *n, const char *fmt, ...) {
    if (plat_inc32(n) > 16) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    rt_log("vk: %s", buf);
}

static void barrier(VkCommandBuffer cb, VkPipelineStageFlags2 src, VkAccessFlags2 sa, VkPipelineStageFlags2 dst,
                    VkAccessFlags2 da) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = src, .srcAccessMask = sa,
                           .dstStageMask = dst, .dstAccessMask = da};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
}
#define ALL_RW VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT

// ---------------------------------------------------------------- images
static VkFormatFeatureFlags features_of(VkFormat f) {
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(vk_ctx()->phys, f, &p);
    return p.optimalTilingFeatures;
}

#ifdef GAME_NS2
VkFormat vk_storage_format(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8_SRGB: return VK_FORMAT_R8_UNORM;
    case VK_FORMAT_R8G8_SRGB: return VK_FORMAT_R8G8_UNORM;
    case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32: return VK_FORMAT_A8B8G8R8_UNORM_PACK32;
    default: return f;
    }
}
#endif

static VkImageAspectFlags aspect_of(const TexVkFormat *f) {
    VkImageAspectFlags a = 0;
    if (f->depth) a |= VK_IMAGE_ASPECT_DEPTH_BIT;
    if (f->stencil) a |= VK_IMAGE_ASPECT_STENCIL_BIT;
    return a ? a : VK_IMAGE_ASPECT_COLOR_BIT;
}

// Binds img into the pool memory behind its GPU address (physical pools, virtual pools through the page table) when
// the memory type, alignment and contiguous size allow; 0 otherwise.
// A depth / stencil image that needs more bytes than its NVN storage (guest_size) is not bound: AMD's RDNA2 driver
// (26.10.1) puts the depth compression metadata after the depth data, and above 1080p that tail lies over the next
// texture of the game's heap; each depth write then scrambled it (garbage band at the top, pink glow, blocks in the
// grass, a blue checkered scene in the menu). Colour images keep their place: the RX 9070 also sizes some of them
// above their storage, and nothing there showed that the GPU writes the excess.
static int format_compressed(VkFormat f) {   // vkCmdClearColorImage takes no block-compressed format
    return (f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK) ||
           (f >= VK_FORMAT_ASTC_4x4_SFLOAT_BLOCK && f <= VK_FORMAT_ASTC_12x12_SFLOAT_BLOCK);
}

static int suballoc_on(void) {   // REKARINA_TEX_SUBALLOC=0: every texture lets the driver choose dedicated memory
    static int on = -1;
    if (on < 0) {
        const char *v = cfg_env("REKARINA_TEX_SUBALLOC");
        on = !(v && v[0] == '0');
    }
    return on;
}

// Memory sub-allocated for a texture holds what the texture freed from it last held; dedicated memory comes zeroed. Without
// this the first frames after a load drew doors and cracks of white light: bright leftovers read where the old dedicated
// allocations gave zero. A buffer aliasing the allocation clears it before the upload batch writes the image.
static void zero_suballoc(VkCtx *c, const Tex *e) {
    VmaAllocationInfo info;
    vmaGetAllocationInfo(c->vma, e->alloc, &info);
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = info.size,
                             .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer b;
    if (vkCreateBuffer(c->device, &bi, NULL, &b) != VK_SUCCESS) return;
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(c->device, b, &mr);
    if (!(mr.memoryTypeBits >> info.memoryType & 1) || info.offset % mr.alignment ||
        vmaBindBufferMemory(c->vma, e->alloc, b) != VK_SUCCESS) {
        vkDestroyBuffer(c->device, b, NULL);
        return;
    }
    VkCommandBuffer cb = vk_upload_begin();
    vkCmdFillBuffer(cb, b, 0, VK_WHOLE_SIZE, 0);
    barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, ALL_RW);
    vk_upload_end();
    vk_defer_buffer(b, NULL);
}

// REKARINA_SECTIONS: what asked for the entry being made (texture lock): 0 a render target or vk_texture_get, 1 a draw's
// binding, 2 the texture pool's update. The calls of one creation, also for the TEXSLOW line.
static int sec_mk;
enum { ICS_CREATE, ICS_REQ, ICS_RESOLVE, ICS_BIND, ICS_VMA, ICS_N };
static uint64_t ics[ICS_N];
static inline void ic_end(int split, int kind, uint64_t t0) {
    if (t0) {
        uint64_t d = sec_clock() - t0;
        ics[split] += d;
        sec_add(kind, d);
    }
}
static _Thread_local int pm_thread;   // the thread that makes images ahead (pm_main): no section timers, no log lines
static VkResult timed_create_image(VkCtx *c, const VkImageCreateInfo *ci, VkImage *img) {
    uint64_t t = pm_thread ? 0 : sec_begin();
    VkResult r = vkCreateImage(c->device, ci, NULL, img);
    ic_end(ICS_CREATE, SEC_IC_CREATE_T + sec_mk, t);
    if (r == VK_SUCCESS) vk_live_check_born((uint64_t)(uintptr_t)*img);   // DEBUG-LIVECHECK
    return r;
}

// The texture keys destroyed so far (hash, low bits index, later ones overwrite): how many new images repeat an address and
// shape that was destroyed earlier.
enum { REP_SLOTS = 1 << 18 };
static uint64_t *rep_keys;
static uint64_t rep_hash(const TexKey *k) {
    const uint8_t *p = (const uint8_t *)k;
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < sizeof *k; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h | 1;
}

// What this thread's last bind_pool_memory resolved the image's address to, before the twin's shift: an image made
// ahead (pm_take) is used only while its address still resolves to that.
static _Thread_local struct { VmaAllocation alloc; VkDeviceSize at, size; } bind_res;

static int bind_pool_memory(VkCtx *c, Tex *e, VkImage img, uint64_t guest_size) {
    VkMemoryRequirements req;
    uint64_t sec1 = pm_thread ? 0 : sec_begin();
    vkGetImageMemoryRequirements(c->device, img, &req);
    ic_end(ICS_REQ, SEC_IC_REQ, sec1);
    static uint64_t why[5], why_b[5];   // DEBUG-ALIASWHY: bound, depth larger than its guest size, not resolved, misaligned, type
    static uint64_t why_n;
#define ALIAS_WHY(k) do { why[k]++, why_b[k] += req.size; if (!(++why_n & 1023)) rt_log("ALIASWHY bound %llu (%llu MiB), depth too large %llu (%llu MiB), unresolved %llu, misaligned %llu (%llu MiB), memory type %llu; this one: size %llu guest %llu align %llu", (unsigned long long)why[0], (unsigned long long)(why_b[0] >> 20), (unsigned long long)why[1], (unsigned long long)(why_b[1] >> 20), (unsigned long long)why[2], (unsigned long long)why[3], (unsigned long long)(why_b[3] >> 20), (unsigned long long)why[4], (unsigned long long)req.size, (unsigned long long)guest_size, (unsigned long long)req.alignment); } while (0)
    if ((e->f.depth || e->f.stencil) && req.size > guest_size) { ALIAS_WHY(1); return 0; }
    {   // DEBUG-DEPTHALIAS: REKARINA_DEPTH_ALIAS=0: a depth image never aliases pool memory (memory of its own)
        static int da = -1;
        if (da < 0) da = rt_knob("DEPTH_ALIAS", "1");
        if (!da && (e->f.depth || e->f.stencil)) { ALIAS_WHY(1); return 0; }
    }
    VkMemSpan sp;
    if (!req.size) return 0;   // an image the device could not lay out (image_size)
    sec1 = pm_thread ? 0 : sec_begin();
    int resolved = vk_mem_resolve(e->pub.addr, req.size, &sp);
    ic_end(ICS_RESOLVE, SEC_IC_RESOLVE, sec1);
    if (!resolved || !sp.alloc) { ALIAS_WHY(2); return 0; }
    VkDeviceSize at = sp.alloc_offset;
    bind_res.alloc = sp.alloc, bind_res.at = at, bind_res.size = req.size;
    if (at % req.alignment) {
        // an offset the pool's block cannot take: its twin takes it VK_MEM_TWIN_SHIFT bytes lower (REKARINA_POOL_TWIN=0: memory
        // of its own, as before)
        static int twin = -1;
        // REKARINA_POOL_TWIN: 0 never, 1 always, unset (OoT): while video memory is over 80 % of its budget.
        // Measured in the village: the same frame rate either way (72.3 against 72.7 fps, 63.0 against 62.5 with frame
        // generation), 0.35-0.5 GiB less video memory with it, and an image bound into pool memory costs the replay
        // ~0.3 ms where a recycled image of its own costs nothing, so it is used only when memory is short.
        if (twin < 0) {
            const char *v = cfg_get("REKARINA_POOL_TWIN");
#ifdef GAME_OOT
            twin = v && *v ? atoi(v) : 2;
#else
            twin = v && *v ? atoi(v) : 0;
#endif
        }
        int use = twin == 1;
        if (twin == 2) {
            uint64_t used = 0, budget = 0;
            int exact = 0;
            use = vk_mem_vram(&used, &budget, &exact) && budget && used > budget / 5 * 4;
        }
        VmaAllocation tw = use && at % req.alignment == VK_MEM_TWIN_SHIFT ? vk_mem_phys_twin(sp.alloc) : NULL;
        if (!tw) { ALIAS_WHY(3); return 0; }
        sp.alloc = tw, at -= VK_MEM_TWIN_SHIFT;
    }
    VmaAllocationInfo info;
    vmaGetAllocationInfo(c->vma, sp.alloc, &info);
    if (at + req.size > info.size) { ALIAS_WHY(2); return 0; }
    if (!(req.memoryTypeBits >> info.memoryType & 1)) { ALIAS_WHY(4); return 0; }
    ALIAS_WHY(0);
    sec1 = pm_thread ? 0 : sec_begin();
    uint64_t hb0 = plat_now_ns();   // DEBUG-HRTIME
    VkResult bound = vmaBindImageMemory2(c->vma, sp.alloc, at, img, NULL);
    if (plat_now_ns() - hb0 > 3000000)
        rt_log("HRTIME bind %ux%u fmt %d size %llu KiB onto alloc of %llu MiB: %.2f ms at %.3f s", e->pub.width, e->pub.height,
               (int)e->pub.format, (unsigned long long)(req.size >> 10), (unsigned long long)(info.size >> 20),
               (plat_now_ns() - hb0) / 1e6, hb0 / 1e9);
    ic_end(ICS_BIND, SEC_IC_BIND_ALIAS_T + sec_mk, sec1);
    if (bound != VK_SUCCESS) return 0;
    e->bound_to = sp.alloc;
    return 1;
}

// agl's BC4 encoder draws a BC texture's blocks through an RGBA16UI view, one texel per 8-byte block (refs/nxnative
// docs/agents/04-graphics.md, BotW and TotK sections). Vulkan does it with an uncompressed
// view of a BLOCK_TEXEL_VIEW_COMPATIBLE image; EXTENDED_USAGE lets the BC image carry the COLOR_ATTACHMENT usage that
// only such views support. Whether the device takes that combination for this format and shape:
static int block_views_ok(const VkImageCreateInfo *ci) {
    VkPhysicalDeviceImageFormatInfo2 fi = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
                                           .format = ci->format, .type = ci->imageType, .tiling = ci->tiling,
                                           .usage = ci->usage, .flags = ci->flags};
    VkImageFormatProperties2 p = {.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
    if (vkGetPhysicalDeviceImageFormatProperties2(vk_ctx()->phys, &fi, &p) != VK_SUCCESS) return 0;
    const VkImageFormatProperties *q = &p.imageFormatProperties;
    return ci->extent.width <= q->maxExtent.width && ci->extent.height <= q->maxExtent.height &&
           ci->extent.depth <= q->maxExtent.depth && ci->mipLevels <= q->maxMipLevels &&
           ci->arrayLayers <= q->maxArrayLayers;
}

// The memory the device wants for an image made from ci: 0 when it cannot lay it out. RADV on GFX9 (Vega, Renoir)
// hands back a 0-byte image for a mipmapped block-compressed 3D image at least as deep as it is wide in blocks, with
// COLOR_ATTACHMENT usage and no STORAGE: that usage asks for a thin 3D layout (Mesa radv_image.c), for which the GFX9
// address library has no swizzle mode left (gfx9addrlib.cpp), and the error is dropped; VMA then refuses the 0-byte
// allocation. The format properties call it supported, so only the size shows it.
static VkDeviceSize image_size(const VkImageCreateInfo *ci) {
    VkDeviceImageMemoryRequirements q = {.sType = VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS, .pCreateInfo = ci};
    VkMemoryRequirements2 r = {.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
    vkGetDeviceImageMemoryRequirements(vk_ctx()->device, &q, &r);
    return r.memoryRequirements.size;
}

// UNDEFINED -> GENERAL; new depth / stencil images start cleared (LAS textures.c: the game copies depth
// without stencil, and the stencil of those images must not be whatever the memory held).
// Not a depth image bound into pool memory (REKARINA_DEPTH_NEW_CLEAR=1: as before): the clear goes into the upload batch,
// which runs ahead of the whole part being recorded, and it writes the pool's bytes, which other render targets of the
// frame share. OoT's transient targets reuse their storage within a frame, and a character's 384x384 shadow map
// gets a new address whenever the frame's set of targets changes (every few frames while the player runs): the new
// image's clear then zeroed the storage of a target the same part still read from, drawn before the map in the
// game's order. Part of the picture and the characters in it went dark for one frame, 5 to 17 times in 2,000 frames
// of running through the Market, where video memory is short and every transient target lies in pool memory
// (REKARINA_POOL_TWIN); none after. The console clears nothing there either: the game clears a target before it draws.
static void init_image(Tex *e, VkCommandBuffer cb) {
    VkImageMemoryBarrier2 ib = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
                                .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .image = e->pub.image, .subresourceRange = {e->pub.aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &ib};
    vkCmdPipelineBarrier2(cb, &di);
    static int bound_clear = -1;
    if (bound_clear < 0) bound_clear = cfg_env("REKARINA_DEPTH_NEW_CLEAR") != NULL;   // a test switch, not a setting
    if ((e->f.depth || e->f.stencil) && (!e->bound_to || e->f.stencil || bound_clear)) {
        VkClearDepthStencilValue zero = {0.0f, 0};
        VkImageSubresourceRange rg = {e->pub.aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
        vkCmdClearDepthStencilImage(cb, e->pub.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &rg);
        barrier(cb, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, ALL_RW);
    } else if (vk_fix_zero() && !e->bound_to && !e->pub.mirrored && !format_compressed(e->f.vk)) {
        // GPU-only storage that got memory of its own starts at zero, as the pool it stands for (vk_memory.c
        // alloc_physical). Images aliasing pool memory are not cleared: that would erase what other textures rendered
        // there; mirrored images are uploaded from their storage on first use.
        VkClearColorValue zero = {0};
        VkImageSubresourceRange rg = {e->pub.aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
        vkCmdClearColorImage(cb, e->pub.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &rg);
        barrier(cb, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, ALL_RW);
    }
}

// REKARINA_SHADOW_SCALE (1 to 4): the game's shadow maps are made this many times larger on each side. TotK renders its
// sun cascades into a 960x960 DEPTH16 2D array of 4 layers and its static shadows into 1440x1440 and 720x720 arrays of 2,
// whatever the output resolution, and shrinks the cascades under load (872, 848, 800, 480 and so on). Those are made
// larger here: render passes into them scale the game's viewports and scissors (vk_exec.c resolve_rt, pass_scale),
// copies scale their regions (vk_exec_xfer.c sub_resolve), and shaders sample them with normalised coordinates, so the
// game sees nothing of it. A map whose storage the CPU writes (mirrored) or that aliases pool memory keeps its size,
// as its texels must match the guest layout.
static int shadow_scale(void) {
    static int scale = -1;
    if (scale < 0) {
        int s = cfg_int("REKARINA_SHADOW_SCALE");
        s = s < 1 ? 1 : s > 4 ? 4 : s;
        rt_log("vk: shadow map scale %d", s);
        scale = s;
    }
    return scale;
}

static int shadow_scale_of(const nvn_texdesc *d, const TexVkFormat *f, uint32_t samples, int mirrored) {
    int scale = shadow_scale();
    if (scale == 1 || mirrored || samples > 1 || d->format != NVN_FORMAT_DEPTH16 || !f->depth) return 1;
    if (d->target != NVN_TEXTURE_TARGET_2D_ARRAY || d->width != d->height || d->width < 256 || d->depth < 2 || d->depth > 4)
        return 1;
    uint32_t most = vk_ctx()->props.limits.maxImageDimension2D;
    return d->width * (uint32_t)scale <= most ? scale : 1;
}

// ---- Images made ahead (REKARINA_TEX_PREMAKE, on for OoT)
// Binding a new image into pool memory costs ~0.3 ms in the driver whatever its size (the kernel reserves and maps GPU
// address space), and OoT gives its transient render targets a new address every frame: 5-15 such images
// a frame in forest village, 1.5-2 ms of the replay thread's 12 ms. The game initializes those textures on its own
// threads well before the replay reaches their first use, so a thread of its own makes the image then (pm_main:
// create_tex up to the bind, nothing else), and create_tex takes the bound image from the table (pm_take) where it
// would create and bind one. What it takes is exactly what it would have made: the same create info, bound where the
// address still resolves to. An entry nothing took within PM_KEEP frames is destroyed; a pool that goes takes its
// entries with it before its memory is freed (pm_range_gone).
// Locks: pm_lock is taken with or without the texture lock held, and nothing is locked while holding it.
#define PM_N 256
#define PM_KEEP 12
enum { PM_FREE, PM_QUEUED, PM_BUSY, PM_READY };
typedef struct {
    volatile int32_t state;
    tex_t t;                    // the texture object when it was initialized
    uint64_t addr, seq, frame;
    VkImageCreateInfo ci;       // PM_READY: what img was created from
    VkImage img;
    VmaAllocation bound_to, res_alloc;
    VkDeviceSize res_at, res_size;
} Premade;
static Premade pm[PM_N];
static PlatMutex pm_lock = PLAT_MUTEX_INIT;
static PlatEvent pm_ev;
static volatile int32_t pm_started;
static uint64_t pm_seq;
static _Thread_local Premade *pm_out;   // pm_main: the entry create_tex fills
static struct { uint64_t asked, made, failed, taken, waited, stolen, stale, full, moved; } PMS;

static int pm_on(void) {
    static int on = -1;
    if (on < 0) on = rt_knob("TEX_PREMAKE",
#ifdef GAME_OOT
                             "1"
#else
                             "0"
#endif
                             );
    return on;
}

static Tex *create_tex(const tex_t *t, const TexKey *k, int defer_init);

static void pm_main(void *arg) {
    (void)arg;
    pm_thread = 1;
    for (;;) {
        Premade *p = NULL;
        plat_mutex_lock(&pm_lock);
        for (int i = 0; i < PM_N; i++)
            if (pm[i].state == PM_QUEUED && (!p || pm[i].seq < p->seq)) p = &pm[i];
        if (p) __atomic_store_n(&p->state, PM_BUSY, __ATOMIC_RELEASE);
        plat_mutex_unlock(&pm_lock);
        if (!p) {
            plat_event_wait(&pm_ev);
            continue;
        }
        TexKey k;
        make_key(&p->t, &k);
        p->img = VK_NULL_HANDLE;
        pm_out = p;
        create_tex(&p->t, &k, 1);   // on this thread: only the image and its bind, left in *pm_out
        pm_out = NULL;
        plat_mutex_lock(&pm_lock);
        if (p->img) PMS.made++; else PMS.failed++;
        p->frame = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
        __atomic_store_n(&p->state, p->img ? PM_READY : PM_FREE, __ATOMIC_RELEASE);
        plat_mutex_unlock(&pm_lock);
    }
}

// nvnTextureInitialize (any thread): a texture in device-only pool memory will be bound into that memory at its first
// use; queued so that the image is there by then.
void vk_texture_premake(const tex_t *t) {
    if (!t->mem || !nvn_gpu_only((uintptr_t)t->mem) || t->d.target == NVN_TEXTURE_TARGET_BUFFER || !pm_on() || !vk_ctx()) return;
    uint64_t addr = (uintptr_t)t->mem, now = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    plat_mutex_lock(&pm_lock);
    if (!pm_started) {
        plat_event_init(&pm_ev, 0, 0);
        if (!plat_thread_spawn(pm_main, NULL, "vk images ahead", 0)) {
            plat_mutex_unlock(&pm_lock);
            return;
        }
        __atomic_store_n(&pm_started, 1, __ATOMIC_RELEASE);
        rt_log("vk: images in pool memory are made ahead of their first use by a thread of their own (REKARINA_TEX_PREMAKE=1)");
    }
    PMS.asked++;
    Premade *slot = NULL;
    for (int i = 0; i < PM_N; i++) {
        Premade *q = &pm[i];
        if (q->state == PM_FREE) {
            if (!slot) slot = q;
        } else if (q->addr == addr && !memcmp(&q->t.d, &t->d, sizeof t->d) && q->t.L.size == t->L.size) {
            q->frame = now;   // the same texture at the same place again: the entry serves it
            slot = NULL;
            goto out;
        }
    }
    if (!slot) {
        PMS.full++;
        goto out;
    }
    slot->t = *t, slot->addr = addr, slot->seq = ++pm_seq, slot->frame = now;
    __atomic_store_n(&slot->state, PM_QUEUED, __ATOMIC_RELEASE);
out:
    plat_mutex_unlock(&pm_lock);
    if (slot) plat_event_set(&pm_ev);
}

static int pm_ci_same(const VkImageCreateInfo *a, const VkImageCreateInfo *b) {
    return a->flags == b->flags && a->imageType == b->imageType && a->format == b->format && a->extent.width == b->extent.width &&
           a->extent.height == b->extent.height && a->extent.depth == b->extent.depth && a->mipLevels == b->mipLevels &&
           a->arrayLayers == b->arrayLayers && a->samples == b->samples && a->usage == b->usage;
}

// The image made ahead for e's address from create info ci, bound (e->bound_to set); VK_NULL_HANDLE when there is none.
// An entry still queued is dropped (the caller makes the image now); one being made is waited for.
static VkImage pm_take(VkCtx *c, Tex *e, const VkImageCreateInfo *ci) {
    if (!__atomic_load_n(&pm_started, __ATOMIC_ACQUIRE)) return VK_NULL_HANDLE;
    uint64_t addr = e->pub.addr;
    for (int spins = 0;; spins++) {
        Premade got = {0};
        int busy = 0;
        plat_mutex_lock(&pm_lock);
        for (int i = 0; i < PM_N && !got.img; i++) {
            Premade *q = &pm[i];
            if (q->state == PM_FREE || q->addr != addr) continue;
            if (q->state == PM_QUEUED) {
                __atomic_store_n(&q->state, PM_FREE, __ATOMIC_RELEASE);
                PMS.stolen++;
            } else if (q->state == PM_BUSY) {
                busy = 1;
            } else if (pm_ci_same(&q->ci, ci)) {
                got = *q;
                __atomic_store_n(&q->state, PM_FREE, __ATOMIC_RELEASE);
            }
        }
        if (busy && !got.img && !spins) PMS.waited++;
        plat_mutex_unlock(&pm_lock);
        if (got.img) {
            VkMemSpan sp;
            if (vk_mem_resolve(addr, got.res_size, &sp) && sp.alloc == got.res_alloc && sp.alloc_offset == got.res_at) {
                e->bound_to = got.bound_to;
                PMS.taken++;
                return got.img;
            }
            vkDestroyImage(c->device, got.img, NULL);   // the address was mapped elsewhere since
            PMS.moved++;
            return VK_NULL_HANDLE;
        }
        if (!busy) return VK_NULL_HANDLE;
        if (spins < 2000) _mm_pause();
        else plat_yield();
    }
}

// The entries of [lo, lo + size) and those bound into alloc go: the pool's memory is about to be freed. One being made
// is waited for. keep_frames: also every ready entry older than that many frames (0: only the range).
static void pm_gone(uint64_t lo, uint64_t size, VmaAllocation alloc, uint64_t keep_frames) {
    if (!__atomic_load_n(&pm_started, __ATOMIC_ACQUIRE)) return;
    VkCtx *c = vk_ctx();
    uint64_t now = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    for (;;) {
        int busy = 0;
        plat_mutex_lock(&pm_lock);
        for (int i = 0; i < PM_N; i++) {
            Premade *q = &pm[i];
            int in = q->addr >= lo && q->addr - lo < size;
            if (q->state == PM_QUEUED && in) {
                __atomic_store_n(&q->state, PM_FREE, __ATOMIC_RELEASE);
            } else if (q->state == PM_BUSY) {
                busy |= in || alloc != NULL;   // which block it binds into shows only when it is done
            } else if (q->state == PM_READY &&
                       (in || (alloc && (q->bound_to == alloc || q->res_alloc == alloc)) || (keep_frames && now - q->frame > keep_frames))) {
                if (c) vkDestroyImage(c->device, q->img, NULL);
                __atomic_store_n(&q->state, PM_FREE, __ATOMIC_RELEASE);
                PMS.stale++;
            }
        }
        plat_mutex_unlock(&pm_lock);
        if (!busy) break;
        plat_yield();
    }
}

// The image of e bound into the pool memory behind its address, from create info ci: the one made ahead, else made
// now. VK_NULL_HANDLE when the image cannot be bound there.
static VkImage alias_image(VkCtx *c, Tex *e, const VkImageCreateInfo *ci, uint64_t guest_size) {
    VkImage img = pm_thread ? VK_NULL_HANDLE : pm_take(c, e, ci);
    if (img) return img;
    if (timed_create_image(c, ci, &img) != VK_SUCCESS) return VK_NULL_HANDLE;
    if (!bind_pool_memory(c, e, img, guest_size)) {
        vkDestroyImage(c->device, img, NULL);
        return VK_NULL_HANDLE;
    }
    return img;
}

// defer_init: the caller records the image's first transition (init_image or alias_copy).
// DEBUG-R8LOG: REKARINA_R8LOG=1 logs what happens to R8 textures 256 or more wide and 400 or less high (the subtitle lines):
// creation with the storage layout, CPU uploads, copies into them and passes that draw into them, up to 400 lines.
void vk_dbg_r8(const char *what, const tex_t *t, long a, long b) {
    static int on = -1, n;
    if (on < 0) on = cfg_get("REKARINA_R8LOG") != NULL;
    if (!on || !t || t->d.format != 0x1 || t->d.width < 256 || t->d.height > 400 || n >= 400) return;
    n++;
    rt_log("R8LOG frame %llu %s off %#llx mem %p %ux%u flags %#x levels %u stride %u pkg_bh %d has_layout %d lay0 %#x "
           "linear %d tileH %u size %llu | %ld %ld",
           (unsigned long long)__atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED), what, (unsigned long long)t->off,
           (const void *)t->mem, t->d.width, t->d.height, (unsigned)t->d.flags, t->d.levels, t->d.stride,
           (int)t->d.pkg_block_h, t->has_layout, t->layout[0], nvn_tex_is_linear(&t->d), t->L.tileH,
           (unsigned long long)t->L.size, a, b);
}

static Tex *create_tex(const tex_t *t, const TexKey *k, int defer_init) {
    static volatile int32_t n_unmapped, n_unsupported, n_failed, n_unbound, n_thick;
    VkCtx *c = vk_ctx();
    const nvn_texdesc *d = &t->d;
    if (!c || d->target == NVN_TEXTURE_TARGET_BUFFER) return NULL;
    TexVkFormat f;
    if (!tex_vk_format(d->format, &f)) {
        log_limited(&n_unmapped, "texture format %#x has no Vulkan format: %s", d->format, f.reason);
        return NULL;
    }
    if (!tex_vk_format_supported(&f)) {
        log_limited(&n_unsupported, "texture format %#x: VkFormat %d lacks features %#x", d->format, (int)f.vk, (unsigned)f.need);
        return NULL;
    }
    nvn_texlayout L;
    nvn_tex_layout(d, &L);
    vk_dbg_r8("create", t, defer_init, 0);   // DEBUG-R8LOG
    if (d->width >= 4096 && d->height >= 4096 && cfg_get("REKARINA_BIGLOG"))   // DEBUG-BIGLOG
    {
        rt_log("BIGLOG create %p fmt %#x flags %#x pool_flags %#x frame %llu", (const void *)t->mem, d->format, d->flags,
               (unsigned)t->pool_flags, (unsigned long long)__atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED));
        void vk_dump_arm_soon(void);
        void boot_shots_arm(uint64_t n);
        if (d->width == 12288) boot_shots_arm(__atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED));
        const char *aw = cfg_env("REKARINA_BIGLOG_ARM_W");   // the width whose creation arms the dump (default 4096)
        if (d->width == (uint32_t)(aw && *aw ? atoi(aw) : 4096) && (d->format == 0x34 || d->format == 0x32)) vk_dump_arm_soon();
    }
    uint32_t w = umax(d->width, 1), h = umax(d->height, 1), depth = 1, layers = 1;
    VkImageType type = VK_IMAGE_TYPE_2D;
    switch (d->target) {
    case NVN_TEXTURE_TARGET_1D: type = VK_IMAGE_TYPE_1D; h = 1; break;
    case NVN_TEXTURE_TARGET_1D_ARRAY: type = VK_IMAGE_TYPE_1D; h = 1; layers = L.layers; break;
    case NVN_TEXTURE_TARGET_3D: type = VK_IMAGE_TYPE_3D; depth = umax(d->depth, 1); break;
    case NVN_TEXTURE_TARGET_2D_ARRAY: case NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY:
    case NVN_TEXTURE_TARGET_CUBEMAP: case NVN_TEXTURE_TARGET_CUBEMAP_ARRAY: layers = L.layers; break;
    default: break;
    }
    int scale = shadow_scale_of(d, &f, d->samples > 1 ? (uint32_t)d->samples : 1, tex_mirrored(t));
    w *= (uint32_t)scale, h *= (uint32_t)scale;
    uint32_t levels = umax(d->levels, 1), most = 1;
    for (uint32_t s = umax(umax(w, h), depth); s > 1; s >>= 1) most++;
    if (levels > most) levels = most;
    uint32_t samples = d->samples > 1 ? (uint32_t)d->samples : 1;
    if (samples > 1) levels = 1;
    VkFormatFeatureFlags ff = features_of(f.vk);
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    int ds = f.depth || f.stencil;
    if (ds && (ff & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (!ds && (ff & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if ((d->flags & NVN_TEXTURE_FLAGS_IMAGE) && (ff & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) && samples == 1)
        usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    // contact shadows (vk_contact.c) darken the game's B10G11R11 scene colour in place from a compute shader
    if (f.vk == VK_FORMAT_B10G11R11_UFLOAT_PACK32 && (usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) && samples == 1 &&
        (ff & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) && w >= 640 && vk_contact_strength())
        usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    VkImageCreateFlags flags = ds ? 0 : VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;   // format-reinterpreting views
#ifdef GAME_NS2
    // NVN2 games write through image slots into textures made without NVN_TEXTURE_FLAGS_IMAGE: OoT's movie converts its
    // YUV frame into an RGBA8 texture of flags 0 and composes its output into an sRGB one of flags COMPRESSIBLE, with
    // nothing to say they are images until the dispatch binds them (MNG nvn_mtl.m image_prepare upgrades a texture
    // then). Without the usage the replay bound a stand-in and the writes were lost: the movie came out black. So every
    // single-sample colour image a storage view can serve gets it here; an sRGB one through its UNORM format
    // (EXTENDED_USAGE; vk_exec_draw.c get_view makes such views, and leaves the usage out of its sRGB views). An image
    // in device-only pool memory that cannot alias that memory with the usage is made without it (below).
    VkImageUsageFlags ns2_usage = 0;
    VkImageCreateFlags ns2_flags = 0;
    if (!ds && samples == 1 && !(usage & VK_IMAGE_USAGE_STORAGE_BIT)) {
        VkFormat sf = vk_storage_format(f.vk);
        if (ff & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) ns2_usage = VK_IMAGE_USAGE_STORAGE_BIT;
        else if (sf != f.vk && (features_of(sf) & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT))
            ns2_usage = VK_IMAGE_USAGE_STORAGE_BIT, ns2_flags = VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
        usage |= ns2_usage, flags |= ns2_flags;
    }
#endif
    // Cube compatible only for cube targets: the replay makes cube views of nothing else (vk_exec_draw.c get_view checks
    // the image's own view type). The flag on every square array of 6+ layers changed how AMD's Windows driver lays such
    // an image out, and terrain material arrays sampled through it came out in rainbow blocks that moved with the mip
    // level (Savage Wind Recomp issue #59, BotW on an RX 580 and a 7800 XT).
    if (type == VK_IMAGE_TYPE_2D && layers >= 6 && w == h && samples == 1 &&
        (d->target == NVN_TEXTURE_TARGET_CUBEMAP || d->target == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY))
        flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    // NVN renders into a 3D texture slice by slice (a view's min layer is the depth plane, nvn_mtl.m), e.g. the
    // grading LUT of the menu background blur. Vulkan needs a 2D view of the slice for that.
    if (type == VK_IMAGE_TYPE_3D && (usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
    // The 512x512 float maps of the ground fog's fluid simulation are only read and written by compute shaders. Created as
    // possible render targets too, one of their passes (cs 3a7117b072154c38) took 9 ms where its twin took 0.2 ms, on the frames
    // the simulation runs; without those usages the same frames took 30 ms instead of 39 in two runs of three.
    // REKARINA_SIM_NO_ATTACH=0 keeps the usages.
    if (w == 512 && h == 512 && !tex_mirrored(t) && (f.vk == VK_FORMAT_R16G16_SFLOAT || f.vk == VK_FORMAT_R16G16B16A16_SFLOAT) &&
        (usage & VK_IMAGE_USAGE_STORAGE_BIT) && rt_knob("SIM_NO_ATTACH", "1"))
        usage &= ~(VkImageUsageFlags)(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT);
    {   // DEBUG-TEXFLAGS: REKARINA_TEXFLAGS=1: each distinct (flags, format, target) of a new non-depth image, once, with its size
        static struct { uint32_t fl, fm, tg; } seen[256];
        static int ns;
        if (cfg_env("REKARINA_TEXFLAGS") && !ds && ns < 256) {
            int found = 0;
            for (int i = 0; i < ns; i++) found |= seen[i].fl == (uint32_t)d->flags && seen[i].fm == (uint32_t)d->format && seen[i].tg == (uint32_t)d->target;
            if (!found) {
                seen[ns].fl = (uint32_t)d->flags, seen[ns].fm = (uint32_t)d->format, seen[ns].tg = (uint32_t)d->target, ns++;
                rt_log("TEXFLAGS flags %#x nvn fmt %#x target %d %ux%u levels %u usage %#x mirrored %d", (unsigned)d->flags, (unsigned)d->format, (int)d->target, w, h, levels, (unsigned)usage, tex_mirrored(t));
            }
        }
    }
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = flags, .imageType = type, .format = f.vk,
                            .extent = {w, h, depth}, .mipLevels = levels, .arrayLayers = layers,
                            .samples = (VkSampleCountFlagBits)samples, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    int bw, bh, block_views = 0;
    // REKARINA_BLOCK_VIEWS_3D (default 0): a 3D BC image also gets block views. On RDNA2 cards (driver 26.10.1, issue #5)
    // the 32x32x32 BC4 volume the game creates at the logo, made BLOCK_TEXEL_VIEW_COMPATIBLE and 2D_ARRAY_COMPATIBLE,
    // took a GPU write just past the end of its memory: device lost at the first submit after it when the image ended
    // its memory block. Draws into a 3D BC image go through a stand-in instead (vk_exec.c bc_target).
    static int bv3d = -1;
    if (bv3d < 0) {
        bv3d = rt_knob("BLOCK_VIEWS_3D", "0");
        rt_log("vk: 3D BC textures %s (REKARINA_BLOCK_VIEWS_3D=%d)", bv3d ? "get block views (renderable)" : "have no block views",
               bv3d);
    }
    if (!ds && samples == 1 && f.vk == f.ideal && nvn_fmt_bpb(d->format, &bw, &bh) && bw == 4 && bh == 4 &&
        (type != VK_IMAGE_TYPE_3D || bv3d)) {
        VkImageCreateInfo bc = ci;   // BC formats: renderable through views of their blocks (block_views_ok)
        bc.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        bc.flags |= VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
        if (type == VK_IMAGE_TYPE_3D) bc.flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
        VkImageUsageFlags hidden = 0;   // creation usage the replay never uses (views restrict theirs, vk_exec.c)
        int ok = block_views_ok(&bc);
        if (ok && type == VK_IMAGE_TYPE_3D && !image_size(&bc)) {
            // STORAGE usage keeps RADV on GFX9 from the thin layout (image_size), so agl's BC4 encoder still renders
            // into the volume; else the image is made without block views and its encoder passes are left out
            bc.usage |= VK_IMAGE_USAGE_STORAGE_BIT, hidden = VK_IMAGE_USAGE_STORAGE_BIT;
            ok = block_views_ok(&bc) && image_size(&bc);
            log_limited(&n_thick, "texture fmt %#x %ux%ux%u: the device cannot lay out a 3D image with block views; %s",
                        d->format, w, h, depth, ok ? "made with storage usage (a thick layout)" : "made without them");
        }
        if (ok) ci = bc, usage = bc.usage & ~hidden, flags = bc.flags, block_views = 1;
    }
    // Feedback loops (vk.h VkCtx.feedback): every attachable image may be sampled by a draw of a pass that renders it,
    // so it gets the usage now; adding it when a loop shows up would mean recreating the image (that lost the
    // grass tile maps once).
    int fb_usage = c->feedback == VK_FEEDBACK_LAYOUT && !block_views &&
                   (usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT));
    if (fb_usage) ci.usage = usage |= VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT;
    Tex *e = calloc(1, sizeof *e);
    if (!e) rt_fatal("vk: out of memory for a texture");
    e->key = *k;
    e->t = *t;
    e->f = f;
    e->pub.addr = k->addr;
    VkImage img = VK_NULL_HANDLE;
    uint64_t spk_t0 = plat_now_ns();   // DEBUG-SPIKE
    int spk_alias = 0;
    if (__builtin_expect(sec_on, 0) && !pm_thread) memset(ics, 0, sizeof ics);
    // REKARINA_TEX_FRESH_OWN=1 (a test, off): a render target at storage no other descriptor names takes memory of its own,
    // so that a released image can go to the next target of its shape (recycle_take). OoT puts its transient
    // targets at ever new addresses, 5-15 a frame in the village, and an image bound into pool memory serves no other
    // address. Measured there it lost: the game then names the same storage through a second descriptor (a depth target
    // and its R32F view, a 2560x1440 target and a 1280x720 one), which pool memory shares for free and separate images
    // share by a copy (alias_copy): GPU time went from 15 to 25.7 ms a frame, 46 to 32 fps.
    static int fresh_own = -1;
    if (fresh_own < 0) fresh_own = rt_knob("TEX_FRESH_OWN", "0");
    int fresh = fresh_own && !tex_mirrored(t) && samples == 1 &&
                (usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) &&
                !nvn_hm_get(&by_addr, &k->addr);
    if (scale == 1 && nvn_gpu_only(k->addr) && !fresh) {   // storage in a physical or virtual pool: alias the pool memory (section 3.2)
        spk_alias = 1;
        img = alias_image(c, e, &ci, L.size);
        if (!img) {
#ifdef GAME_NS2
            if (ns2_usage && !block_views) {   // the storage usage changed the image's layout: alias without it
                ci.usage = usage &= ~ns2_usage, ci.flags = flags &= ~ns2_flags;
                img = alias_image(c, e, &ci, L.size);
                if (!img) ci.usage = usage |= ns2_usage, ci.flags = flags |= ns2_flags;   // own memory, with the usage
                else if (!pm_thread)
                    log_limited(&n_unbound, "texture %#llx (%ux%u fmt %#x) made without storage usage to alias its pool "
                                "memory: an image slot bound to it gets a stand-in", (unsigned long long)k->addr, w, h, d->format);
            }
            if (!img && !pm_thread)
#else
            if (!pm_thread)
#endif
            log_limited(&n_unbound, "texture %#llx (%ux%u fmt %#x) cannot alias its pool memory; own memory instead",
                        (unsigned long long)k->addr, w, h, d->format);
        }
    }
    if (pm_thread) {   // made ahead (pm_main): the bound image goes into the table, the rest happens at its first use
        if (img) {
            pm_out->img = img, pm_out->ci = ci, pm_out->bound_to = e->bound_to;
            pm_out->res_alloc = bind_res.alloc, pm_out->res_at = bind_res.at, pm_out->res_size = bind_res.size;
        }
        free(e);
        return NULL;
    }
    // A texture whose content the CPU provides (tex_mirrored), up to SUBALLOC_MAX, takes memory from VMA's device blocks:
    // the driver prefers a dedicated allocation for most images and vmaCreateImage follows that, so loading a scene makes
    // one vkAllocateMemory (a kernel call) per new texture, thousands of them in the frames right after a save loads.
    // vmaAllocateMemory with the plain requirements sub-allocates instead, from the blocks the headroom thread keeps ahead
    // (vk_headroom.c). Render targets keep vmaCreateImage and the driver's preference.
    // Above REKARINA_TEX_SUBALLOC_KB (default 2048) a texture has memory of its own: freed, it goes back to the system at
    // once. A freed piece of a block stays the allocator's until the whole block is empty, and after a few map
    // changes the blocks held one to three gigabytes with nothing in it, which the driver counts as the game's all
    // the same (vk_texture.c sweep_pressure). The large textures are few and hold most of the bytes.
    static VkDeviceSize suballoc_max;
    if (!suballoc_max) {
        const char *v = cfg_get("REKARINA_TEX_SUBALLOC_KB");
        suballoc_max = (VkDeviceSize)(v && atoi(v) > 0 ? atoi(v) : 2048) << 10;
    }
    enum { SUBALLOC_MAX = 32 << 20 };
    int own_memory = 0;   // the size rule sent it to memory of its own
    if (!img && tex_mirrored(t) && samples == 1 && suballoc_on()) {
        if (timed_create_image(c, &ci, &img) == VK_SUCCESS) {
            // an image the driver requires (not only prefers) dedicated memory for keeps it: vmaCreateImage below
            VkMemoryDedicatedRequirements dr = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
            VkMemoryRequirements2 mr2 = {.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, .pNext = &dr};
            VkImageMemoryRequirementsInfo2 ri = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2, .image = img};
            uint64_t sec1 = sec_begin();
            vkGetImageMemoryRequirements2(c->device, &ri, &mr2);
            ic_end(ICS_REQ, SEC_IC_REQ, sec1);
            VkMemoryRequirements mr = mr2.memoryRequirements;
            VmaAllocationCreateInfo ai = {.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT};
            sec1 = sec_begin();
            own_memory = mr.size > suballoc_max;
            int ok = !dr.requiresDedicatedAllocation && mr.size <= SUBALLOC_MAX && !own_memory &&
                     vmaAllocateMemory(c->vma, &mr, &ai, &e->alloc, NULL) == VK_SUCCESS;
            sec_end(SEC_IC_ALLOC_SUB, sec1);
            sec1 = sec_begin();
            VkResult bound = ok ? vmaBindImageMemory(c->vma, e->alloc, img) : VK_SUCCESS;
            ic_end(ICS_BIND, SEC_IC_BIND_SUB_T + sec_mk, sec1);
            if (bound != VK_SUCCESS) {
                vmaFreeMemory(c->vma, e->alloc);
                e->alloc = NULL;
                ok = 0;
            }
            sec1 = sec_begin();
            if (ok) zero_suballoc(c, e);
            sec_end(SEC_IC_ZERO, sec1);
            if (!ok) {
                vkDestroyImage(c->device, img, NULL);
                img = VK_NULL_HANDLE, e->alloc = NULL;
            }
        } else {
            img = VK_NULL_HANDLE;
        }
    }
    if (!img && !tex_mirrored(t)) img = recycle_take(&ci, &e->alloc);
    if (!img) {
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
                                      .flags = own_memory ? VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT : 0};
        uint64_t sec1 = sec_begin();
        VkResult r = vmaCreateImage(c->vma, &ci, &ai, &img, &e->alloc, NULL);
        ic_end(ICS_VMA, SEC_IC_VMA_T + sec_mk, sec1);
        if (r == VK_SUCCESS) vk_live_check_born((uint64_t)(uintptr_t)img);   // DEBUG-LIVECHECK
        if (r != VK_SUCCESS) {
            log_limited(&n_failed, "image creation failed (%d): nvn fmt %#x %ux%ux%u target %d levels %u layers %u", (int)r,
                        d->format, w, h, depth, d->target, levels, layers);
            free(e);
            return NULL;
        }
    }
    vk_addr_label((uint64_t)img, "tex %#llx fmt %#x %ux%ux%u lv %u%s%s", (unsigned long long)(uintptr_t)t->mem, d->format, w, h, depth, levels, e->alloc ? " own" : " pool",
                  tex_mirrored(t) ? "" : " gpu");
    {   // DEBUG-SPIKE: a slow image creation
        uint64_t dt = plat_now_ns() - spk_t0;
        if ((vk_spike_on > 0 || sec_on) && dt > 3000000) {
            char split[96] = "";   // the calls are timed only under REKARINA_SECTIONS
            if (sec_on) snprintf(split, sizeof split, "; create %.1f requirements %.1f resolve %.1f bind %.1f vma %.1f", sec_ticks_ms(ics[ICS_CREATE]),
                                 sec_ticks_ms(ics[ICS_REQ]), sec_ticks_ms(ics[ICS_RESOLVE]), sec_ticks_ms(ics[ICS_BIND]), sec_ticks_ms(ics[ICS_VMA]));
            rt_log("TEXSLOW %.1f ms %s %ux%ux%u fmt %#x target %d levels %u layers %u%s", dt / 1e6, spk_alias ? (e->alloc ? "own memory after alias failed" : "alias") : "own memory",
                   w, h, depth, d->format, d->target, levels, layers, split);
        }
        if (__builtin_expect(sec_on, 0)) {   // a new image: was this address and shape destroyed before?
            int al = spk_alias && !e->alloc;
            uint64_t h = rep_hash(k);
            sec_count(al ? SEC_REP_ALIAS_NEW : SEC_REP_OWN_NEW, 1);
            if (rep_keys && rep_keys[h & (REP_SLOTS - 1)] == h) sec_count(al ? SEC_REP_ALIAS_REPEAT : SEC_REP_OWN_REPEAT, 1);
        }
    }
    {   // DEBUG-TEXNEW: REKARINA_TEXNEW=<frame>:<frames>: every image made in those frames
        static int on = -1;
        static uint64_t from, cnt;
        if (on < 0) { const char *v = cfg_get("REKARINA_TEXNEW"); on = v && sscanf(v, "%llu:%llu", (unsigned long long *)&from, (unsigned long long *)&cnt) == 2; }
        uint64_t f = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
        if (on && f >= from && f < from + cnt) {
            uint64_t h = rep_hash(k);
            if (!rep_keys) rep_keys = calloc(REP_SLOTS, sizeof *rep_keys);
            rt_log("TEXNEW %llu addr %#llx %ux%ux%u fmt %#x target %d levels %u layers %u %s%s", (unsigned long long)f,
                   (unsigned long long)k->addr, w, h ? (unsigned)0 + (unsigned)d->height : 0, depth, d->format, d->target, levels, layers,
                   e->alloc ? "own" : "alias", rep_keys && rep_keys[h & (REP_SLOTS - 1)] == h ? " repeat" : "");
        }
    }
    e->ci_flags = ci.flags;
    if (e->alloc) {
        VmaAllocationInfo own;
        vmaGetAllocationInfo(c->vma, e->alloc, &own);
        e->bytes = own.size;
    }
    // A new image's handle can be one a destroyed image had. Views are purged with the image (destroy_tex), so none
    // should be cached under it: one that is was made from a destroyed texture's record after its purge, and would
    // reach this image's memory through the dead one's. Dropped here, and counted in the log.
    {
        static int32_t n_stale;
        int stale = vk_exec_views_gone(img);
        if (stale) vk_tpool_image_gone(img);   // a pool slot written with one of those views resolves again
        if (stale && n_stale++ < 16)
            rt_log("vk: new image %#llx had %d cached views of a destroyed image with its handle; dropped",
                   (unsigned long long)(uintptr_t)img, stale);
    }
    VkTexImage *p = &e->pub;
    p->image = img;
    p->format = f.vk;
    p->type = type;
    p->usage = usage;
    p->aspect = aspect_of(&f);
    p->width = w, p->height = h, p->depth = depth, p->levels = levels, p->layers = layers, p->samples = samples;
    p->view_type = type == VK_IMAGE_TYPE_1D ? (d->target == NVN_TEXTURE_TARGET_1D_ARRAY ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D)
                 : type == VK_IMAGE_TYPE_3D ? VK_IMAGE_VIEW_TYPE_3D
                 : d->target == NVN_TEXTURE_TARGET_CUBEMAP && (flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) ? VK_IMAGE_VIEW_TYPE_CUBE
                 : d->target == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY && (flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY
                 : layers > 1 || d->target == NVN_TEXTURE_TARGET_2D_ARRAY || d->target == NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY
                     ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    p->alpha_one = f.alpha_one;
    memcpy(p->chan, f.chan, sizeof p->chan);
    p->swz[0] = (uint8_t)d->swizzle_r, p->swz[1] = (uint8_t)d->swizzle_g;
    p->swz[2] = (uint8_t)d->swizzle_b, p->swz[3] = (uint8_t)d->swizzle_a;
    if (!(p->swz[0] | p->swz[1] | p->swz[2] | p->swz[3]))   // descriptors built without nvnTextureBuilderSetDefaults
        p->swz[0] = NVN_TEXTURE_SWIZZLE_R, p->swz[1] = NVN_TEXTURE_SWIZZLE_G, p->swz[2] = NVN_TEXTURE_SWIZZLE_B,
        p->swz[3] = NVN_TEXTURE_SWIZZLE_A;
    p->mirrored = (uint8_t)tex_mirrored(t);
    p->bound = e->bound_to != NULL;
    p->block_views = (uint8_t)block_views;
    p->fb_usage = (uint8_t)fb_usage;
    p->scale = (uint8_t)scale;

    if (!defer_init) {
        init_image(e, vk_upload_begin());
        vk_upload_end();
    }
    return e;
}

static int owned_lower(uint64_t a) {
    int lo = 0, hi = nowned;
    while (lo < hi) {
        int m = (lo + hi) / 2;
        if (owned[m]->pub.addr < a) lo = m + 1;
        else hi = m;
    }
    return lo;
}

static void own_add(Tex *e) {
    if (e->listed) return;
    plat_mutex_lock(&owned_lock);
    if (nowned == cowned) {
        cowned = cowned ? 2 * cowned : 256;
        if (!(owned = realloc(owned, sizeof *owned * (size_t)cowned))) rt_fatal("vk: out of memory for the owned-texture list");
    }
    int i = owned_lower(e->pub.addr);
    memmove(owned + i + 1, owned + i, sizeof *owned * (size_t)(nowned - i));
    owned[i] = e;
    __atomic_store_n(&nowned, nowned + 1, __ATOMIC_RELEASE);
    if (e->t.L.size > max_owned) max_owned = e->t.L.size;
    e->listed = 1;
    plat_mutex_unlock(&owned_lock);
}

static void own_remove(Tex *e) {
    if (!e->listed) return;
    plat_mutex_lock(&owned_lock);
    for (int i = owned_lower(e->pub.addr); i < nowned && owned[i]->pub.addr == e->pub.addr; i++)
        if (owned[i] == e) {
            memmove(owned + i, owned + i + 1, sizeof *owned * (size_t)(nowned - i - 1));
            __atomic_store_n(&nowned, nowned - 1, __ATOMIC_RELEASE);
            break;
        }
    e->listed = 0;
    plat_mutex_unlock(&owned_lock);
}

static void rel_append(Tex *e) {
    e->rel_next = NULL;
    e->rel_prev = rel_tail;
    if (rel_tail) rel_tail->rel_next = e;
    else rel_head = e;
    rel_tail = e;
}

static void rel_unlink(Tex *e) {
    if (e->rel_prev) e->rel_prev->rel_next = e->rel_next;
    else rel_head = e->rel_next;
    if (e->rel_next) e->rel_next->rel_prev = e->rel_prev;
    else rel_tail = e->rel_prev;
    e->rel_prev = e->rel_next = NULL;
}

static void destroy_tex(Tex *e) {
    if (__builtin_expect(sec_on, 0)) {
        if (!rep_keys) rep_keys = calloc(REP_SLOTS, sizeof *rep_keys);
        if (rep_keys) {
            uint64_t h = rep_hash(&e->key);
            rep_keys[h & (REP_SLOTS - 1)] = h;
        }
    }
    own_remove(e);
    tex_gen_bump_entry(e->key.addr, e->texq_base);
    nvn_hm_del(&by_key, &e->key);
    Tex *head = nvn_hm_get(&by_addr, &e->key.addr);
    if (head == e) {
        if (e->next_at) nvn_hm_put(&by_addr, &e->key.addr, e->next_at);
        else nvn_hm_del(&by_addr, &e->key.addr);
    } else {
        for (Tex *p = head; p; p = p->next_at)
            if (p->next_at == e) { p->next_at = e->next_at; break; }
    }
    if (e->released) nreleased--, rel_unlink(e);
    if (e->pub.image) vk_exec_image_gone(e->pub.image);
    if (!park_image(e)) vk_defer_image(e->pub.image, e->alloc);
    free(e->snap);
    free(e->edge);
    // The record goes once the GPU is past the submission being recorded, as its image: the replay keeps &e->pub as a
    // render target for the rest of a command buffer (vk_exec.c resolve_target) and DLSS for the frame, and this runs on
    // whichever thread released the texture. Freed here, a pass that began again read the freed record, cached a view of
    // the dead image (vk_exec_target_view), and a later image given the same handle drew through it into freed memory.
    e->pub.gone = 1;
    vk_defer_free(e);
}

// REKARINA_TEX_RECYCLE=<frames> (default 30, 0: off): a new GPU-only image takes the image and memory of a released entry of the same
// shape that has been released at least that long, instead of allocating. The console renders transient targets into the same
// memory one after the other; here each address got an image of its own, about 3 GB released and waiting in heavy scenes.
static VkImage recycle_take(const VkImageCreateInfo *ci, VmaAllocation *out) {
    static int age = -1;
    static uint64_t taken;
    if (age < 0) {
        const char *s = cfg_env("REKARINA_TEX_RECYCLE");
        // OoT: 6 frames. Its transient targets never come back to an address (REKARINA_TEXNEW: 3195 distinct
        // address and shape pairs of 3273 new images), so 30 frames only kept ~240 dead targets, ~850 MiB, waiting.
        age = s ? atoi(s) :
#ifdef GAME_OOT
              6;
#else
              30;
#endif
    }
    if (!age) return VK_NULL_HANDLE;
    uint64_t now = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED), done = vk_sync_fence_done();
    for (Tex *r = rel_head; r && now - r->rel_frame >= (uint64_t)age; r = r->rel_next) {
        if (!r->alloc || r->bound_to || r->pub.mirrored || r->wr_fence > done || released_in_use(r, now)) continue;
        if (r->pub.format != ci->format || r->pub.width != ci->extent.width || r->pub.height != ci->extent.height ||
            r->pub.depth != ci->extent.depth || r->pub.levels != ci->mipLevels || r->pub.layers != ci->arrayLayers ||
            r->pub.samples != (uint32_t)ci->samples || r->pub.usage != ci->usage || r->ci_flags != ci->flags) continue;
        VkImage img = r->pub.image;
        *out = r->alloc;
        vk_exec_image_gone(img);
        r->pub.image = VK_NULL_HANDLE, r->alloc = NULL;
        destroy_tex(r);
        if (!(++taken & 255)) rt_log("vk: recycled %llu released images", (unsigned long long)taken);
        return img;
    }
    return VK_NULL_HANDLE;
}

// ---- REKARINA_TEX_ADOPT (default on; 0: off): a new texture whose storage holds the bytes a released one was filled
// from takes that one's image instead of an upload.
// The game's texture manager compacts its memory all the time in some places (the thread aglTextureDataMgr::
// Compaction; the Market and the field, not the forest): it copies a texture's bytes to another address,
// finalizes the texture object and initializes one of the same shape there, a block of 9 to 14 MiB every few frames
// and back again later. On the console that is a memcpy. Here the new address was a new image, created and filled
// again: about 1.5 images a frame, 0.6 to 0.8 ms of the replay for the staging copies, as much again for the GPU's
// copies, and frames of 25 to 33 ms where a few large ones met (the GPU busy for 20 ms; 2026-11-12).
// The bytes are hashed when an entry with memory of its own is made (64 bits over the whole storage, 4 lanes; a
// collision needs the same shape and size too). A released entry of the same descriptor and hash that the GPU never
// wrote and whose storage was not uploaded a second time holds exactly these texels: the two entries trade images and
// the released one goes, with the new, empty image. Among the newest ADOPT_LOOK released entries only: the move's
// source was released a moment ago.
#define ADOPT_LOOK 512
static uint64_t n_adopted, adopted_bytes, n_adopt_miss;

static inline uint64_t rotl64(uint64_t v, int r) { return (v << r) | (v >> (64 - r)); }
static uint64_t content_hash(const uint8_t *p, uint64_t n) {
    const uint64_t M = 0xff51afd7ed558ccdull;
    uint64_t h[4] = {0x9e3779b97f4a7c15ull ^ n, 0xc2b2ae3d27d4eb4full, 0x165667b19e3779f9ull, 0x27d4eb2f165667c5ull};
    for (; n >= 32; p += 32, n -= 32) {
        uint64_t w[4];
        memcpy(w, p, 32);
        for (int i = 0; i < 4; i++) h[i] = rotl64(h[i] ^ w[i], 29) * M;
    }
    if (n) {
        uint64_t w[4] = {0};
        memcpy(w, p, (size_t)n);
        for (int i = 0; i < 4; i++) h[i] = rotl64(h[i] ^ w[i], 29) * M;
    }
    uint64_t r = h[0] ^ rotl64(h[1], 17) ^ rotl64(h[2], 31) ^ rotl64(h[3], 47);
    r ^= r >> 33, r *= 0xc4ceb9fe1a85ec53ull, r ^= r >> 29;
    return r;
}

static int comes_back_intact(const Tex *e);

// ---- moved storage
// The Market's texture compaction moves a texture's bytes with one memcpy of 9 to 14 MiB and registers the texture at
// the new address; a few moves later it moves them back. The new entry found its image by the hash of its storage
// (adopt_released), and hashing was what such a frame cost: six textures in every sixth frame, 0.4 to 0.5 ms each,
// 2.5 to 3 ms on the replay thread (17 % of the Market's frames, 2026-11-13). The game's large copies pass through
// nvn_api_core.c ov_memcpy, which notes them here. A new entry whose storage the newest such copy filled, from the
// storage address of a released or parked texture with the same descriptor, size and content print, has that
// texture's bytes and takes its hash. The print is the hash of 64 samples of 64 bytes spread over the storage and
// its two ends: it stands between the copy's record and a storage written again since, by other means than a large
// copy. REKARINA_TEX_MOVED=0: every new entry hashes its storage.
#define MOVES 4096
static struct { uint64_t dst, src, n; } moves[MOVES];
static uint32_t nmoves;
static PlatMutex moves_lock = PLAT_MUTEX_INIT;
static uint64_t n_moved_hits;

void gfx_guest_moved(uint64_t dst, uint64_t src, uint64_t n) {   // any guest thread
    plat_mutex_lock(&moves_lock);
    moves[nmoves++ % MOVES] = (typeof(moves[0])){dst, src, n};
    plat_mutex_unlock(&moves_lock);
}

// 1: the newest noted copy that touches [addr, addr + size) filled all of it; *src: where those bytes came from.
static int moved_from(uint64_t addr, uint64_t size, uint64_t *src) {
    int found = 0;
    plat_mutex_lock(&moves_lock);
    for (uint32_t k = 0; k < MOVES && k < nmoves; k++) {
        const typeof(moves[0]) *m = &moves[(nmoves - 1 - k) % MOVES];
        if (addr >= m->dst + m->n || addr + size <= m->dst) continue;   // does not touch it
        if (addr >= m->dst && addr + size <= m->dst + m->n) *src = m->src + (addr - m->dst), found = 1;
        break;   // the newest one that touches it decides
    }
    plat_mutex_unlock(&moves_lock);
    return found;
}

// 1: the newest noted copy that read from [addr, addr + size) took all of it; *dst: where those bytes went.
static int moved_to(uint64_t addr, uint64_t size, uint64_t *dst) {
    int found = 0;
    plat_mutex_lock(&moves_lock);
    for (uint32_t k = 0; k < MOVES && k < nmoves; k++) {
        const typeof(moves[0]) *m = &moves[(nmoves - 1 - k) % MOVES];
        if (addr >= m->src + m->n || addr + size <= m->src) continue;
        if (addr >= m->src && addr + size <= m->src + m->n) *dst = m->dst + (addr - m->src), found = 1;
        break;
    }
    plat_mutex_unlock(&moves_lock);
    return found;
}

static uint64_t content_print(const uint8_t *p, uint64_t n) {
    uint8_t buf[66 * 64];
    if (n <= sizeof buf) return content_hash(p, n) ^ 0x5bd1e995u;
    memcpy(buf, p, 64);
    memcpy(buf + 64, p + n - 64, 64);
    for (uint64_t i = 0; i < 64; i++) memcpy(buf + 128 + i * 64, p + ((n - 64) / 64 * i & ~(uint64_t)63), 64);
    return content_hash(buf, sizeof buf) ^ n;
}

// Images of destroyed entries, kept by what they hold: the move's source is often destroyed before its copy is made
// (another texture is initialized at its address, or it leaves the released list), and the same block comes back to
// an address it had a few moves ago. At most PARK_MAX images and REKARINA_TEX_PARK_MB of video memory (default 384; 0: none
// are kept), each for PARK_FRAMES presents at most; the oldest goes first. The cache is outside the released entries'
// pressure rule (sweep_pressure destroys those above 75 % of the device's budget, which a 12 GiB card at 1440p is
// always above, and their images come here), so it has its own: never more than 1/32 of the budget (128 MiB on a
// 4 GiB card), and above PARK_FULL percent of the budget nothing is kept and what is here goes, a few images a sweep.
// tex_lock held everywhere.
#define PARK_MAX 512
#define PARK_FRAMES 1800
#define PARK_FULL 97
typedef struct {
    uint64_t chash, size, bytes, frame;
    uint64_t cprint, addr;        // the entry's content_print and storage address (adopt_released, "moved storage")
    uint8_t moved;                // kept for its bytes' new address, outside the budget (park_image)
    nvn_texdesc d;
    uint8_t has_layout, layout[8];
    VkImage image;
    VmaAllocation alloc;
    VkFormat format;
    VkImageUsageFlags usage;
    VkImageCreateFlags ci_flags;
    uint32_t width, height, depth, levels, layers, samples;
} Parked;
static Parked parked[PARK_MAX];
static int nparked;
static uint64_t parked_bytes, park_budget = ~0ull, park_cap = ~0ull, n_parked, n_park_taken;
static uint64_t moved_bytes, n_moved_parked;   // of parked_bytes: images kept for a moved texture's new address
#define MOVED_FRAMES 240   // such an image waits this long for its texture to be made at the new address
static int park_hold;   // the device is above PARK_FULL percent of its budget (sweep_pressure)

static void park_drop(int i) {
    vk_defer_image(parked[i].image, parked[i].alloc);
    parked_bytes -= parked[i].bytes;
    if (parked[i].moved) moved_bytes -= parked[i].bytes;
    parked[i] = parked[--nparked];
}

static void park_trim(uint64_t room) {
    uint64_t now = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    for (int i = nparked - 1; i >= 0; i--)
        if (now - parked[i].frame > (parked[i].moved ? MOVED_FRAMES : PARK_FRAMES)) park_drop(i);
    uint64_t limit = park_budget < park_cap ? park_budget : park_cap;
    // the images kept for moved textures are outside the budget: they go by age, or oldest first when the table is full
    while (nparked && (nparked >= PARK_MAX || parked_bytes - moved_bytes + room > limit)) {
        int old = -1;
        for (int i = 0; i < nparked; i++)
            if ((!parked[i].moved || nparked >= PARK_MAX) && (old < 0 || parked[i].frame < parked[old].frame)) old = i;
        if (old < 0) break;
        park_drop(old);
    }
}

// The storage of e was copied elsewhere by a noted copy and the bytes there are still e's: the game is moving the
// texture, and the entry it makes at the new address looks for this image (adopt_released). Kept for that entry it is
// no extra video memory: the entry would otherwise get an image of its own and an upload. Most moved textures lost
// their image before that entry was made (another texture was moved into their old storage first, 94 % of the moves
// noted while running through the Market), and where video memory is short the cache kept nothing.
static int moved_away(const Tex *e) {
    static int moved_on = -1;
    if (moved_on < 0) { const char *v = cfg_env("REKARINA_TEX_MOVED"); moved_on = !v || *v != '0'; }
    uint64_t dst;
    VkMemSpan m;
    if (!moved_on || !moved_to(e->key.addr, e->t.L.size, &dst) || !vk_mem_resolve(dst, e->t.L.size, &m)) return 0;
    return content_print((const uint8_t *)(uintptr_t)dst, e->t.L.size) == e->cprint;
}

// destroy_tex: e's image goes into the cache instead of being destroyed. 1: taken (the caller must not destroy it).
static int park_image(Tex *e) {
    if (park_budget == ~0ull) {
        const char *v = cfg_env("REKARINA_TEX_PARK_MB"), *on = cfg_env("REKARINA_TEX_ADOPT");
        park_budget = (on && *on == '0') ? 0 : (uint64_t)(v && *v ? atoi(v) : 384) << 20;
    }
    uint64_t limit = park_budget < park_cap ? park_budget : park_cap;
    if (!park_budget || !e->has_chash || !e->uploaded || !e->alloc || e->bound_to || !e->pub.image || !comes_back_intact(e) ||
        !e->bytes)
        return 0;
    int moved = moved_away(e);
    if (!moved && (park_hold || e->bytes > limit / 4)) return 0;
    for (int i = 0; i < nparked; i++)   // one image for a content is enough
        if (parked[i].chash == e->chash && parked[i].size == e->t.L.size && !memcmp(&parked[i].d, &e->key.d, sizeof e->key.d)) return 0;
    park_trim(moved ? 0 : e->bytes);
    if (nparked >= PARK_MAX) return 0;
    if (moved) {
        moved_bytes += e->bytes;
        if (!(++n_moved_parked & 2047))
            rt_log("vk: image cache: %llu images kept for moved textures so far (%.0f MiB waiting now)",
                   (unsigned long long)n_moved_parked, moved_bytes / 1048576.0);
    }
    parked[nparked++] = (Parked){.chash = e->chash, .size = e->t.L.size, .bytes = e->bytes, .cprint = e->cprint,
                                 .addr = e->key.addr, .moved = (uint8_t)moved,
                                 .frame = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED), .d = e->key.d,
                                 .has_layout = e->key.has_layout, .image = e->pub.image, .alloc = e->alloc,
                                 .format = e->pub.format, .usage = e->pub.usage, .ci_flags = e->ci_flags,
                                 .width = e->pub.width, .height = e->pub.height, .depth = e->pub.depth,
                                 .levels = e->pub.levels, .layers = e->pub.layers, .samples = e->pub.samples};
    memcpy(parked[nparked - 1].layout, e->key.layout, sizeof e->key.layout);
    parked_bytes += e->bytes;
    n_parked++;
    // e->pub.image stays as it is: the replay may hold &e->pub for the rest of the command buffer it is recording
    // (destroy_tex), and the image it names lives on here, with the same texels. Cleared, a draw recorded after this
    // sampled a null image and the device was lost in the Market within two minutes.
    return 1;
}

static void park_clear(void) {
    while (nparked) park_drop(0);
}

// sweep_pressure, with the device's use and budget: the cache's cap, and its retreat when the device is nearly full.
static void park_pressure(uint64_t used, uint64_t budget) {
    static int full = -1;   // REKARINA_TEX_PARK_FULL=<percent> overrides PARK_FULL
    if (full < 0) { const char *v = cfg_env("REKARINA_TEX_PARK_FULL"); full = v && atoi(v) > 0 ? atoi(v) : PARK_FULL; }
    park_cap = budget / 32;
    park_hold = used > budget / 100 * (uint64_t)full;
    static uint64_t held, sweeps;
    held += park_hold, sweeps++;
    if (!(sweeps & 16383))
        rt_log("vk: image cache: %d images, %.0f MiB of at most %.0f; held back by video memory in %llu of %llu sweeps (%.0f of %.0f MiB in use)",
               nparked, parked_bytes / 1048576.0, park_cap / 1048576.0, (unsigned long long)held, (unsigned long long)sweeps,
               used / 1048576.0, budget / 1048576.0);
    for (int i = 0; park_hold && nparked && i < 8; i++) {
        int old = -1;
        for (int k = 0; k < nparked; k++)
            if (!parked[k].moved && (old < 0 || parked[k].frame < parked[old].frame)) old = k;
        if (old < 0) break;
        park_drop(old);
    }
}

// e was just made for t and nothing has used its image yet (entry_of, tex_lock held). 1: e holds its texels now.
static int adopt_released(Tex *e, const tex_t *t) {
    static int on = -1;
    if (on < 0) { const char *v = cfg_env("REKARINA_TEX_ADOPT"); on = !v || *v != '0'; }
    e->has_chash = 0;
    if (!on || !e->pub.mirrored || e->pub.gpu_owned || !e->alloc || e->bound_to || !t->mem || t->L.size < 4096) return 0;
    uint32_t gen;
    vk_mem_changed(e->pub.addr, t->L.size, 0, &gen);   // resets the write bits before the storage is read, as make_fresh
    int pre = up_worker && up_pre.valid && up_pre.addr == (uintptr_t)t->mem && up_pre.size == t->L.size;
    e->cprint = pre ? up_pre.cprint : content_print((const uint8_t *)(uintptr_t)t->mem, t->L.size);
    Tex *src_rel = NULL;
    {   // "moved storage": the hash of the texture these bytes were copied from
        static int moved_on = -1;
        if (moved_on < 0) { const char *v = cfg_env("REKARINA_TEX_MOVED"); moved_on = !v || *v != '0'; }
        uint64_t src = 0;
        int have = 0;
        if (moved_on && moved_from((uintptr_t)t->mem, t->L.size, &src)) {
            TexKey sk = e->key;
            sk.addr = src;
            Tex *r = nvn_hm_get(&by_key, &sk);   // the texture these bytes were: the same descriptor at the source
            if (r && r != e && r->released && r->has_chash && r->cprint == e->cprint && r->t.L.size == t->L.size)
                e->chash = r->chash, have = 1, src_rel = r;
            for (int i = 0; i < nparked && !have; i++)
                if (parked[i].addr == src && parked[i].cprint == e->cprint && parked[i].size == t->L.size &&
                    !memcmp(&parked[i].d, &e->key.d, sizeof e->key.d))
                    e->chash = parked[i].chash, have = 1;
        }
        static uint64_t n_calls, n_copy, n_hashed_bytes;
        n_calls++, n_copy += src != 0;
        if (have) n_moved_hits++;
        else if (pre) e->chash = up_pre.chash;
        else e->chash = content_hash((const uint8_t *)(uintptr_t)t->mem, t->L.size), n_hashed_bytes += t->L.size;
        if (!(n_calls & 8191))
            rt_log("vk: new textures looked for their image by content: %llu so far, %llu filled by a noted copy, %llu of "
                   "those took the copy's source hash, %.1f GiB hashed", (unsigned long long)n_calls,
                   (unsigned long long)n_copy, (unsigned long long)n_moved_hits, n_hashed_bytes / 1073741824.0);
        e->has_chash = 1;
    }
    uint64_t done = vk_sync_fence_done();
    int looked = 0;
    // the released texture at the copy's source first: the list below is only looked at ADOPT_LOOK entries deep, and
    // the game's transient targets push a moved texture's old entry past that within a frame or two
    for (Tex *r = src_rel ? src_rel : rel_tail; r && looked < ADOPT_LOOK; r = r == src_rel ? rel_tail : r->rel_prev, looked++) {
        if (r == e || !r->has_chash || r->chash != e->chash || r->t.L.size != t->L.size) continue;
        if (!r->uploaded || !r->alloc || r->bound_to || !comes_back_intact(r) || r->wr_fence > done || !r->pub.image)
            continue;
        if (memcmp(&r->key.d, &e->key.d, sizeof e->key.d) || r->key.has_layout != e->key.has_layout ||
            memcmp(r->key.layout, e->key.layout, sizeof e->key.layout))
            continue;
        if (r->pub.format != e->pub.format || r->pub.usage != e->pub.usage || r->ci_flags != e->ci_flags ||
            r->pub.levels != e->pub.levels || r->pub.layers != e->pub.layers || r->pub.width != e->pub.width ||
            r->pub.height != e->pub.height || r->pub.depth != e->pub.depth || r->pub.samples != e->pub.samples)
            continue;
        vk_exec_image_gone(e->pub.image);
        vk_exec_image_gone(r->pub.image);
        VkImage img = e->pub.image;
        VmaAllocation al = e->alloc;
        uint64_t by = e->bytes;
        e->pub.image = r->pub.image, e->alloc = r->alloc, e->bytes = r->bytes;
        r->pub.image = img, r->alloc = al, r->bytes = by;
        r->has_chash = 0;   // it holds the empty image now: nothing to keep (park_image)
        destroy_tex(r);
        e->wgen = gen;
        e->uploaded = 1;
        e->n_up++;
        n_adopted++, adopted_bytes += t->L.size;
        if (!(n_adopted & 1023))
            rt_log("vk: %llu moved textures took their released images (%.1f GiB not uploaded again; %llu new textures found none)",
                   (unsigned long long)n_adopted, adopted_bytes / 1073741824.0, (unsigned long long)n_adopt_miss);
        return 1;
    }
    for (int i = 0; i < nparked; i++) {
        Parked *q = &parked[i];
        if (q->chash != e->chash || q->size != t->L.size || memcmp(&q->d, &e->key.d, sizeof e->key.d) ||
            q->has_layout != e->key.has_layout || memcmp(q->layout, e->key.layout, sizeof e->key.layout) ||
            q->format != e->pub.format || q->usage != e->pub.usage || q->ci_flags != e->ci_flags || q->levels != e->pub.levels ||
            q->layers != e->pub.layers || q->width != e->pub.width || q->height != e->pub.height || q->depth != e->pub.depth ||
            q->samples != e->pub.samples)
            continue;
        vk_exec_image_gone(e->pub.image);
        vk_defer_image(e->pub.image, e->alloc);   // the empty image made for e
        e->pub.image = q->image, e->alloc = q->alloc, e->bytes = q->bytes;
        parked_bytes -= q->bytes;
        if (q->moved) moved_bytes -= q->bytes;
        *q = parked[--nparked];
        e->wgen = gen;
        e->uploaded = 1;
        e->n_up++;
        n_adopted++, n_park_taken++, adopted_bytes += t->L.size;
        if (!(n_adopted & 1023))
            rt_log("vk: %llu moved textures took their released images (%.1f GiB not uploaded again; %llu new textures found none)",
                   (unsigned long long)n_adopted, adopted_bytes / 1073741824.0, (unsigned long long)n_adopt_miss);
        return 1;
    }
    n_adopt_miss++;
    return 0;
}

// Released entries with another descriptor at the same address are replaced; past RELEASED_MAX released entries the
// oldest one goes at each release (it comes back on demand, a mirrored one re-uploaded from its storage). TotK
// finalizes and initializes transient textures every frame, so a released entry is mostly taken back within frames.
// Dropping all of them at once when the count passed 4096 destroyed thousands of images in one frame: 333-422 ms
// frames every 40-50 s on a GTX 1050 Ti, then the textures still in use were created and uploaded again.
#define RELEASED_MAX 2048
// A released entry that lookups still reach is in use, whatever the game did with its texture object. OoT
// finalizes the object of its far shadow map (12288x12288 D16, 288 MiB of its own) and of the 4096x4096 one at a
// map's arrival and goes on reading them through their slots for as long as the map is on screen; the entries wait
// here, released, with the only copy of what the GPU drew. Counted from the release, the age rule (sweep_pressure)
// destroyed them 600 frames after the arrival, the limit of this list earlier still when a burst of transient
// targets filled it, and the lookup that came next made an empty image: at the castle the ground lost its sunlight
// six seconds after the scene appeared, while the map's name was still on screen, and stayed in shade (the step in
// the brightness of every frame's thumbnail, exactly 600 frames after the first lit one, 2026-11-14). So the clock
// of a released entry is its last lookup (touch: use_frame), not its release.
#define RELEASED_IDLE 120   // frames without a lookup before a released entry counts as let go for good
static int released_in_use(const Tex *e, uint64_t now) {
    static int on = -1;   // REKARINA_REL_KEEP=0: the old clock, the release (a test: the castle's ground goes dark again)
    if (on < 0) on = rt_knob("REL_KEEP", "1") != 0;
    return on && e->use_frame > e->rel_frame && now - e->use_frame < RELEASED_IDLE;
}

static void sweep_released(void) {
    uint64_t now = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    Tex *e = rel_head;
    for (int looked = 0; nreleased > RELEASED_MAX && e && looked < 256; looked++) {   // the oldest that nothing reads
        Tex *next = e->rel_next;
        if (!released_in_use(e, now)) destroy_tex(e);
        e = next;
    }
}

// REKARINA_TEX_VRAM_PRESSURE (percent of the device-local budget, default 85; 0 = off): above it, each submit drops up to
// PRESSURE_STEP of the oldest released entries, so a 4 GiB card does not keep RELEASED_MAX released images (each ASTC
// one decoded to RGBA8) while the heap spills into system memory. A few per submit, for the reason above. First the
// entries that come back intact: mirrored ones the GPU never wrote, re-uploaded from their storage. A render target
// lives only in its image; dropping it brought the texture back empty (black flashes in a Depths boss fight, ~170
// "left uninitialised" aliases a minute: transient targets released and taken back within frames). So a target goes
// only once it has stayed released for REKARINA_TEX_DROP_AGE frames (default 600; 0: never). With 30, the value from
// 2026-11-10 to 2026-11-12, OoT lost what its lighting reads: it lets go of targets for longer than that
// and takes them back with their content, and in the Market the lit scene came out black for one frame 20 to
// 30 times in 95 s (fog, particles and HUD on a dark scene: the "shadow flicker" of the performance video at 1:22;
// measured with the flight recorder's flash watch at 80 % of the mean, 0 to 1 with 600). OoT kept ~2,000
// released entries of 2.9 GiB at 95 % of a 12 GiB card's budget, while pools that wanted a VRAM mirror (which needs
// used + 1/8 of the budget to fit, vk_memory.c) stayed in host memory and the GPU read them over the bus. The default
// is below that 87.5 % line, so the cache gives memory back before mirrors are refused.
#define PRESSURE_STEP 32
static int vram_pressure = -1, drop_age = -1;
static uint64_t n_pressure_drops, n_age_drops;

static int comes_back_intact(const Tex *e) { return e->pub.mirrored && !e->pub.gpu_owned && !e->gpu_dirty; }

// When a lookup last reached an entry: entry_of (every texture get and slot resolution) and the per-submit check of a
// slot lookup (vk_texture_by_id_locked). The texture pool refresh looks every registered slot up once in each
// submission that draws through the pool, so an entry no lookup reached for IDLE_ROUNDS such submissions and STALE_FRAMES
// presents has no live descriptor naming it: the game keeps the texture object, nothing samples it. The clock is
// look_round, which only a submission whose slot checks reached half of the registered pool slots moves on
// (vk_texture_new_submit): counted in submissions, a pause screen or a fade that draws nothing through the pool would
// make the whole world's textures idle within seconds.
#define IDLE_ROUNDS 240
#define STALE_FRAMES 120
static uint32_t look_round = 1;     // submissions whose lookups walked the texture pool
static uint32_t round_checks;       // slot checks of the submission being recorded (vk_texture_by_id_locked)
static uint32_t pool_slots;         // registered slots
// What draws use. The texture pool's set (vk_texpool.c) looks every registered slot up in turn, so the rule above
// only ever found textures the game had unregistered, and the game keeps the textures of the maps it left registered
// as long as its own heap has room: the pool made all of them resident. The forest, then the field, the Market
// and the castle held 7.3 GiB of textures on a 12 GiB card, video memory went over the budget and the frame rate from
// 95 to 28 with frames of 100 ms (2026-11-14; the title screen afterwards ran at 8). So an entry also keeps when a
// draw's binding last named it: a lookup that is not the pool's (pool_lookup). The clock is draw_round, moved on by
// the submissions that bound DRAW_SUBMIT_MIN slots or more for the first time in them: a pause screen or a movie
// draws a handful of textures and must not make the scene behind it look unused. An entry no draw named for
// COLD_ROUNDS such submissions and COLD_FRAMES presents is cold: evict_cold takes those, least recently drawn first,
// while video memory is over the line, and the pool leaves an evicted texture out (cold_keys) until a draw asks for it.
// Three ages, by how badly room is needed (sweep_pressure). Long cold (COLD_LONG of both clocks, half a minute and
// more of play): the textures of a map the player left. Short cold (COLD_SHORT, some seconds): also what the camera
// turned away from, uploaded again at its next turn. And while no scene is on screen (a loading screen: scene_frame)
// whatever no draw named for COLD_LOAD presents: the map behind the loading screen is gone from the screen, and what
// goes now is not there when the next map's textures arrive, so the two maps never have to fit together.
#define DRAW_SUBMIT_MIN 24
#define COLD_SHORT 600
#define COLD_LONG 3600
#define COLD_LOAD 30
static uint64_t scene_frame;        // gfx_counters.frames when a submission last drew a scene
static int room_short;              // sweep_pressure: the room for new textures is under its reserve
static int cs_trim_due;   // sweep_pressure found video memory short: vk_texture_new_submit trims the unused compute pipelines

// The game's format is ASTC, whatever the image holds (BC3 blocks, decoded texels, or ASTC itself where the card
// samples it): the class the picture test showed no shader to read through the pool (evictable).
static int fmt_astc(const TexVkFormat *f) {
    return f->astc_bc || f->astc_bw || (f->vk >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && f->vk <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK);
}
static uint32_t draw_round = 1;     // submissions that drew a scene
static uint32_t draw_checks;        // slots a draw looked up first in the submission being recorded
static int pool_lookup;             // the lookup under way is the texture pool's (pool_slot_locked; tex_lock)
static void drawn(Tex *e) {
    e->draw_round = draw_round;
    e->draw_frame = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
}
static void touch(Tex *e) {
    e->use_round = look_round;
    // The texture pool resolves every registered slot, also the stale ones no draw reads: its lookup does not keep a
    // released entry (released_in_use). Counted, the pool kept ~2,000 released targets "in use" past the limit of the
    // list, and the sweeps dropped and the pool made them again in bursts (frames of 50-68 ms, 2026-11-15).
    if (!(pool_lookup && e->released)) e->use_frame = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    if (!pool_lookup || !e->draw_round) drawn(e);   // a new entry ages from its making
}
static int cold(const Tex *e, uint64_t now, uint32_t age) {
    return draw_round - e->draw_round >= age && now - e->draw_frame >= age;
}

// The keys of evicted textures (evict_cold). The pool's lookups make no entry for one (slot_resolve): its slot shows
// the stand-in, which nothing reads, since what was evicted is of a class no shader reads through the pool
// (evictable). A draw's lookup makes the entry as for a texture seen the first time and takes the key out (entry_of).
// When the game finalizes the texture the key is done: what comes to that storage next is another texture, which the
// pool makes resident as any new one. It stays for COLD_MOVE_FRAMES as a mark of where an evicted texture was, because
// the game's heap compaction moves textures (a copy, a new texture object at the new address, the old one finalized,
// in an order not relied on here): the texture at the new address is the evicted one still and stays out
// (cold_kept_out by the noted copy, vk_texture_register by the slot). The keys of a memory pool go with it
// (vk_texture_range_gone).
// Values: COLD_KEY (a draw's binding brings it back), COLD_TAB (a texture no draw ever bound: a handle table naming
// it brings it back too, ht_take), or COLD_GONE + (the frame the texture was finalized in) * 2 + (1 for COLD_TAB).
#define COLD_KEY ((void *)(uintptr_t)1)
#define COLD_TAB ((void *)(uintptr_t)2)
static int cold_val(const void *v) { return v == COLD_KEY || v == COLD_TAB; }
#define COLD_GONE 16
#define COLD_MOVE_FRAMES 240
static nvn_hmap cold_keys = {.klen = sizeof(TexKey)};
static uint32_t n_cold;        // keys of evicted textures (not the marks of finalized ones)
static uint64_t n_cold_kept;   // pool lookups left without an entry so far
static void cold_put(const TexKey *k, void *kind) {
    if (!cold_val(nvn_hm_put(&cold_keys, k, kind))) n_cold++;
}
static void *cold_del(const TexKey *k) {
    void *v = cold_keys.n ? nvn_hm_del(&cold_keys, k) : NULL;
    if (cold_val(v)) n_cold--;
    return v;
}
// k is evicted, or was when its texture was finalized a moment ago (what moved from there is the same texture): its
// kind, COLD_KEY or COLD_TAB. NULL otherwise.
static void *cold_or_just_gone(const TexKey *k) {
    void *v = cold_keys.n ? nvn_hm_get(&cold_keys, k) : NULL;
    if (!v || cold_val(v)) return v;
    uint64_t now = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED), w = (uint64_t)(uintptr_t)v - COLD_GONE;
    if (now - (w >> 1) <= COLD_MOVE_FRAMES) return w & 1 ? COLD_TAB : COLD_KEY;
    nvn_hm_del(&cold_keys, k);
    return NULL;
}
static void cold_released(const TexKey *k) {   // vk_texture_release
    void *v = cold_keys.n ? nvn_hm_get(&cold_keys, k) : NULL;
    if (!cold_val(v)) return;
    uint64_t now = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    nvn_hm_put(&cold_keys, k, (void *)(uintptr_t)(COLD_GONE + (now << 1 | (v == COLD_TAB))));
    n_cold--;
}
static int moved_from(uint64_t addr, uint64_t size, uint64_t *src);
// slot_resolve, before it makes an entry for k (storage of size bytes): 1 when the lookup is the pool's and the
// texture is an evicted one.
static int cold_kept_out(const TexKey *k, uint64_t size) {
    if (!pool_lookup || !cold_keys.n) return 0;
    if (!cold_val(nvn_hm_get(&cold_keys, k))) {
        uint64_t src;
        TexKey sk = *k;
        if (!size || !moved_from(k->addr, size, &src)) return 0;
        sk.addr = src;
        void *kind = cold_or_just_gone(&sk);
        if (!kind) return 0;
        cold_put(k, kind);   // the evicted texture, moved here
    }
    n_cold_kept++;
    return 1;
}

// ---------------------------------------------------------------- handle tables
// What the shaders read through the texture pool's set (vk_texpool.c) no binding shows: a fragment shader loads a
// texture handle from a storage buffer of its draw (the game's material and baked-lighting tables, megabytes of
// records) and indexes the pool with it. In every one of 500 cached shaders with pool arrays that were traced, the
// index came from loads of the draw's storage buffers and from nothing else (2026-11-14). So the storage buffers of
// the draws whose pipelines read the pool are the handle tables (vk_exec_draw.c names them: vk_texture_handle_table),
// and a thread of its own reads them a few times a second and notes every word that has the form of a handle: its
// sampler id (bits 20 and up) is a registered sampler, its texture id (bits 0-19) a slot. The replay then counts the
// entries of those slots as used by a draw (vk_texture_new_submit: ht_take), and an evicted texture a table names
// again comes back through the pool. A word that only looks like a handle keeps a texture that could have gone;
// nothing a shader can read through the pool is missed as long as its table is one of these buffers.
// The tables are read with plat_mem_read, which fails where the game has unmapped the memory meanwhile.
#define HT_N 64
#define HT_IDS (1u << 20)
#define HT_KEEP_FRAMES 1800       // a table no draw named for this long is not read any more
#define HT_TABLE_MAX (64ull << 20)
typedef struct { uint64_t addr, size, frame; } HTab;
static HTab htab[HT_N];
static PlatMutex ht_lock = PLAT_MUTEX_INIT;   // htab, ht_pub, ht_gen
static PlatEvent ht_ev;                       // a new table: read now
static volatile int32_t ht_started;
static uint64_t *ht_pub;                      // the newest finished reading: a bit per texture id
static uint32_t ht_gen, ht_taken;             // readings finished / taken in by the replay (tex_lock)
static uint64_t ht_frame;                     // the frame the newest reading finished in
static uint8_t samp_reg[4096];                // registered sampler ids (racy reads by the thread: a late id is seen next time)
static struct { uint64_t scans, bytes, words, fails, ids, marked, revived; } HS;   // since the last perf line

static void ht_main(void *arg) {
    (void)arg;
    enum { CHUNK = 1 << 20 };
    plat_thread_background();   // nobody waits for a reading: off the cores the frame runs on, where there are slower ones
    uint8_t *buf = malloc(CHUNK);
    uint64_t *bits = malloc(HT_IDS / 8);
    if (!buf || !bits) return;
    for (;;) {
        plat_event_wait_ns(&ht_ev, 250000000ull);
        HTab t[HT_N];
        int n = 0;
        uint64_t now = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
        plat_mutex_lock(&ht_lock);
        for (int i = 0; i < HT_N; i++)
            if (htab[i].addr && now - htab[i].frame <= HT_KEEP_FRAMES) t[n++] = htab[i];
        plat_mutex_unlock(&ht_lock);
        memset(bits, 0, HT_IDS / 8);
        uint64_t bytes = 0, words = 0, fails = 0;
        for (int i = 0; i < n; i++) {
            uint64_t size = t[i].size < HT_TABLE_MAX ? t[i].size : HT_TABLE_MAX;
            for (uint64_t off = 0; off < size; off += CHUNK) {
                size_t m = (size_t)(size - off < CHUNK ? size - off : CHUNK) & ~(size_t)3;
                if (!m) break;
                // A read fails as a whole when one page of it is not there (the game commits the large tables piece by
                // piece, or has unmapped the buffer meanwhile): then the chunk again a page at a time, with what is not
                // there read as zeros, so the pages that are there are never skipped.
                if (!plat_mem_read(t[i].addr + off, buf, m)) {
                    for (size_t pg = 0, pn; pg < m; pg += pn) {
                        pn = 4096 - (size_t)((t[i].addr + off + pg) & 4095);   // up to the page's end
                        if (pn > m - pg) pn = m - pg;
                        if (!plat_mem_read(t[i].addr + off + pg, buf + pg, pn)) memset(buf + pg, 0, pn), fails++;
                    }
                }
                bytes += m;
                const uint32_t *w = (const uint32_t *)buf;
                for (size_t k = 0; k < m / 4; k++) {
                    uint32_t v = w[k];
                    if (!v || !samp_reg[v >> 20]) continue;
                    bits[(v & (HT_IDS - 1)) >> 6] |= 1ull << (v & 63);
                    words++;
                }
            }
        }
        plat_mutex_lock(&ht_lock);
        if (!ht_pub) ht_pub = calloc(HT_IDS / 8, 1);
        if (ht_pub) memcpy(ht_pub, bits, HT_IDS / 8), ht_gen++;
        ht_frame = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
        HS.scans++, HS.bytes += bytes, HS.words += words, HS.fails += fails;
        plat_mutex_unlock(&ht_lock);
    }
}

// vk_exec_draw.c, any thread that records draws: [addr, addr + size) is a storage buffer of a draw whose pipeline reads
// the texture pool.
static int evict_cls(void);
void vk_texture_handle_table(uint64_t addr, uint64_t size) {
    static _Thread_local struct { uint64_t addr, frame; } last[8];
    static _Thread_local uint32_t turn;
    if (!addr || !size || evict_cls() < 3) return;   // the tables are read only where their word decides (evictable)
    uint64_t now = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    for (int i = 0; i < 8; i++)
        if (last[i].addr == addr && now - last[i].frame < 30) return;   // told lately by this thread
    last[turn++ & 7] = (typeof(last[0])){addr, now};
    int fresh = 0, at = 0;
    plat_mutex_lock(&ht_lock);
    for (int i = 0; i < HT_N; i++) {
        if (htab[i].addr == addr) { at = i; goto found; }
        if (htab[i].frame < htab[at].frame) at = i;   // the one named longest ago goes
    }
    htab[at] = (HTab){addr, 0, 0}, fresh = 1;
found:
    if (size > htab[at].size) htab[at].size = size;
    htab[at].frame = now ? now : 1;
    plat_mutex_unlock(&ht_lock);
    if (!__atomic_load_n(&ht_started, __ATOMIC_ACQUIRE)) {
        plat_mutex_lock(&ht_lock);
        if (!ht_started) {
            plat_event_init(&ht_ev, 0, 0);
            if (plat_thread_spawn(ht_main, NULL, "vk handle tables", 0)) __atomic_store_n(&ht_started, 1, __ATOMIC_RELEASE);
            else __atomic_store_n(&ht_started, -1, __ATOMIC_RELEASE);
        }
        plat_mutex_unlock(&ht_lock);
    }
    if (fresh && ht_started > 0) plat_event_set(&ht_ev);
}

// 1 while the tables are being read: the thread runs and its newest reading is not older than a second or two.
// Without that, only the textures no table can name may be evicted (evictable).
static int ht_fresh(void) {
    if (__atomic_load_n(&ht_started, __ATOMIC_ACQUIRE) <= 0) return 0;
    uint64_t now = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    return ht_gen && now - ht_frame < 240;
}

// vk_texture_new_submit (tex_lock): the newest reading's texture ids count as used now.
static void ht_take(void) {
    static uint64_t *mine;
    if (__atomic_load_n(&ht_started, __ATOMIC_ACQUIRE) <= 0 || ht_gen == ht_taken) return;
    if (!mine && !(mine = malloc(HT_IDS / 8))) return;
    plat_mutex_lock(&ht_lock);
    memcpy(mine, ht_pub, HT_IDS / 8);
    ht_taken = ht_gen;
    plat_mutex_unlock(&ht_lock);
    int top = ntslots < (int)HT_IDS ? ntslots : (int)HT_IDS;
    for (int wi = 0; wi < (top + 63) / 64; wi++) {
        for (uint64_t b = mine[wi]; b; b &= b - 1) {
            int id = wi * 64 + __builtin_ctzll(b);
            if (id >= top || !tslots[id].used || !tslots[id].t.mem) continue;
            TexSlot *s = &tslots[id];
            HS.ids++;
            TexKey k[2];
            int nk = 1;
            make_key(&s->t, &k[0]);
            tex_t half;
            if (texq_mode > 0 && texq_shape(&s->t, &half)) make_key(&half, &k[nk++]);   // "texture quality": the smaller one
            for (int j = 0; j < nk; j++) {
                Tex *e = nvn_hm_get(&by_key, &k[j]);
                if (e && !e->released) {
                    // only for what nothing else shows the use of (evictable): the game's material tables name every
                    // texture it has loaded, the maps it left too, and counted for all of them nothing would ever go
                    if (!e->ever_drawn && !fmt_astc(&e->f)) drawn(e), HS.marked++;
                } else if (cold_keys.n && nvn_hm_get(&cold_keys, &k[j]) == COLD_TAB) {
                    cold_del(&k[j]);   // evicted, and a table names it again: the pool makes it
                    HS.revived++;
                    vk_tpool_slot_dirty(id);
                } else if (!e && room_short && !j) {
                    // held back while room is short (pool_holds_back) and named now: the pool looks at it at once
                    TexVkFormat f;
                    if (tex_vk_format(s->t.d.format, &f) && !(fmt_astc(&f) && s->t.d.depth <= 1)) vk_tpool_slot_dirty(id);
                }
            }
        }
    }
}

// Whether e may be evicted while the game keeps it registered: only what a draw binds by handle when it uses it.
// Baked lighting and the terrain's layers are read through the pool's set by handles in a storage buffer, with no
// binding to see; those stay as long as they are registered. Seen on screen with the pool showing the stand-in for
// one class at a time (REKARINA_TPOOL_HIDE, vk_texpool.c): without the textures decoded from ASTC the forest and the field
// look as they do with them (two runs of the same setting differ as much), without the other single-layer textures
// the lightmapped surfaces go dark and the stone path loses its stones.
// A texture some draw has bound (ever_drawn) is taken to be used through bindings: it goes by when a draw last
// named it, like the textures decoded from ASTC. The material tables name those too, for the maps the game left as
// well, so a table's word for them means nothing; with the pool showing the stand-in for every texture a draw had
// bound (REKARINA_TPOOL_HIDE=8) the picture did not change. A texture no draw ever bound is used through the pool if at
// all (the baked lighting's arrays among them): it goes by when a table last named it (ht_take), and only while the
// tables are being read (ht_fresh).
// The tables are read a few times a second, so a texture a table names for the first time is seen up to a quarter
// of a second late. The one that carries the handles (the game's table of baked lighting and material records, one
// buffer for everything loaded) is written when a map loads, behind the loading screen, and read from then on, so
// the textures of an arriving map are named before its first frame. Without this class the textures no draw ever
// bound stayed for every map visited (the masks and lighting of the parts of a map the player did not look at): 3.7
// GiB after six maps, and the card was over its budget again in the fifth minute of the tour.
// REKARINA_TEX_EVICT_CLASS: 0 none, 1 single-layer textures decoded from ASTC only, 2 those and every texture a draw
// has bound, 3 also what only the handle tables name, arrays and volumes included (default).
static int evict_class = -1;
static int evict_cls(void) {
    if (evict_class < 0) { const char *v = cfg_get("REKARINA_TEX_EVICT_CLASS"); evict_class = v && *v ? atoi(v) : 3; }
    return evict_class;
}
static int evictable(const Tex *e) {
    int cls = evict_cls(), many = e->pub.layers > 1 || e->pub.depth > 1;
    if (!cls) return 0;
    if (fmt_astc(&e->f) && !many) return 1;
    if (cls >= 2 && e->ever_drawn) return 1;
    return cls >= 3 && ht_fresh();
}

// slot_resolve, before it makes an entry for slot texture t on the pool's behalf (id: its slot). While room is short
// the pool makes no image for a texture nothing is known to use: an ASTC one (a draw makes it when it binds it), or
// one no handle table names. Made ahead as before, such textures were only uploaded to be evicted half a minute
// later: in a tour of six maps the evictions came to 11.8 GiB while 4 GiB of what stayed had never been bound by a
// draw. Nothing is noted for the slot: the pool asks again at its next turn and makes the image once there is room.
static int pool_holds_back(const tex_t *t, int id) {
    if (!pool_lookup || !room_short) return 0;
    TexVkFormat f;
    if (!tex_vk_format(t->d.format, &f)) return 0;
    if (fmt_astc(&f) && t->d.depth <= 1) return 1;
    if (evict_cls() < 3 || !ht_fresh()) return 0;   // what only a table can name is made ahead as ever (evictable)
    int named = 0;   // by the newest reading (ht_pub is replaced whole under ht_lock)
    plat_mutex_lock(&ht_lock);
    if (ht_pub && (unsigned)id < HT_IDS) named = (int)(ht_pub[id >> 6] >> (id & 63) & 1);
    plat_mutex_unlock(&ht_lock);
    return !named;
}

// REKARINA_TEX_EVICT (percent of the device-local budget, default 88, 0 = off): above it, once the released entries that
// come back intact are gone, live entries go too, least recently looked up first: those that come back intact (the CPU
// provides their bytes) and that are idle. A later lookup creates the image again and uploads it from its storage, as
// for a texture seen the first time. Up to EVICT_STEP entries per check; the next check waits until the GPU is past the
// submissions that may still name the images, as their memory goes back to the driver only then (vk_defer_image) and
// the budget would still show the old use. Going over the budget makes Windows move video memory to system memory, and
// the GPU stops for 100-250 ms at a time (2026-11-09: 9005 of 8789 MiB on the forest porch).
#define EVICT_STEP 128
static int tex_evict = -1;
static uint64_t evict_wait;                       // vk_sync_fence_done value the next eviction waits for
static uint64_t n_evict, n_evict_bytes;           // since the last perf line
static uint64_t n_evict_all, n_evict_all_bytes;   // so far
static uint64_t n_cold_back;                      // evicted textures a draw brought back so far
static int evict_logs;

// mode: 0 long cold, 1 short cold, 2 anything no draw named for COLD_LOAD presents (no scene is on screen).
static void evict_cold(uint64_t want, uint64_t used, uint64_t budget, int mode) {   // tex_lock held
    if (vk_sync_fence_done() < evict_wait) return;
    uint32_t min_age = mode ? COLD_SHORT : COLD_LONG;
    Tex *v[EVICT_STEP];
    int nv = 0;
    uint64_t now = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    for (uint32_t i = 0; i < by_key.cap; i++) {   // the EVICT_STEP candidates no draw named for the longest, those first
        Tex *e = by_key.vals[i];
        if (!e || e->released || !e->bytes || !comes_back_intact(e) || !evictable(e)) continue;
        if (mode == 2 ? now - e->draw_frame < COLD_LOAD : !cold(e, now, min_age)) continue;
        uint64_t age = now - e->draw_frame;   // presents: the round clock stands still behind a loading screen
        int at = nv;
        while (at > 0 && now - v[at - 1]->draw_frame < age) at--;
        if (at == EVICT_STEP) continue;
        if (nv < EVICT_STEP) nv++;
        memmove(v + at + 1, v + at, sizeof *v * (size_t)(nv - 1 - at));
        v[at] = e;
    }
    uint64_t freed = 0;
    int n = 0;
    if (cold_keys.n > 65536) nvn_hm_clear(&cold_keys, NULL), n_cold = 0;   // mostly marks of finalized textures: start over
    for (; n < nv && freed < want; n++) {
        freed += v[n]->bytes;
        cold_put(&v[n]->key, v[n]->ever_drawn || fmt_astc(&v[n]->f) ? COLD_KEY : COLD_TAB);
        destroy_tex(v[n]);
    }
    if (!n) return;
    n_evict += (uint64_t)n, n_evict_bytes += freed;
    n_evict_all += (uint64_t)n, n_evict_all_bytes += freed;
    evict_wait = vk_sync_issued() + 2;
    if (evict_logs < 4 || !(++evict_logs & ((rt_trace & RT_TRACE_PERF) ? 31 : 255))) {
        if (evict_logs < 4) evict_logs++;
        rt_log("vk: video memory %.0f of %.0f MiB in use: %llu textures no draw used lately evicted so far (%.2f GiB), "
               "the longest unused first%s; one comes back when a draw uses it (%llu did, %u wait)", used / 1048576.0,
               budget / 1048576.0, (unsigned long long)n_evict_all, n_evict_all_bytes / 1073741824.0,
               mode == 2 ? " (behind a loading screen)" : mode ? " (room is short)" : "",
               (unsigned long long)n_cold_back, n_cold);
    }
}

// Live textures with their own memory by what brings them back, the idle part apart, then the formats holding the most
// and the largest entries (REKARINA_TRACE=perf, every 600 frames).
static void texture_census(uint64_t frame) {   // tex_lock held
    enum { K_BC, K_CPU, K_GPU, K_N };
    static const char *const kn[K_N] = {"ASTC as BC", "other CPU-written", "GPU-written"};
    uint64_t n[K_N] = {0}, b[K_N] = {0}, in[K_N] = {0}, ib[K_N] = {0}, bound = 0, nd[K_N] = {0}, ndb[K_N] = {0};
    struct { int fmt; uint64_t n, b; } fm[48];
    int nfm = 0;
    struct { uint64_t b; uint32_t w, h, d; int fmt, gpu; } big[6] = {{0}};
    const double M = 1048576.0;
    uint64_t now = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    for (uint32_t i = 0; i < by_key.cap; i++) {
        const Tex *e = by_key.vals[i];
        if (!e || e->released) continue;
        if (!e->bytes) { bound += e->bound_to != NULL; continue; }
        int k = !comes_back_intact(e) ? K_GPU : e->f.astc_bc ? K_BC : K_CPU;
        n[k]++, b[k] += e->bytes;
        if (!e->ever_drawn) nd[k]++, ndb[k] += e->bytes;
        if (evictable(e) && comes_back_intact(e) && cold(e, now, COLD_LONG)) in[k]++, ib[k] += e->bytes;
        int j = 0;
        while (j < nfm && fm[j].fmt != (int)e->f.vk) j++;
        if (j == nfm && nfm < 48) fm[nfm++] = (typeof(fm[0])){(int)e->f.vk, 0, 0};
        if (j < nfm) fm[j].n++, fm[j].b += e->bytes;
        for (int q = 0; q < 6; q++)
            if (e->bytes > big[q].b) {
                memmove(big + q + 1, big + q, sizeof *big * (size_t)(5 - q));
                big[q] = (typeof(big[0])){e->bytes, e->pub.width, e->pub.height, e->pub.layers * e->pub.depth, (int)e->f.vk, k == K_GPU};
                break;
            }
    }
    char line[1024];
    int o = snprintf(line, sizeof line, "mem: textures frame %llu: live with own memory by kind, the long cold part that may be evicted in brackets:", (unsigned long long)frame);
    for (int k = 0; k < K_N; k++)
        o += snprintf(line + o, sizeof line - (size_t)o, " %s %llu %.0f MiB (%llu %.0f MiB), no draw ever bound %llu %.0f MiB;",
                      kn[k], (unsigned long long)n[k], b[k] / M, (unsigned long long)in[k], ib[k] / M,
                      (unsigned long long)nd[k], ndb[k] / M);
    snprintf(line + o, sizeof line - (size_t)o, " bound into pool memory %llu; evicted and not back %u, pool lookups left "
             "empty %llu", (unsigned long long)bound, n_cold, (unsigned long long)n_cold_kept);
    rt_log("%s", line);
    for (int x = 0; x < nfm; x++)
        for (int y = x + 1; y < nfm; y++)
            if (fm[y].b > fm[x].b) { typeof(fm[0]) t = fm[x]; fm[x] = fm[y], fm[y] = t; }
    o = snprintf(line, sizeof line, "mem: textures frame %llu: formats:", (unsigned long long)frame);
    for (int x = 0; x < nfm && x < 8; x++)
        o += snprintf(line + o, sizeof line - (size_t)o, " %d: %llu %.0f MiB;", fm[x].fmt, (unsigned long long)fm[x].n, fm[x].b / M);
    o += snprintf(line + o, sizeof line - (size_t)o, " largest:");
    for (int q = 0; q < 6 && big[q].b; q++)
        o += snprintf(line + o, sizeof line - (size_t)o, " %ux%ux%u fmt %d %s %.0f MiB;", big[q].w, big[q].h, big[q].d, big[q].fmt,
                      big[q].gpu ? "gpu" : "cpu", big[q].b / M);
    rt_log("%s", line);
}

static void sweep_pressure(void) {   // tex_lock held
    if (vram_pressure < 0) {
        const char *s = cfg_get("REKARINA_TEX_VRAM_PRESSURE");
        vram_pressure = s && *s ? atoi(s) : 75;
        if (vram_pressure < 0 || vram_pressure > 100) vram_pressure = 75;
        const char *a = cfg_get("REKARINA_TEX_DROP_AGE");
        drop_age = a && *a ? atoi(a) : 600;
        if (drop_age < 0) drop_age = 0;
        rt_log("vk: released textures under VRAM pressure: %s%d%%", vram_pressure ? "dropped above " : "off", vram_pressure);
    }
    if (tex_evict < 0) {
        const char *s = cfg_get("REKARINA_TEX_EVICT");
        tex_evict = s && *s ? atoi(s) : 88;
        if (tex_evict < 0 || tex_evict > 100) tex_evict = 88;
        if (tex_evict) rt_log("vk: textures no draw used lately are evicted while the room under %d%% of the video memory budget is short", tex_evict);
        else rt_log("vk: textures are never evicted (REKARINA_TEX_EVICT=0)");
    }
    // the budget query every 8 submits; every second one behind a loading screen, where textures arrive by the hundred
    uint64_t now_f = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    int no_scene = now_f - scene_frame >= COLD_LOAD;
    if ((!vram_pressure && !tex_evict) || submit_epoch & (no_scene ? 1 : 7)) return;
    uint64_t used, budget;
    int exact, dropped = 0;
    if (!vk_mem_vram(&used, &budget, &exact)) return;
    park_pressure(used, budget);
    uint64_t freed = 0;
    if (vram_pressure && rel_head && used > budget / 100 * (uint64_t)vram_pressure) {
        for (Tex *e = rel_head, *next; e && dropped < PRESSURE_STEP; e = next) {
            next = e->rel_next;
            if (!comes_back_intact(e)) continue;
            freed += e->bytes;
            destroy_tex(e);
            dropped++, n_pressure_drops++;
        }
        if (drop_age && dropped < PRESSURE_STEP) {
            // Then anything released long enough ago, oldest first (the list is in release order).
            uint64_t now = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
            for (Tex *e = rel_head, *next; e && dropped < PRESSURE_STEP && now - e->rel_frame >= (uint64_t)drop_age; e = next) {
                next = e->rel_next;
                if (released_in_use(e, now)) continue;   // read through its slot still (above sweep_released)
                freed += e->bytes;
                destroy_tex(e);
                dropped++, n_age_drops++;
            }
        }
    }
    // Room for what comes next: what is left under the line (REKARINA_TEX_EVICT percent of the budget) plus what the
    // allocator's blocks hold free. Freed textures leave their bytes in the blocks (a block goes back to the system
    // only when nothing is left in it), so the use the driver reports hardly falls after an eviction, and judged by it
    // alone every texture went the moment it was long cold, for no gain: the tour of six maps evicted 4.3 GiB that
    // way while 1.9 GiB of the blocks stood free. While the room is at least the reserve, nothing goes.
    uint64_t line = budget / 100 * (uint64_t)tex_evict;
    if (tex_evict) {
        uint64_t holes = vk_mem_vma_free(), filled = used > holes ? used - holes : 0;   // what really holds something
        uint64_t room = line > filled ? line - filled : 0;
        uint64_t reserve = budget / 12 > (512ull << 20) ? budget / 12 : 512ull << 20;
        room_short = room < reserve;   // the pool makes nothing ahead that nothing is known to use (pool_holds_back)
        if (room + freed < reserve) {
            int mode = no_scene ? 2 : room < reserve / 3 || used > budget ? 1 : 0;
            evict_cold(reserve - room - freed + (64ull << 20), used, budget, mode);
            cs_trim_due = 1;   // the compute pipelines nothing dispatched go too (vk_texture_new_submit, outside the lock)
        }
    }
}

// REKARINA_TRACE=perf: what the released entries hold (tex_lock held). Own memory only: an entry bound into pool memory
// costs nothing of its own.
static void released_census(uint64_t frame) {
    struct { uint64_t n, bytes; } cls[3] = {{0}};   // 0 droppable (clean mirror), 1 GPU content, 2 other
    struct { uint32_t w, h, fmt, n; uint64_t bytes; } top[8] = {{0}};
    uint64_t now = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED), old = 0, old_bytes = 0;
    for (Tex *e = rel_head; e; e = e->rel_next) {
        uint64_t b = 0;
        if (e->alloc) {
            VmaAllocationInfo ai;
            vmaGetAllocationInfo(vk_ctx()->vma, e->alloc, &ai);
            b = ai.size;
        }
        int k = e->pub.mirrored && !e->pub.gpu_owned && !e->gpu_dirty ? 0 : e->pub.gpu_owned || e->gpu_dirty ? 1 : 2;
        cls[k].n++, cls[k].bytes += b;
        if (now - e->rel_frame >= 600) old++, old_bytes += b;
        int j = 0;
        while (j < 8 && top[j].n && !(top[j].w == e->key.d.width && top[j].h == e->key.d.height && top[j].fmt == (uint32_t)e->key.d.format)) j++;
        if (j == 8) {   // replace the smallest
            j = 0;
            for (int i = 1; i < 8; i++) if (top[i].bytes < top[j].bytes) j = i;
            if (top[j].bytes > b) continue;
            top[j].n = 0, top[j].bytes = 0;
        }
        top[j].w = e->key.d.width, top[j].h = e->key.d.height, top[j].fmt = (uint32_t)e->key.d.format;
        top[j].n++, top[j].bytes += b;
    }
    rt_log("perf: frame %llu: released textures: clean mirrors %llu (%.0f MiB), GPU content %llu (%.0f MiB), other %llu "
           "(%.0f MiB); released 600+ frames ago %llu (%.0f MiB); dropped by age %llu", (unsigned long long)frame,
           (unsigned long long)cls[0].n, cls[0].bytes / 1048576.0, (unsigned long long)cls[1].n, cls[1].bytes / 1048576.0,
           (unsigned long long)cls[2].n, cls[2].bytes / 1048576.0, (unsigned long long)old, old_bytes / 1048576.0,
           (unsigned long long)n_age_drops);
    char line[512];
    size_t o = 0;
    for (int i = 0; i < 8 && o < sizeof line - 64; i++)
        if (top[i].n)
            o += (size_t)snprintf(line + o, sizeof line - o, " %ux%u/%#x*%u=%.0fMiB", top[i].w, top[i].h, top[i].fmt, top[i].n,
                                  top[i].bytes / 1048576.0);
    rt_log("perf: frame %llu: released textures by size:%s", (unsigned long long)frame, o ? line : " none");
    n_age_drops = 0;
}

// ---------------------------------------------------------------- uploads
static uint32_t texel_alpha_one(int format, uint32_t *elem) {
    switch (format) {
    case 0x19: *elem = 1; return 0xFF;
    case 0x1A: *elem = 1; return 0x7F;
    case 0x1B: case 0x1C: *elem = 1; return 1;
    case 0x1D: *elem = 2; return 0x3C00;
    case 0x1E: *elem = 2; return 0xFFFF;
    case 0x1F: *elem = 2; return 0x7FFF;
    case 0x20: case 0x21: *elem = 2; return 1;
    case 0x22: *elem = 4; return 0x3F800000;
    default: *elem = 4; return 1;   // 0x23, 0x24
    }
}

// 3-component texels to 4 (n texels).
static void expand_rgb(int format, const uint8_t *src, uint8_t *dst, size_t n) {
    uint32_t e, one = texel_alpha_one(format, &e);
    for (size_t i = 0; i < n; i++, src += 3 * e, dst += 4 * e) {
        memcpy(dst, src, 3 * e);
        memcpy(dst + 3 * e, &one, e);
    }
}

static void shrink_rgb(int format, const uint8_t *src, uint8_t *dst, size_t n) {
    uint32_t e;
    texel_alpha_one(format, &e);
    for (size_t i = 0; i < n; i++, src += 4 * e, dst += 3 * e) memcpy(dst, src, 3 * e);
}

// REKARINA_ASTC=check: the compute decoder's texels of n regions (gpu: the regions one after the other) against
// astc_decode_ldr of the same raw blocks (raw: the staging bytes the regions point into).
static void astc_check(const Tex *e, const uint8_t *raw, const VkAstcRegion *ar, int n, const uint8_t *gpu) {
    static volatile int32_t n_bad;
    static uint64_t n_tex, n_texels, n_diff_tex;
    uint32_t bw = e->f.astc_bw, bh = e->f.astc_bh;
    uint64_t diff = 0, first = UINT64_MAX;
    uint32_t want = 0, got = 0;
    int first_k = 0;
    for (int k = 0; k < n; k++) {
        const VkAstcRegion *r = &ar[k];
        size_t sb = (size_t)((r->w + bw - 1) / bw) * ((r->h + bh - 1) / bh) * 16, st = (size_t)r->w * r->h * 4;
        uint8_t *cpu = malloc(st);
        for (uint32_t z = 0; z < r->slices; z++, gpu += st) {
            astc_decode_ldr(raw + r->src + z * sb, r->w, r->h, bw, bh, e->f.astc_srgb, cpu);
            if (!memcmp(cpu, gpu, st)) continue;
            for (size_t i = 0; i < st / 4; i++)
                if (memcmp(cpu + 4 * i, gpu + 4 * i, 4)) {
                    if (first == UINT64_MAX) {
                        first = i, first_k = k;
                        memcpy(&want, cpu + 4 * i, 4), memcpy(&got, gpu + 4 * i, 4);
                    }
                    diff++;
                }
        }
        free(cpu);
        n_texels += (uint64_t)r->w * r->h * r->slices;
    }
    n_tex++;
    if (diff) {
        n_diff_tex++;
        log_limited(&n_bad, "ASTC check: texture %#llx fmt %#x %ux%u: %llu texels differ, first region %d texel %llu "
                    "cpu %08x gpu %08x", (unsigned long long)e->pub.addr, e->t.d.format, bw, bh,
                    (unsigned long long)diff, first_k, (unsigned long long)first, want, got);
    }
    if (n_tex == 1 || !(n_tex & 63) || diff)
        rt_log("vk: ASTC check: %llu textures, %llu texels compared, %llu textures differ", (unsigned long long)n_tex,
               (unsigned long long)n_texels, (unsigned long long)n_diff_tex);
}

// Staging bytes of one part of a texture upload decoded or copied on the CPU (see upload). With REKARINA_ASTC=cpu a
// large ASTC array decodes to RGBA8 in one go: a staging buffer of 676682336 bytes failed (-2) on an Intel UHD with
// 8 GB of shared memory and the game closed before the title screen.
#define UPLOAD_PART (32ull << 20)   // fits the staging pool (vk_staging takes up to half of its 64 MiB block)

// Copies the whole storage into the image (every level and layer the image has).
// ---- the texture cache (tex_cache, REKARINA_TEX_CACHE; on by default since 2026-11-13): the BC3 form of ASTC textures on disk.
// With REKARINA_ASTC_BC the GPU decodes an ASTC texture and encodes it to BC3 at its first use: 16 to 35 ms of GPU time
// for one of the field's large textures, in the frame that first draws it, so running through the field had 58
// frames over 25 ms a minute where standing and turning had none (2026-11-12). The blocks that encode makes are the
// same every time, so they are kept: tex-cache/<hash of the storage>-<format>-<size>-<levels>x<layers>.bc3 beside the
// game (REKARINA_TEX_CACHE_DIR names another folder), every level and layer in upload's order. A texture whose storage
// has that hash is filled from the file with one buffer-to-image copy. A miss decodes as before and also copies the
// blocks to a host buffer, which the reaper thread writes out once the GPU is past the batch (vk_defer_call); the
// storage is hashed again after it was read, and a texture the game wrote meanwhile is not kept.
// The folder holds at most tex_cache_mb MiB (REKARINA_TEX_CACHE_MB, default 14336): a thread counts it at start and deletes
// files, in the order the listing gives them, until it fits; while the game runs nothing new is written once the
// count and what this run wrote reach the limit. Nothing fills the folder ahead of play yet.
// On by default since 2026-11-13. What it gains depends on what decides the frame: docs/oot-notes.md, "The morning's
// control runs" (nothing in the Market, the field's long frames where the GPU is the limit). The device
// losses in the Market that were first laid at its door came from the texture pool (vk_texpool.c update).
typedef struct {
    char path[600];
    uint8_t *map;
    uint64_t size;
    VkBuffer buf;
    VmaAllocation alloc;
    int keep;
} TexCacheJob;
static volatile int64_t tc_loaded, tc_saved, tc_load_bytes, tc_failed;
static volatile int64_t tc_disk = -1;   // bytes in the folder: -1 until the start's count is done, then kept by the writes
static int64_t tc_limit;

typedef struct { const char *dir; int64_t total, limit, deleted; int files; } TexCacheScan;
static int tex_cache_count(const char *name, void *ctx) {
    TexCacheScan *s = ctx;
    char path[700];
    snprintf(path, sizeof path, "%s/%s", s->dir, name);
    int64_t n = plat_file_size(path);
    if (n < 0) return 1;
    if (s->total + n > s->limit) {   // over the limit from here on: this file goes
        plat_file_delete(path);
        s->deleted += n;
        return 1;
    }
    s->total += n, s->files++;
    return 1;
}
// The folder's files into the system's file cache, once at start. After a restart of the machine they are on the disk
// only, and the replay thread read each at disk speed at its texture's first use: the first run through Castle Town
// Market after a restart had 38 frames over 25 ms a minute where the next had 15 and a run without the cache 12, and
// the 180 frames after an arrival in the field took 22 ms each instead of 18 (2026-11-13). The count's thread reads
// them at the lowest priority, in the listing's order: REKARINA_TEX_CACHE_WARM_MB at most (default 8192, less where the
// machine has under 22 GiB of memory, see tex_cache_scan; 0: none). It
// gives up when the disk gives less than 150 MiB a second over the first 256 MiB: a hard disk, where these reads
// would hold up the game's own loading.
typedef struct { const char *dir; uint8_t *buf; int64_t done, limit; uint64_t t0; int files, slow; } TexCacheWarm;
static int tex_cache_warm_one(const char *name, void *ctx) {
    TexCacheWarm *w = ctx;
    char path[700];
    snprintf(path, sizeof path, "%s/%s", w->dir, name);
    PlatFile *f = plat_file_open(path);
    if (!f) return 1;
    for (uint64_t off = 0;;) {
        int64_t n = plat_file_pread(f, w->buf, 1u << 20, off);
        if (n <= 0) break;
        off += (uint64_t)n, w->done += n;
    }
    plat_file_close(f);
    w->files++;
    if (w->done >= w->limit) return 0;
    if (!w->slow && w->done >= (256ll << 20)) {   // judged once
        double sec = (double)(plat_now_ns() - w->t0) / 1e9;
        w->slow = (double)w->done / 1048576.0 < 150.0 * sec ? 1 : -1;
        if (w->slow > 0) return 0;
    }
    return 1;
}

static void tex_cache_scan(void *arg) {
    TexCacheScan s = {.dir = arg, .limit = tc_limit};
    plat_file_list(s.dir, ".bc3", tex_cache_count, &s);
    __atomic_store_n(&tc_disk, s.total, __ATOMIC_RELEASE);
    rt_log("vk: texture cache: %d textures, %.2f GiB of at most %.2f%s", s.files, s.total / 1073741824.0,
           tc_limit / 1073741824.0, s.deleted ? "; files over the limit deleted" : "");
    // The default leaves the file cache what the machine has beyond the game: the game's working set is about 13 GiB
    // (2026-11-14), so with 16 GiB of memory 8 GiB of files could only push the game's own pages out.
    const char *wm = cfg_env("REKARINA_TEX_CACHE_WARM_MB");
    float ram_used, ram_total, ram_proc;
    plat_mem_info(&ram_used, &ram_total, &ram_proc);
    int warm_mb = ram_total < 0 ? 8192 : ram_total < 14336 + 512 ? 0 : ram_total > 14336 + 8192 ? 8192 : (int)ram_total - 14336;
    TexCacheWarm w = {.dir = s.dir, .limit = (int64_t)(wm && *wm ? atoi(wm) : warm_mb) << 20, .t0 = plat_now_ns()};
    if (w.limit <= 0 || !s.files || !(w.buf = malloc(1u << 20))) return;
    plat_thread_priority(PLAT_PRIO_LOWEST);
    plat_file_list(s.dir, ".bc3", tex_cache_warm_one, &w);
    free(w.buf);
    rt_log("vk: texture cache: %d files, %.2f GiB, read into the system's file cache in %.1f s%s", w.files,
           (double)w.done / 1073741824.0, (double)(plat_now_ns() - w.t0) / 1e9,
           w.slow > 0 ? " and left there: the disk gave under 150 MiB a second" : "");
}

static const char *tex_cache_dir(void) {
    static char dir[512];
    static int state;   // 0 not asked, 1 on, -1 off
    if (!state) {
        const char *d = cfg_env("REKARINA_TEX_CACHE_DIR"), *mb = cfg_get("REKARINA_TEX_CACHE_MB");
        state = cfg_bool("REKARINA_TEX_CACHE") ? 1 : -1;
        // cfg_get gives no table default: unset, the limit was 6144 MiB whatever the table said, and a folder that had
        // reached it took no new texture (2026-11-13)
        tc_limit = (int64_t)(mb && atoi(mb) >= 256 ? atoi(mb) : 14336) << 20;
        if (state > 0) {
            if (d && *d) snprintf(dir, sizeof dir, "%s", d);
            else {
                char exe[400] = "";
                plat_exe_path(exe, sizeof exe);
                char *cut = strrchr(exe, '/'), *cut2 = strrchr(exe, 92);
                if (cut2 > cut) cut = cut2;
                if (cut) *cut = 0;
                snprintf(dir, sizeof dir, "%s/tex-cache", exe);
            }
            plat_file_mkdir_p(dir);
            rt_log("vk: texture cache on: BC3 blocks of ASTC textures in %s (tex_cache)", dir);
            if (!plat_thread_spawn(tex_cache_scan, dir, "texture cache", 0)) __atomic_store_n(&tc_disk, 0, __ATOMIC_RELEASE);
        }
    }
    return state > 0 ? dir : NULL;
}

static void tex_cache_path(char *out, size_t n, const Tex *e, uint64_t hash, uint32_t levels, uint32_t layers) {
    const nvn_texdesc *d = &e->t.d;
    snprintf(out, n, "%s/%016llx-%x-%ux%ux%u-%ux%u.bc3", tex_cache_dir(), (unsigned long long)hash, (unsigned)d->format,
             d->width, d->height, d->depth, levels, layers);
}

// The reaper thread, after the GPU wrote the blocks: to a file of its own name first, then renamed, so a reader never
// sees half of one.
static void tex_cache_write(void *arg) {
    TexCacheJob *j = arg;
    VkCtx *c = vk_ctx();
    if (j->keep && c) {
        vmaInvalidateAllocation(c->vma, j->alloc, 0, VK_WHOLE_SIZE);
        char tmp[640];
        snprintf(tmp, sizeof tmp, "%s.%u.tmp", j->path, (unsigned)plat_thread_id());
        FILE *f = fopen(tmp, "wb");
        int ok = f && fwrite(j->map, 1, (size_t)j->size, f) == j->size;
        if (f) ok = fclose(f) == 0 && ok;
        if (ok && rename(tmp, j->path) == 0) plat_inc64(&tc_saved), plat_add64(&tc_disk, (int64_t)j->size);
        else remove(tmp), plat_inc64(&tc_failed);   // (a texture with the same bytes got there first: rename does not replace)
    }
    if (c) vmaDestroyBuffer(c->vma, j->buf, j->alloc);
    free(j);
}

// 1: the image was filled from the cache's file (one copy in the upload batch).
// Back under the texture lock after a fill. 0 when the entry went or was uploaded by another thread meanwhile (the
// caller drops what it filled). A destroyed entry's record is freed once the GPU is past the submission that was being
// recorded (destroy_tex), which a file read can outlast, so e is not read before the table says its key still names
// it; the image is compared too, for an entry made again at the same address in the same record.
static int fill_leave(const TexKey *k, const Tex *e, VkImage image, int was_uploaded) {
    lock();
    plat_mutex_unlock(&fill_lock);
    return nvn_hm_get(&by_key, k) == e && !e->pub.gone && e->pub.image == image && e->uploaded == was_uploaded;
}

// *skip (the worker only): the entry went or was uploaded by the replay while the file was read; nothing was recorded.
static int tex_cache_load(Tex *e, const char *path, const VkBufferImageCopy *rg, uint32_t n, uint64_t total, int *skip) {
    VkBuffer sb = VK_NULL_HANDLE;
    VkCommandBuffer cb;
    int ok = 0;
    if (up_worker) {
        int was = e->uploaded;
        TexKey key = e->key;
        VkImage image = e->pub.image;
        VmaAllocation sa = NULL;
        fill_enter();
        FILE *f = fopen(path, "rb");
        if (f) {
            uint8_t *stg = vk_staging(total, &sb, &sa);
            ok = fread(stg, 1, (size_t)total, f) == total && fgetc(f) == EOF;
            fclose(f);
        }
        int alive = fill_leave(&key, e, image, was);
        if (f && !alive) vmaDestroyBuffer(vk_ctx()->vma, sb, sa);
        if (!alive) *skip = 1;
        if (!f || !alive) return 0;
        cb = vk_upload_begin();
        owned_staging_later(sb, sa);   // after the batch that copies from it is open: the deferral is stamped with it
    } else {
        FILE *f = fopen(path, "rb");
        if (!f) return 0;
        cb = vk_upload_begin();
        uint8_t *stg = vk_staging(total, &sb, NULL);
        ok = fread(stg, 1, (size_t)total, f) == total && fgetc(f) == EOF;
        fclose(f);
    }
    if (ok) {
        barrier(cb, ALL_RW, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        vkCmdCopyBufferToImage(cb, sb, e->pub.image, VK_IMAGE_LAYOUT_GENERAL, n, rg);
        barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, ALL_RW);
        plat_inc64(&tc_loaded);
        plat_add64(&tc_load_bytes, (int64_t)total);
    } else {
        remove(path);   // another size than this texture's blocks: not ours, or cut short
        plat_inc64(&tc_failed);
    }
    vk_upload_end();
    return ok;
}

static void upload(Tex *e) {
    static volatile int32_t n_bad, n_undecoded, n_split;
    // the worker reads the storage with the texture lock released, when e may go: it reads through copies
    tex_t t_own;
    const tex_t *t = &e->t;
    if (up_worker) t_own = e->t, t = &t_own;
    const nvn_texdesc *d = &t->d;
    const TexKey key0 = e->key;
    const VkImage image0 = e->pub.image;
    int bw, bh, bpb = nvn_fmt_bpb(d->format, &bw, &bh);
    if (!bpb || !t->mem) return;
    vk_dbg_r8("upload", t, (long)e->pub.levels, 0);   // DEBUG-R8LOG
    uint64_t p0 = perf_begin(), u0 = kern_now_ns(), sec0 = sec_begin();
    edge_keep(e);
    int linear = nvn_tex_is_linear(d), is3d = d->target == NVN_TEXTURE_TARGET_3D, one_d = e->pub.type == VK_IMAGE_TYPE_1D;
    uint32_t levels = linear ? 1 : e->pub.levels, layers = linear ? 1 : e->pub.layers;
    int astc = e->f.astc_bw != 0, rgb = e->f.expand_rgb;
    int agpu = astc && vk_astc_gpu(), acheck = agpu && vk_astc_mode() == VK_ASTC_CHECK;   // raw blocks to the GPU
    uint32_t n = levels * layers, k = 0;
    VkBufferImageCopy *rg = calloc(n, sizeof *rg);
    VkAstcRegion *ar = agpu ? calloc(n, sizeof *ar) : NULL;
    VkDeviceSize total = 0, decoded = 0;
    for (uint32_t lv = 0; lv < levels; lv++)
        for (uint32_t ly = 0; ly < layers; ly++) {
            uint32_t w = mip(d->width, lv), h = one_d ? 1 : mip(d->height, lv), sl = is3d ? mip(d->depth, lv) : 1;
            uint64_t texels = (uint64_t)w * h * sl, raw = (uint64_t)(w + bw - 1) / bw * bpb * ((h + bh - 1) / bh) * sl;
            uint64_t out = agpu ? raw : astc ? texels * 4 : rgb ? texels * (uint64_t)bpb / 3 * 4 : raw;
            rg[k++] = (VkBufferImageCopy){.bufferOffset = total, .imageSubresource = {e->pub.aspect, lv, ly, 1},
                                          .imageExtent = {w, h, sl}};
            total += (out + 15) & ~15ull;
            decoded += texels * 4;
        }
    VkBuffer sb, rbuf = VK_NULL_HANDLE;
    VmaAllocation ralloc = NULL;
    uint8_t *rmap = NULL, *raw_copy = NULL;
    int checked = 0, parts = 1;
    // the texture cache: BC3 images the GPU would decode and encode, by the hash of their storage
    TexCacheJob *tcj = NULL;
    uint64_t tc_hash = 0;
    int tc_done = 0, w = up_worker, was = e->uploaded, skip = 0;
    if (agpu && e->f.astc_bc && !acheck && !e->uploaded && t->L.size >= 4096 &&
        tex_cache_dir()) {
        // the entry's own hash where it was just taken (adopt_released): the same function over the same bytes
        tc_hash = e->has_chash ? e->chash : content_hash((const uint8_t *)(uintptr_t)t->mem, t->L.size);
        VkBufferImageCopy *br = calloc(n, sizeof *br);
        uint64_t bc_total = 0;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t w = rg[i].imageExtent.width, h = rg[i].imageExtent.height, sl = rg[i].imageExtent.depth;
            br[i] = rg[i];
            br[i].bufferOffset = bc_total, br[i].bufferRowLength = (w + 3) / 4 * 4, br[i].bufferImageHeight = (h + 3) / 4 * 4;
            bc_total += vk_astc_bc_bytes(w, h, sl);
        }
        char path[600];
        tex_cache_path(path, sizeof path, e, tc_hash, levels, layers);
        tc_done = tex_cache_load(e, path, br, n, bc_total, &skip);
        free(br);
        int64_t on_disk = __atomic_load_n(&tc_disk, __ATOMIC_ACQUIRE);
        if (!tc_done && !w && on_disk >= 0 && on_disk + (int64_t)bc_total <= tc_limit && (tcj = calloc(1, sizeof *tcj))) {
            snprintf(tcj->path, sizeof tcj->path, "%s", path);
            tcj->size = bc_total;
            tcj->map = vk_staging(bc_total, &tcj->buf, &tcj->alloc);
        }
    }
    // A CPU decode or copy stages at most UPLOAD_PART bytes per staging buffer (a region larger than that alone) and
    // records one copy per buffer; every part stays in the same upload batch, submitted as before. The GPU decoder
    // takes the raw blocks of every region from one buffer.
    // The worker leaves every ASTC decode to the replay thread (the GPU decoder's recording is the replay's, and the
    // CPU decoder reads the entry while it runs), and anything it lost a race for.
    if (w && !tc_done && (astc || acheck)) skip = 1;
    for (uint32_t k0 = 0, k1; k0 < n && !tc_done && !skip; k0 = k1) {
        VkDeviceSize base = rg[k0].bufferOffset, size;
        for (k1 = k0 + 1; k1 < n && (agpu || (k1 + 1 < n ? rg[k1 + 1].bufferOffset : total) - base <= UPLOAD_PART);) k1++;
        size = (k1 < n ? rg[k1].bufferOffset : total) - base;
        if (k0) parts++;
        VmaAllocation sa = NULL;
        VkCommandBuffer cb = w ? VK_NULL_HANDLE : vk_upload_begin();
        uint8_t *stg = vk_staging(size, &sb, w ? &sa : NULL);
        if (acheck) rmap = vk_staging(decoded, &rbuf, &ralloc);
        if (w) fill_enter();   // the copy out of the game's memory, with the texture lock released
        for (k = k0; k < k1; k++) {
            uint32_t lv = rg[k].imageSubresource.mipLevel, ly = rg[k].imageSubresource.baseArrayLayer;
            uint32_t w = rg[k].imageExtent.width, h = rg[k].imageExtent.height, sl = rg[k].imageExtent.depth;
            uint32_t rb = (w + bw - 1) / bw * (uint32_t)bpb, rows = (h + bh - 1) / bh;
            size_t raw = (size_t)rb * rows * sl;
            rg[k].bufferOffset -= base;
            uint8_t *dst = stg + rg[k].bufferOffset, *tmp = (astc && !agpu) || rgb ? malloc(raw) : NULL;
            uint64_t sec1 = sec_begin();
            int swz_bad = nvn_deswizzle(d, lv, ly, t->mem, tmp ? tmp : dst, rb);
            sec_end(raw < (64u << 10) ? SEC_DESW_S : raw < (1u << 20) ? SEC_DESW_M : raw < (4u << 20) ? SEC_DESW_L : SEC_DESW_XL, sec1);
            if (swz_bad) {
                log_limited(&n_bad, "texture %#llx fmt %#x target %d: level %u layer %u has no CPU layout; uploaded as zeros",
                            (unsigned long long)(uintptr_t)t->mem, d->format, d->target, lv, ly);
                memset(tmp ? tmp : dst, 0, raw);
            }
            if (agpu) {
                ar[k] = (VkAstcRegion){.src = rg[k].bufferOffset, .w = w, .h = h, .slices = sl, .level = lv, .layer = ly};
            } else if (astc) {
                uint64_t a0 = perf_begin();
                for (uint32_t z = 0; z < sl; z++)
                    astc_decode_ldr_mt(tmp + (size_t)z * rb * rows, w, h, e->f.astc_bw, e->f.astc_bh, e->f.astc_srgb,
                                       dst + (size_t)z * w * h * 4);
                perf_end(PERF_ASTC, a0);
                rg[k].bufferRowLength = w, rg[k].bufferImageHeight = h;
            } else if (rgb) {
                expand_rgb(d->format, tmp, dst, (size_t)w * h * sl);
                rg[k].bufferRowLength = w, rg[k].bufferImageHeight = h;
            } else {
                rg[k].bufferRowLength = rb / (uint32_t)bpb * (uint32_t)bw, rg[k].bufferImageHeight = rows * (uint32_t)bh;
            }
            free(tmp);
        }
        if (acheck) {
            raw_copy = malloc(size);
            memcpy(raw_copy, stg, size);
        }
        if (w) {
            if (!fill_leave(&key0, e, image0, was)) {
                vmaDestroyBuffer(vk_ctx()->vma, sb, sa);
                skip = 1;
                break;
            }
            cb = vk_upload_begin();
            owned_staging_later(sb, sa);   // after the batch that copies from it is open: the deferral is stamped with it
        }
        barrier(cb, ALL_RW, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        if (agpu) {
            uint64_t a0 = perf_begin();
            if (tcj) vk_astc_bc_readback(tcj->buf);
            checked = vk_astc_record(cb, sb, size, ar, (int)n, e->f.astc_bw, e->f.astc_bh, e->f.astc_srgb, e->f.astc_bc,
                                     e->pub.image, e->pub.aspect, rbuf);
            perf_end(PERF_ASTC, a0);
            if (!checked)
                log_limited(&n_undecoded, "ASTC texture %#llx (fmt %#x %ux%u) not decoded on the GPU: left as it was",
                            (unsigned long long)e->pub.addr, d->format, d->width, d->height);
        } else {
            vkCmdCopyBufferToImage(cb, sb, e->pub.image, VK_IMAGE_LAYOUT_GENERAL, k1 - k0, rg + k0);
        }
        barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, ALL_RW);
        vk_upload_end();
    }
    if (tcj) {   // written once the GPU is past this batch; not kept when the decode failed or the game wrote meanwhile
        tcj->keep = checked && content_hash((const uint8_t *)(uintptr_t)t->mem, t->L.size) == tc_hash;
        vk_defer_call(tex_cache_write, tcj);
    }
    if (tc_hash && !((tc_loaded + tc_saved) & 255) && tc_loaded + tc_saved)
        rt_log("vk: texture cache: %lld textures filled from it (%.2f GiB), %lld written to it, %lld failed",
               (long long)tc_loaded, tc_load_bytes / 1073741824.0, (long long)tc_saved, (long long)tc_failed);
    if (parts > 1)
        log_limited(&n_split, "texture %#llx fmt %#x %ux%u, %u levels, %u layers: %.1f MiB staged in %d parts",
                    (unsigned long long)e->pub.addr, d->format, d->width, d->height, levels, layers, total / 1048576.0,
                    parts);
    if (acheck) {   // waits for the GPU
        vk_upload_flush(1);
        if (checked) {
            vmaInvalidateAllocation(vk_ctx()->vma, ralloc, 0, VK_WHOLE_SIZE);
            astc_check(e, raw_copy, ar, (int)n, rmap);
        }
        vmaDestroyBuffer(vk_ctx()->vma, rbuf, ralloc);
    }
    free(raw_copy);
    free(ar);
    free(rg);
    if (skip) return;   // the worker: left to the replay thread, the entry is as it was
    uint64_t dt = kern_now_ns() - u0;
    if (e->uploaded) {
        e->has_chash = 0;   // the image no longer holds the bytes that were hashed (adopt_released)
        n_reuploads++, perf_count(PERF_CNT_TEX_REUP);
        ru_n++, ru_bytes += t->L.size, ru_ns += dt;
        if (e->wt_n) {
            ru_wt_n++, ru_wt_bytes += t->L.size, ru_wt_ns += dt;
            if (rt_trace & RT_TRACE_PERF)
                rt_log("perf: frame %llu: re-upload after %u WriteTexels: texture %#llx fmt %#x %ux%u, %.1f KiB, %.3f ms",
                       (unsigned long long)__atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED), e->wt_n,
                       (unsigned long long)e->pub.addr, d->format, d->width, d->height, t->L.size / 1024.0, dt / 1e6);
        }
    } else {
        n_uploads++, perf_count(PERF_CNT_TEX_UP);
    }
    e->wt_n = 0;
    e->uploaded = 1;
    e->n_up++;
    perf_end(PERF_TEX, p0);
    sec_end(SEC_UP_TOTAL, sec0);
}

// Whether e's storage fills no 4 KiB page of its own, so the write tracking can only see writes to pages it shares.
static int fills_no_page(const Tex *e) {
    return ((e->pub.addr + e->t.L.size) & ~0xfffull) <= ((e->pub.addr + 0xfff) & ~0xfffull);
}

// A texture that fills no page keeps a copy of its storage bytes from when a GPU write to it was recorded
// (vk_texture_gpu_written), up to SNAP_MAX bytes; wb_exact (REKARINA_WB_EXACT, default on) enables it.
enum { SNAP_MAX = 4096 };
static int wb_exact = -1;
static volatile int32_t n_kept;

// vk_mem_changed over the pages wholly inside e's storage (all of its pages when it fills none): a neighbour that shares
// the first or last page does not count as a write to e. A texture that fills no page shares all of its pages, so
// there a page write counts only when e's own bytes differ from the copy taken at the GPU write. TotK's tablet
// camera renders the ID, screen rect and depth of what is under the reticle into 4x4 targets of 512 bytes, two
// slots used in turn, and the CPU reads them a frame later. One slot's rect target shares its page with data the CPU
// writes every frame: without the copy its renders were never written back, the game read a zero rect every other
// frame and the target's name label blinked. REKARINA_WB_EXACT=0 restores the page-only decision.
static int storage_rewritten(const Tex *e, uint32_t since, uint32_t *now) {
    uint64_t lo = (e->pub.addr + 0xfff) & ~0xfffull, hi = (e->pub.addr + e->t.L.size) & ~0xfffull;
    if (hi <= lo) lo = e->pub.addr, hi = e->pub.addr + e->t.L.size;
    int ch = vk_mem_changed(lo, hi - lo, since, now);
    if (ch != 1 || !since || !e->snap || memcmp(e->snap, (const void *)(uintptr_t)e->pub.addr, e->t.L.size)) return ch == 1;
    log_limited(&n_kept, "write-back of %#llx kept: fmt %#x %ux%u, %llu bytes; a neighbour wrote its page",
                (unsigned long long)e->pub.addr, e->t.d.format, e->t.d.width, e->t.d.height,
                (unsigned long long)e->t.L.size);
    return 0;
}

// The page check cannot tell a texture's bytes from its neighbours' in the first and last page of its storage. OoT's
// texture compaction moves blocks of 10 to 13 MiB every few frames in the Market, and whenever
// one landed beside the 8-layer R16 array its lighting reads (1775x1175, 32 MiB) that array was uploaded again from
// its storage, every 18 frames or so, although nobody had written it: 3 ms of copying each time, and one frame in
// three or four of those came out a third darker (the upload's frame; the flight recorder's flash watch counted 21 in
// 95 s, 2026-11-12). So a texture of more than two pages keeps its own bytes of those two pages from its last upload
// (edge_keep), and a write that touched only them counts when those bytes differ. REKARINA_TEX_EDGE=0: every write counts.
static int tex_edge = -1;
static uint64_t n_edge_spared;
static void edge_span(const Tex *e, uint64_t *lo, uint64_t *hi) {
    *lo = (e->pub.addr + 0xfff) & ~0xfffull, *hi = (e->pub.addr + e->t.L.size) & ~0xfffull;
}
static void edge_keep(Tex *e) {   // before the upload reads the storage
    uint64_t lo, hi;
    edge_span(e, &lo, &hi);
    if (tex_edge < 0) tex_edge = rt_knob("TEX_EDGE", "1");
    if (!tex_edge || hi <= lo + 0x2000) return;
    uint64_t head = lo - e->pub.addr, tail = e->pub.addr + e->t.L.size - hi;
    if (!e->edge && !(e->edge = malloc(0x2000))) return;
    memcpy(e->edge, (const void *)(uintptr_t)e->pub.addr, head);
    memcpy(e->edge + 0x1000, (const void *)(uintptr_t)hi, tail);
}
// 1: the write the page check reported (since the generation `since`) left e's own bytes as they were uploaded.
static int edge_only(const Tex *e, uint32_t since, int snap) {
    uint64_t lo, hi;
    edge_span(e, &lo, &hi);
    if (!tex_edge || !e->edge || hi <= lo + 0x2000) return 0;
    uint32_t now;
    int in = snap ? vk_mem_changed_snap(lo, hi - lo, since, submit_epoch, &now) : vk_mem_changed(lo, hi - lo, since, &now);
    if (in) return 0;   // written inside, or not tracked
    uint64_t head = lo - e->pub.addr, tail = e->pub.addr + e->t.L.size - hi;
    if (memcmp(e->edge, (const void *)(uintptr_t)e->pub.addr, head) || memcmp(e->edge + 0x1000, (const void *)(uintptr_t)hi, tail)) return 0;
    n_edge_spared++;
    return 1;
}

// A mirrored texture is uploaded on first use and again when its storage pages carry a newer write generation.
static void make_fresh(Tex *e) {
    if (!e->pub.mirrored || e->pub.gpu_owned) return;
    uint64_t size = e->t.L.size;
    if (!e->uploaded) {
        vk_mem_changed(e->pub.addr, size, 0, &e->wgen);   // resets the write bits before the storage is read
        upload(e);
        return;
    }
    uint32_t now, since = e->wgen;
    int ch = vk_mem_changed(e->pub.addr, size, e->wgen, &now);
    e->wgen = now;
    if (ch == 1 && edge_only(e, since, 0)) ch = 0;
    e->last_ch = ch;
    if (ch) upload(e);   // 1 written, -1 not tracked: both upload again
}

// make_fresh for draw bindings: the page check answers from this submission's write-watch snapshot (vk_memory.c
// vk_mem_changed_snap) instead of a system call per texture. A write the snapshot of the texture's region missed (made
// after it, during this replay) keeps its write-watch bit and is caught by the next submission's snapshot.
static void make_fresh_snap(Tex *e) {
    if (!e->pub.mirrored || e->pub.gpu_owned) return;
    uint32_t now, since = e->wgen;
    n_checks++;
    int ch = vk_mem_changed_snap(e->pub.addr, e->t.L.size, e->uploaded ? e->wgen : 0, submit_epoch, &now);
    e->wgen = now;
    if (!e->uploaded) { upload(e); return; }
    if (ch == 1 && edge_only(e, since, 1)) ch = 0;
    e->last_ch = ch;
    if (ch) upload(e);
}

// ---------------------------------------------------------------- aliases (REKARINA_TEX_ALIAS)
// Bytes per block and block size of the image's VkFormat (ASTC decodes to RGBA8, RGB expands to RGBA); 0 when the
// VkFormat substitutes another one of unknown size.
static int img_block(const Tex *e, int *bw, int *bh) {
    if (e->f.astc_bc) { *bw = *bh = 4; return 16; }
    if (e->f.astc_bw) { *bw = *bh = 1; return 4; }
    int b = nvn_fmt_bpb(e->t.d.format, bw, bh);
    if (e->f.expand_rgb) { *bw = *bh = 1; return b / 3 * 4; }
    return e->f.vk == e->f.ideal ? b : 0;
}

// The new entry e can take src's content texel for texel: colour, one sample, the same shape and storage layout, and
// image formats of one block size.
static int alias_compatible(const Tex *e, const Tex *src) {
    const VkTexImage *p = &e->pub, *q = &src->pub;
    if (e->f.depth || e->f.stencil || src->f.depth || src->f.stencil || p->samples > 1 || q->samples > 1) return 0;
    if (p->type != q->type || p->width != q->width || p->height != q->height || p->depth != q->depth) return 0;
    if (e->t.L.size != src->t.L.size || nvn_tex_is_linear(&e->t.d) != nvn_tex_is_linear(&src->t.d)) return 0;
    int ew, eh, sw, sh, eb = img_block(e, &ew, &eh), sb = img_block(src, &sw, &sh);
    return eb && eb == sb && ew == sw && eh == sh;
}

// src holds content the storage bytes do not: the GPU wrote it and, for a mirrored texture, it was not written back
// and the CPU has not rewritten the storage since (the new entry's upload then reads the newer bytes).
static int alias_has_gpu_content(const Tex *src) {
    if (!src->pub.gpu_owned) return 0;
    if (!src->pub.mirrored) return 1;
    uint32_t now;
    return src->gpu_dirty && !storage_rewritten(src, src->gpu_gen, &now);
}

// Maxwell reads the same bytes through both descriptors: src's content goes into the new image e at the replay's
// current position, before any later command of the submission uses e. Through a buffer, since bound images share
// their memory and image copies may not overlap; e leaves UNDEFINED between the two halves, so its first transition
// cannot clobber src's bytes before they are read. 0 when no replay is recording (e is then not initialised yet).
static int alias_copy(Tex *e, const Tex *src) {
    int reopen;
    VkCommandBuffer cb = vk_exec_inline_begin(&reopen);
    if (!cb) return 0;
    int bw, bh, bpb = img_block(e, &bw, &bh);
    uint32_t levels = e->pub.levels < src->pub.levels ? e->pub.levels : src->pub.levels;
    uint32_t layers = e->pub.layers < src->pub.layers ? e->pub.layers : src->pub.layers, n = levels * layers, k = 0;
    VkBufferImageCopy *rg = calloc(n, sizeof *rg);
    if (!rg) rt_fatal("vk: out of memory for an alias copy");
    VkDeviceSize total = 0;
    for (uint32_t lv = 0; lv < levels; lv++)
        for (uint32_t ly = 0; ly < layers; ly++, k++) {
            uint32_t w = mip(e->pub.width, lv), h = mip(e->pub.height, lv), z = mip(e->pub.depth, lv);
            rg[k] = (VkBufferImageCopy){.bufferOffset = total, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, lv, ly, 1},
                                        .imageExtent = {w, h, z}};
            total += ((VkDeviceSize)(w + bw - 1) / bw * ((h + bh - 1) / bh) * z * bpb + 15) & ~15ull;
        }
    VkBuffer buf = vk_exec_inline_buffer(total);
    if (!buf) {
        free(rg);
        vk_exec_inline_end(reopen);
        return 0;
    }
    vk_crumb(cb, VK_CRUMB_ALIAS, src->pub.addr >> 12);   // DEBUG-COPYCRUMB
    barrier(cb, ALL_RW, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    vkCmdCopyImageToBuffer(cb, src->pub.image, VK_IMAGE_LAYOUT_GENERAL, buf, n, rg);
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                           .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                           .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT};
    VkImageMemoryBarrier2 ib = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                                .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT, .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .image = e->pub.image,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb,
                           .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &ib};
    vkCmdPipelineBarrier2(cb, &di);
    vkCmdCopyBufferToImage(cb, buf, e->pub.image, VK_IMAGE_LAYOUT_GENERAL, n, rg);
    barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, ALL_RW);
    vk_crumb(cb, VK_CRUMB_ALIAS_END, src->pub.addr >> 12);   // DEBUG-COPYCRUMB
    vk_exec_inline_end(reopen);
    free(rg);
    return 1;
}

// DEBUG-TEXWATCH: REKARINA_TEXWATCH=<hex addr>,... logs from frame REKARINA_TEXWATCH_FROM (default 4400) which entry and
// VkImage each lookup of a texture at those addresses resolves to (600 lines at most)
static void tex_watch(const char *where, const TexKey *k, const Tex *e) {
    static int n = -1, nl;
    static uint64_t w[8], from;
    if (n < 0) {
        n = 0;
        const char *s = cfg_get("REKARINA_TEXWATCH"), *f = cfg_get("REKARINA_TEXWATCH_FROM");
        from = f && *f ? strtoull(f, NULL, 0) : 4400;
        for (char *x; s && *s && n < 8; s = *x == ',' ? x + 1 : x) {
            w[n++] = strtoull(s, &x, 16);
            if (x == s) break;
        }
    }
    if (!n || nl >= 600) return;
    const char *wk = cfg_get("REKARINA_TEXWATCH_KEYS");   // =stale: only lookups that ended on an entry older than a live one of its format
    if (e && wk && !strcmp(wk, "stale")) {
        int stale = 0;
        for (const Tex *p = nvn_hm_get(&by_addr, &k->addr); p && !stale; p = p->next_at)
            stale = p != e && !p->released && p->key.d.format == e->key.d.format && wr_at(p) > wr_at(e);
        if (!stale) return;
    }
    int hit = 0;
    for (int i = 0; i < n; i++) hit |= w[i] == k->addr;
    uint64_t fr = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    if (!hit || fr < from) return;
    int live = 0, all = 0;
    for (const Tex *p = nvn_hm_get(&by_addr, &k->addr); p; p = p->next_at) all++, live += !p->released;
    nl++;
    rt_log("TEXWATCH frame %llu %s addr %#llx key fmt %#x flags %#x %ux%u -> entry %p image %p fmt %#x flags %#x%s%s own %d "
           "gpu_owned %d mirrored %d dirty %d run %u pool_flags %#x; entries at addr %d live %d", (unsigned long long)fr, where,
           (unsigned long long)k->addr,
           k->d.format, k->d.flags, k->d.width, k->d.height, (const void *)e, e ? (void *)e->pub.image : NULL,
           e ? e->key.d.format : 0, e ? e->key.d.flags : 0, e && e->released ? " RELEASED" : "",
           e && memcmp(&e->key, k, sizeof *k) ? " (other key)" : "", e ? e->alloc != NULL : -1,
           e ? e->pub.gpu_owned : -1, e ? e->pub.mirrored : -1, e ? e->gpu_dirty : -1, e ? (unsigned)e->run : 0u,
           e ? (unsigned)e->t.pool_flags : 0u, all, live);
    if (cfg_get("REKARINA_TEXWATCH_KEYS"))   // every entry at the address with its whole key and its write order
        for (const Tex *p = nvn_hm_get(&by_addr, &k->addr); p; p = p->next_at)
            rt_log("TEXWATCH   %s%p tgt %d fmt %#x flags %#x %ux%ux%u lv %u smp %d stride %u bh %d layout %d %02x%02x%02x%02x "
                   "wr %llu (as of %llu) gpu_owned %d mirrored %d%s", p == e ? "> " : "  ", (const void *)p, p->key.d.target,
                   p->key.d.format, p->key.d.flags, p->key.d.width, p->key.d.height, p->key.d.depth, p->key.d.levels,
                   p->key.d.samples, p->key.d.stride, p->key.d.pkg_block_h, p->key.has_layout, p->key.layout[0],
                   p->key.layout[1], p->key.layout[2], p->key.layout[3], (unsigned long long)p->wr_serial,
                   (unsigned long long)wr_at(p), p->pub.gpu_owned, p->pub.mirrored, p->released ? " RELEASED" : "");
}

// DEBUG-ALIASLOG: REKARINA_ALIASLOG=<from frame>: every new entry at an address that already has entries (800 lines at
// most): the new key, each entry there and what became of it (released ones are replaced, a live one may be the
// content source), and whether the new image got content. For the whole-scene black frames (flight.c flash watch).
static int alias_log_on(void) {
    static int on = -1;
    static uint64_t from;
    if (on < 0) { const char *e = cfg_get("REKARINA_ALIASLOG"); on = e && *e; from = on ? strtoull(e, NULL, 0) : 0; }
    return on && __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) >= from;
}
static void alias_log(const char *what, const TexKey *k, const Tex *p) {
    static int n, only = -1;
    // REKARINA_ALIASLOG_EMPTY=1: only the new images that got no content or a copy, without the square target whose size
    // changes every frame while the camera turns; 6000 lines
    if (only < 0) only = cfg_get("REKARINA_ALIASLOG_EMPTY") ? atoi(cfg_get("REKARINA_ALIASLOG_EMPTY")) : 0;
    // =2: also the released entries a new one replaces and the new ones that take a released one's image
    if (only && (!(strstr(what, "EMPTY") || strstr(what, "copied") || strstr(what, "storage") ||
                   (only == 2 && (strstr(what, "replaces") || strstr(what, "released one")))) ||
                 (k->d.width == k->d.height && k->d.width > 512 && k->d.format == 0x34)))
        return;
    if (n >= (only ? 6000 : 800)) return;
    n++;
    rt_log("ALIASLOG frame %llu %s addr %#llx new %ux%u fmt %#x flags %#x | entry %ux%u fmt %#x flags %#x%s gpu_owned %d "
           "dirty %d mirrored %d", (unsigned long long)__atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED), what,
           (unsigned long long)k->addr, k->d.width, k->d.height, k->d.format, k->d.flags, p ? p->key.d.width : 0,
           p ? p->key.d.height : 0, p ? p->key.d.format : 0, p ? p->key.d.flags : 0, p && p->released ? " RELEASED" : "",
           p ? p->pub.gpu_owned : -1, p ? p->gpu_dirty : -1, p ? p->pub.mirrored : -1);
}

static Tex *entry_of(const tex_t *t) {
    TexKey k;
    make_key(t, &k);
    Tex *e = nvn_hm_get(&by_key, &k);
    if (!e) {
        if (tex_alias < 0) {
            tex_alias = cfg_bool("REKARINA_TEX_ALIAS");
            rt_log("vk: texture aliases: %s", tex_alias ? "a new image takes the live image's content" : "a new image starts empty");
        }
        uint64_t sec0 = sec_begin();
        if (!(e = create_tex(t, &k, tex_alias))) return NULL;
        perf_count(PERF_CNT_TEX_NEW);
        if (cold_val(cold_del(&k))) n_cold_back++;
        // released entries of another descriptor at this address are replaced by the new one; a live one holding
        // GPU-written content is the alias source, preferring the one that differs only in swizzle (same_but_swizzle)
        int alias = 0;
        Tex *src = NULL;
        int alog = alias_log_on();
        for (Tex *p = nvn_hm_get(&by_addr, &k.addr), *nx; p; p = nx) {
            nx = p->next_at;
            if (alog) alias_log(p->released ? "replaces released" : "beside live", &k, p);
            if (p->released) { destroy_tex(p); continue; }
            alias = 1;
            // (under a limit, "lookups as of a place": an image only later passes write holds nothing yet at this place)
            if (tex_alias && alias_compatible(e, p) && alias_has_gpu_content(p) && !(asof && p->wr_serial && !wr_at(p)) &&
                (!src || wr_at(p) > wr_at(src) ||
                 (wr_at(p) == wr_at(src) && same_but_swizzle(&p->key, &k) && !same_but_swizzle(&src->key, &k))))
                src = p;   // the newest GPU writer of the storage (as slot_resolve follows it)
        }
        if (alias) perf_count(PERF_CNT_TEX_ALIAS);
        if (tex_alias) {
            if (src && !up_worker && alias_copy(e, src)) {
                n_alias_copied++;
                if (alog) alias_log("new image copied from", &k, src);
                if (e->pub.mirrored) {   // the content is src's GPU render, newer than the storage bytes
                    vk_mem_changed(e->pub.addr, e->t.L.size, 0, &e->wgen);
                    e->uploaded = 1;
                    e->pub.gpu_owned = 1;
                }
            } else if (adopt_released(e, t)) {
                if (alog) alias_log("new image is a released one with the same bytes", &k, NULL);
            } else {
                init_image(e, vk_upload_begin());
                vk_upload_end();
                if (alias) {
                    if (e->pub.mirrored) n_alias_upload++;
                    else n_alias_uninit++;
                    if (alog) alias_log(e->pub.mirrored ? "new image filled from storage" : "new image LEFT EMPTY", &k, src);
                    if (vk_dump_armed && !e->pub.mirrored && k.d.width == k.d.height && k.d.format == 0x32) {   // DEBUG-DUMPEMPTY
                        void vk_dump_arm_now(const char *why);
                        vk_dump_arm_now("the shadow layers' image at a new address");
                    }
                }
            }
        }
        e->next_at = nvn_hm_get(&by_addr, &k.addr);
        nvn_hm_put(&by_addr, &k.addr, e);
        nvn_hm_put(&by_key, &k, e);
        tex_gen_bump(k.addr);
        sec_end(SEC_MADE_T + sec_mk, sec0);
    }
    if (e->released) e->released = 0, nreleased--, rel_unlink(e), tex_gen_bump_entry(k.addr, e->texq_base);
    e->t = *t;
    touch(e);
    return e;
}

// vk_dump: upload bookkeeping of an image vk_texture_get / vk_texture_by_id returned. pages_written: whether its
// storage pages were written since the last check (1, 0, -1 not tracked); the query leaves that check's state alone.
void vk_texture_debug(const VkTexImage *img, uint32_t *uploads, int *last_change, int *pages_written) {
    lock();
    const Tex *e = (const Tex *)img;   // pub is the first member
    uint32_t now;
    *uploads = e->n_up;
    *last_change = e->last_ch;
    *pages_written = e->pub.mirrored && e->uploaded ? vk_mem_changed(e->pub.addr, e->t.L.size, e->wgen, &now) : 0;
    unlock();
}

const VkTexImage *vk_texture_get(const tex_t *t) {
    if (!t || !t->mem) return NULL;
    lock();
    Tex *e = entry_of(t);
    if (e) make_fresh(e);
    {   // DEBUG-TEXWATCH
        TexKey k;
        make_key(t, &k);
        tex_watch("get", &k, e);
    }
    unlock();
    return e ? &e->pub : NULL;
}

// Render targets the CPU reads without NVN's invalidate (vk_texture_write_back_small); small_pending: one may be dirty.
// wb_limit: targets up to this size (bytes) always go back when the CPU waits for a fence, REKARINA_WB_FENCE_LIMIT, read
// once. A larger one goes back unless the GPU wrote it in each of the last WB_RUN frames (wb_wanted). wb_trace:
// REKARINA_TRACE=gfx, read together with it.
enum { WB_SMALL_DEFAULT = 64 << 10, WB_RUN = 16 };
static int small_pending;
static uint64_t wb_limit = WB_SMALL_DEFAULT;
static int wb_trace;
static int wb_cfg_read;

static void wb_cfg_init(void) {
    if (wb_cfg_read) return;
    int v = cfg_int("REKARINA_WB_FENCE_LIMIT");
    wb_limit = v < 0 ? 0 : (uint64_t)v;
    wb_trace = (rt_trace & RT_TRACE_GFX) != 0;
    wb_cfg_read = 1;
    if (wb_limit != WB_SMALL_DEFAULT) rt_log("vk: fence write-back limit %llu bytes", (unsigned long long)wb_limit);
}

// Whether the fence write-back takes a dirty target. A target the GPU draws every frame is scratch the CPU does not
// read: in the field three 256x256 RGBA8 targets and a 256x256 RG8 one, which cost 2.7 ms a frame to write back.
// TotK's 260x260 RG8 grass maps, which its fire spread reads after a fence wait (#132), are drawn once per change, or
// up to 9 frames in a row while a tile loads.
static int wb_wanted(const Tex *e) { return e->t.L.size <= wb_limit || e->run < WB_RUN; }

void vk_texture_gpu_written(const tex_t *t) {
    if (!t || !t->mem) return;
    {   // DEBUG-BIGLOG: REKARINA_BIGLOG=1: who writes 4096x4096 textures (the far shadow maps the fog reads), and when
        static int on = -1, k;
        if (on < 0) on = cfg_get("REKARINA_BIGLOG") != NULL;
        if (on && t->d.width >= 4096 && t->d.height >= 4096 && (k++ < 20 || !(k % 200)))
            rt_log("BIGLOG written#%d %p fmt %#x by %p (vk_texture_gpu_written at %p) frame %llu", k, (const void *)t->mem,
                   t->d.format, __builtin_return_address(0), (void *)vk_texture_gpu_written,
                   (unsigned long long)__atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED));
    }
    {   // DEBUG-LUMLOG: who writes the 1x1 RGBA32F luminance targets (caller address, look it up in rekarina.map)
        static int on = -1, k;
        if (on < 0) on = cfg_get("REKARINA_LUMLOG") != NULL;
        if (on && t->d.format == 0x2e && t->d.width == 1 && t->d.height == 1 && (k++ < 6 || !(k % 300)))
            rt_log("LUMLOG written#%d %p by %p (vk_texture_gpu_written at %p) frame %llu pool_flags %#x mirrored %d", k,
                   (const void *)t->mem, __builtin_return_address(0), (void *)vk_texture_gpu_written,
                   (unsigned long long)__atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED), (unsigned)t->pool_flags,
                   tex_mirrored(t));
    }
    vk_exec_frame_written(t);
    lock();
    Tex *e = entry_of(t);
    if (e) {
        make_fresh(e);   // a partial render keeps what the storage held before
        e->pub.gpu_owned = 1;
        uint64_t f = vk_exec_recording_fence();
        if (f > e->wr_fence) e->wr_fence = f;
        note_writer(e);
        if (e->pub.mirrored) {
            e->gpu_dirty = 1;
            storage_rewritten(e, 0, &e->gpu_gen);   // CPU writes up to here are older than this render
            if (wb_exact < 0) wb_exact = cfg_bool("REKARINA_WB_EXACT");
            if (wb_exact && !e->snap && e->t.L.size <= SNAP_MAX && fills_no_page(e) && !(e->snap = malloc(SNAP_MAX)))
                rt_fatal("vk: out of memory for a texture's storage copy");
            if (e->snap) memcpy(e->snap, (const void *)(uintptr_t)e->pub.addr, e->t.L.size);
            e->gpu_seq = ++gpu_writes;
            e->gpu_fence = vk_exec_recording_fence();
            if (mem_result_watch_on() && cfg_env("REKARINA_RESULT_WATCH_TEX") && e->t.L.size <= 65536)   // DEBUG-RESULTWATCH: who reads it before the write-back
                mem_result_watch(e->pub.addr, e->t.L.size, e->gpu_fence);
            uint64_t fr = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
            if (fr != e->run_frame || !e->run) {
                e->run = fr == e->run_frame + 1 && e->run ? (uint8_t)(e->run < WB_RUN ? e->run + 1 : WB_RUN) : 1;
                e->run_frame = fr;
            }
            wb_cfg_init();
            if (wb_wanted(e)) __atomic_store_n(&small_pending, 1, __ATOMIC_RELEASE);
            own_add(e);
        }
    }
    unlock();
}

static void release_key(const TexKey *k) {   // tex lock held
    Tex *e = nvn_hm_get(&by_key, k);
    if (e && !e->released) {
        e->released = 1;
        e->rel_frame = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
        nreleased++;
        rel_append(e);
        tex_gen_bump_entry(k->addr, e->texq_base);
        sweep_released();
    }
}

void vk_texture_release(const tex_t *t) {
    if (!t || !t->mem) return;
    TexKey k;
    make_key(t, &k);
    lock();
    release_key(&k);
    cold_released(&k);
    tex_t half;
    if (texq_mode > 0 && texq_shape(t, &half)) {   // "texture quality": the entry its slots resolved to
        make_key(&half, &k);
        release_key(&k);
        cold_released(&k);
    }
    unlock();
}

void vk_texture_range_gone(uint64_t lo, uint64_t size, VmaAllocation alloc) {
    pm_gone(lo, size, alloc, 0);
    lock();
    Tex **list = malloc(sizeof *list * (size_t)(by_key.n + 1));
    int n = 0;
    for (uint32_t i = 0; i < by_key.cap; i++) {
        Tex *e = by_key.vals[i];
        if (e && ((e->key.addr >= lo && e->key.addr - lo < size) || (alloc && e->bound_to == alloc))) list[n++] = e;
    }
    for (int i = 0; i < n; i++) destroy_tex(list[i]);
    free(list);
    if (cold_keys.n) {   // evicted textures whose storage goes with the pool
        TexKey *gone = malloc(sizeof *gone * cold_keys.n);
        uint32_t ng = 0;
        for (uint32_t i = 0; i < cold_keys.cap && gone; i++) {
            if (!cold_keys.vals[i]) continue;
            TexKey k;
            memcpy(&k, cold_keys.keys + (size_t)i * cold_keys.klen, sizeof k);
            if (k.addr >= lo && k.addr - lo < size) gone[ng++] = k;
        }
        for (uint32_t i = 0; i < ng; i++) cold_del(&gone[i]);
        free(gone);
    }
    unlock();
    up_range_gone();   // a fill in flight reads storage in the range no longer
}

// ---------------------------------------------------------------- descriptor slots
static uint64_t n_same_regs;   // vk_texture_register calls that changed nothing
uint64_t vk_texture_same_regs(void) { return __atomic_load_n(&n_same_regs, __ATOMIC_RELAXED); }
static int same_reg_on(void) {   // REKARINA_TEX_SAME_REG=0: every registration goes the full way
    static int on = -1;
    if (on < 0) on = rt_knob("TEX_SAME_REG", "1");
    return on;
}

void vk_texture_register(int id, const tex_t *t, const view_t *v) {
    if (id < 0 || id >= (1 << 20)) rt_fatal("vk: texture slot %d out of range", id);
    uint64_t sec0 = sec_begin();
    lock();
    sec_end(SEC_TEXREG_LOCK, sec0);
    if (sec0) sec_count(SEC_TEXREG, 1);
    if (id >= ntslots) {
        int n = ntslots ? ntslots : 1024;
        while (n <= id) n *= 2;
        TexSlot *s = realloc(tslots, sizeof *s * (size_t)n);
        if (!s) rt_fatal("vk: out of memory for texture slots");
        memset(s + ntslots, 0, sizeof *s * (size_t)(n - ntslots));
        tslots = s, ntslots = n;
    }
    if (!sinfo) {
        uint64_t *p = calloc(1 << 20, sizeof *p);
        if (!p) rt_fatal("vk: out of memory for texture slot tokens");
        __atomic_store_n(&sinfo, p, __ATOMIC_RELEASE);
    }
    TexSlot *s = &tslots[id];
    // The same texture and view registered again (the game rewrites much of its descriptor pool every frame): the slot
    // keeps its serial and resolved entry and slots_gen stands, so nothing that was resolved from it goes stale: the
    // pool refresh, the descriptor build skip and the shader memo all key on slots_gen. Only while that entry is live:
    // after a release, a registration is what makes a slot current again (slot_resolve's stale rule).
    // s->ce may point at a record destroy_tex let go of: it bumps the generation of the address and frees the record a
    // few submissions later, and the pointer stays in the slot. So the record is read only when it is known to be
    // there: the generation is the one it was resolved under, or (the generations share 65,536 counters, so another
    // address moves them too) it still is the entry of the slot's own key. Reading it unchecked was an access violation
    // in play once the heap had given the freed page back.
    int same = same_reg_on() && s->used && s->ce && !memcmp(&s->t, t, sizeof *t) && s->has_view == (v != NULL) &&
               (!v || !memcmp(&s->v, v, sizeof *v));
    if (same && s->cgen != agen[sinfo[id] & 0xffff]) {
        TexKey k;
        make_key(&s->t, &k);
        same = nvn_hm_get(&by_key, &k) == s->ce;
        tex_t half;
        if (!same && texq_mode > 0 && texq_shape(&s->t, &half)) {   // "texture quality": the slot resolved to the smaller one
            make_key(&half, &k);
            same = nvn_hm_get(&by_key, &k) == s->ce;
        }
    }
    if (same && !s->ce->released) {
        n_same_regs++;
        unlock();
        return;
    }
    if (cold_keys.n && s->used && s->t.mem != t->mem && t->mem && !memcmp(&s->t.d, &t->d, sizeof t->d)) {
        // the same descriptor at another address: the game moved the texture (its heap's compaction). An evicted one
        // stays evicted there, or the pool would make it resident again at every move.
        TexKey ko, kn;
        tex_t ho, hn;
        make_key(&s->t, &ko), make_key(t, &kn);
        void *kind = cold_or_just_gone(&ko);
        if (kind && !nvn_hm_get(&by_key, &kn)) cold_put(&kn, kind);
        if (texq_mode > 0 && texq_shape(&s->t, &ho) && texq_shape(t, &hn)) {   // "texture quality": the smaller one's key
            make_key(&ho, &ko), make_key(&hn, &kn);
            kind = cold_or_just_gone(&ko);
            if (kind && !nvn_hm_get(&by_key, &kn)) cold_put(&kn, kind);
        }
    }
    s->t = *t;
    if (!s->used) pool_slots++;
    s->used = 1;
    s->ce = NULL;
    slots_gen++;
    if (!++reg_serial) reg_serial = 1;
    __atomic_store_n(&sinfo[id], (uint64_t)reg_serial << 16 | agen_idx((uintptr_t)t->mem), __ATOMIC_RELEASE);
    if (t->mem) up_queue(id, reg_serial);
    s->has_view = v != NULL;
    if (v) s->v = *v;
    else memset(&s->v, 0, sizeof s->v);
    unlock();
}

// The entry slot s resolves to, and the view and has_view a caller gets with it (the slot's, with the descriptor's
// swizzle when the entry is another descriptor at the same storage). NULL when the slot names no texture.
// The live entry at k's storage that the GPU wrote last; a same-but-swizzle entry wins a tie (both never written).
// cpu_content: the slot's bytes are the CPU's, laid out for its own descriptor: only GPU-owned entries qualify.
// fallback when none: own (may be NULL or released).
static Tex *newest_writer(const TexKey *k, Tex *own, int cpu_content);

// A GPU write into e was recorded (tex_lock held). It becomes the storage's newest writer (slot_resolve's "content
// follows the memory"); when another entry at the address held that place, the slots there resolve again.
static void note_writer(Tex *e) {
    int newest = 1;
    for (const Tex *p = nvn_hm_get(&by_addr, &e->key.addr); p && newest; p = p->next_at)
        newest = p == e || p->wr_serial < e->wr_serial;
    // a stretch handed out after the write being replaced may still look e up: that write stays known
    if (e->wr_serial && e->wr_serial <= __atomic_load_n(&asof_floor, __ATOMIC_RELAXED)) {
        memmove(e->wr_old + 1, e->wr_old, sizeof e->wr_old - sizeof e->wr_old[0]);
        e->wr_old[0] = e->wr_serial;
    }
    e->wr_serial = wr_serials + 1;
    __atomic_store_n(&wr_serials, e->wr_serial, __ATOMIC_RELEASE);
    __atomic_store_n(&awr[agen_idx(e->key.addr)], e->wr_serial, __ATOMIC_RELEASE);
    if (!newest) tex_gen_bump(e->key.addr);
}

static int asof_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = cfg_get("REKARINA_CHUNK_ASOF");
        on = !(e && *e == '0');
        if (!on) rt_log("vk: chunks of a draw stretch look textures up as the replay thread sees them (REKARINA_CHUNK_ASOF=0)");
    }
    return on;
}

// vk_exec.c par_try, before a stretch's chunks are handed out: the limit their lookups take. 0: none.
uint64_t vk_texture_asof_mark(void) {
    if (!asof_on()) return 0;
    uint64_t m = __atomic_load_n(&wr_serials, __ATOMIC_ACQUIRE);
    __atomic_store_n(&asof_floor, m, __ATOMIC_RELEASE);
    return m;
}

// This thread's lookups from now on: GPU writes recorded after limit do not count (0: all count). Returns the limit
// in force before.
uint64_t vk_texture_asof(uint64_t limit) {
    uint64_t was = asof;
    asof = limit;
    return was;
}
uint64_t vk_texture_asof_limit(void) { return asof; }

// 1 when slot id resolves for this thread as it does for the replay thread: no limit, or no GPU write after it at the
// slot's storage (by address bucket). Otherwise what a lookup returns holds for this thread's limit only and must not
// be kept. Read without the lock, as vk_texture_slot_token.
int vk_texture_asof_clean(int id) {
    if (__builtin_expect(!asof, 1) || __atomic_load_n(&wr_serials, __ATOMIC_ACQUIRE) <= asof) return 1;
    const uint64_t *si = __atomic_load_n(&sinfo, __ATOMIC_ACQUIRE);
    if (!si || id < 0 || id >= (1 << 20)) return 1;
    uint64_t v = __atomic_load_n(&si[id], __ATOMIC_ACQUIRE);
    return !v || __atomic_load_n(&awr[v & 0xffff], __ATOMIC_ACQUIRE) <= asof;
}

static Tex *newest_writer(const TexKey *k, Tex *own, int cpu_content) {
    Tex *best = NULL;
    for (Tex *p = nvn_hm_get(&by_addr, &k->addr); p; p = p->next_at) {
        if (p->released || (cpu_content && !p->pub.gpu_owned && !same_but_swizzle(&p->key, k))) continue;
        // Only an entry of the slot's own target: OoT's volumetric fog registers a 3D volume over the storage of a 2D
        // bloom target. Followed, no 3D view can be made of that image and the fog passes read and write stand-ins.
        if (p->key.d.target != k->d.target) continue;
        if (!best || wr_at(p) > wr_at(best) ||
            (wr_at(p) == wr_at(best) && same_but_swizzle(&p->key, k) && !same_but_swizzle(&best->key, k)))
            best = p;
    }
    return best ? best : own;
}

// ---- texture quality (REKARINA_TEX_QUALITY: auto, full, half)
// At 2560x1440 OoT holds about 6.5 GiB of video memory, 2.8 GiB of it the ASTC textures transcoded to BC3
// (twice to four times their size on the console). On a card with 8 GiB, or one another program holds part of, the
// system's budget for the game is less than that, Windows moves images to system memory and the frame rate falls to a
// fraction (13 fps for 104 with After Effects holding 7 GiB of a 12 GiB card, 2026-11-11).
// half: a texture slot that names a plain 2D ASTC texture of 512 texels or more with a mip chain resolves to the same
// texture from its second level on: a quarter of the memory, the picture of the next smaller level. The slot's
// descriptor is replaced by one of half the size and one level less whose storage starts where the second level lies;
// it is taken only when that descriptor's layout is byte for byte the tail of the full one (level offsets and block
// heights compared), so the upload reads the same bytes it would for those levels. A texture something else uses at
// its full size (a copy, a clear, a render target: it has an entry under its own descriptor) stays whole. Sampling
// with normalised coordinates sees no difference but the missing top level; a shader that asks for the size in texels
// gets half.
// quarter: the same from the third level on for textures of 1,024 texels and more (those of 512 lose one level), and
// the game's own block-compressed textures (BC1 to BC7) of 1,024 and more lose their first level too.
// auto (the default) decides at the first texture from the card's memory that other programs leave: the card's size
// less what the system counts in use on it apart from this process (the adapter's dedicated usage, the counter the
// overlay shows). 7.25 GiB or more: full; 6.2 GiB or more: half (about 1.1 GiB less than full at 1440p); less:
// quarter (about 1.5 GiB less). The budget the driver reports is no help at that moment: it still says the whole card while the other
// program is in the background, and falls only once the game has filled the card. Without the counter the budget
// decides (half under 6.75 GiB).
static int texq_mode = -1;   // 0 full, 1 half, 2 quarter
static uint64_t texq_n, texq_bytes;

static int texq_on(void) {
    if (texq_mode < 0) {
        const char *e = cfg_get("REKARINA_TEX_QUALITY");
        int m = e && !strcmp(e, "quarter") ? 2 : e && !strcmp(e, "half") ? 1 : e && !strcmp(e, "full") ? 0 : -1;
        double free_gib = -1;
        if (m < 0) {
            VkCtx *c = vk_ctx();
            if (!c) return 0;   // asked before the device exists: not decided yet
            uint64_t used = 0, budget = 0, total = 0;
            int exact = 0;
            vk_mem_vram(&used, &budget, &exact);
            for (uint32_t h = 0; h < c->mem_props.memoryHeapCount; h++)
                if (c->mem_props.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) total += c->mem_props.memoryHeaps[h].size;
            VkPhysicalDeviceIDProperties id = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
            VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &id};
            vkGetPhysicalDeviceProperties2(c->phys, &p2);
            float load = -1, sys_mb = -1;
            PlatLoad *l = plat_load_open(id.deviceLUIDValid ? id.deviceLUID : NULL, -1);
            if (l) {
                plat_gpu_load(l, &load, &sys_mb);
                plat_load_close(l);
            }
            if (vk_mem_vram_cap()) {   // DEBUG-VRAMCAP: the capped budget stands for the card
                free_gib = (double)vk_mem_vram_cap() / 1073741824.0;
                m = free_gib >= 7.25 ? 0 : free_gib >= 6.2 ? 1 : 2;
            } else if (sys_mb >= 0 && total) {
                double others = sys_mb * 1048576.0 - (double)used;
                free_gib = ((double)total - (others > 0 ? others : 0)) / 1073741824.0;
                m = free_gib >= 7.25 ? 0 : free_gib >= 6.2 ? 1 : 2;
            } else {
                m = exact && budget && budget < (27ull << 28);
                free_gib = budget / 1073741824.0;
            }
        }
        texq_mode = m;
        static const char *const name[3] = {"full", "half: ASTC textures of 512 texels and more load from their second level",
                                            "quarter: ASTC textures of 1,024 texels and more load from their third level, of 512 and "
                                            "BC textures of 1,024 from their second"};
        if (free_gib >= 0)
            rt_log("vk: texture quality %s (%.1f GiB of video memory are free for the game; REKARINA_TEX_QUALITY: auto, full, half, "
                   "quarter)", name[m], free_gib);
        else
            rt_log("vk: texture quality %s (REKARINA_TEX_QUALITY)", name[m]);
    }
    return texq_mode;
}

// The descriptor of t without its first level (texq_shape: without as many as the quality drops).
static int texq_shape1(const tex_t *t, tex_t *out) {
    const nvn_texdesc *d = &t->d;
    if (d->target != NVN_TEXTURE_TARGET_2D || d->levels < 2 || d->samples > 1 || (d->width < 512 && d->height < 512)) return 0;
    if (!t->mem || nvn_gpu_only((uintptr_t)t->mem) || nvn_tex_is_linear(d)) return 0;
    nvn_texdesc d2 = *d;
    d2.width = umax(d->width >> 1, 1), d2.height = umax(d->height >> 1, 1), d2.levels = d->levels - 1;
    uint64_t off1 = nvn_tex_level_offset(d, 1);
    if (!off1) return 0;
    for (unsigned l = 0; l < d2.levels; l++) {
        unsigned th, td, th2, td2;
        nvn_tex_level_tiles(d, l + 1, &th, &td);
        nvn_tex_level_tiles(&d2, l, &th2, &td2);
        if (th != th2 || td != td2 || nvn_tex_level_offset(&d2, l) != nvn_tex_level_offset(d, l + 1) - off1) return 0;
    }
    *out = *t;
    out->d = d2;
    nvn_tex_layout(&out->d, &out->L);
    if (off1 + out->L.size > t->L.size) return 0;
    out->mem = t->mem + off1, out->off = t->off + (int64_t)off1;
    return 1;
}

// The descriptor the quality in force gives t; the levels dropped, 0 when t stays whole.
static int texq_shape(const tex_t *t, tex_t *out) {
    TexVkFormat f;
    if (!tex_vk_format(t->d.format, &f)) return 0;
    int astc = f.astc_bw != 0;
    int bc = !astc && f.vk >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f.vk <= VK_FORMAT_BC7_SRGB_BLOCK && texq_mode == 2 &&
             (t->d.width >= 1024 || t->d.height >= 1024);
    if ((!astc && !bc) || !texq_shape1(t, out)) return 0;
    tex_t two;
    if (astc && texq_mode == 2 && texq_shape1(out, &two)) {
        *out = two;
        return 2;
    }
    return 1;
}

// Two descriptors whose images hold the same picture: target, format, size, levels and samples (flags, layout, stride
// and swizzle may differ: the images are ours, not the storage).
static int same_shape(const TexKey *a, const TexKey *b) {
    const nvn_texdesc *x = &a->d, *y = &b->d;
    return a->addr == b->addr && x->target == y->target && x->format == y->format && x->width == y->width &&
           x->height == y->height && x->depth == y->depth && x->levels == y->levels &&
           (x->samples > 1) == (y->samples > 1) && (x->samples <= 1 || x->samples == y->samples);
}

// REKARINA_FOLLOW_SHAPE=0: a live descriptor once written keeps its own image (slot_resolve), as before 2026-11-15.
static int follow_same_shape(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = cfg_get("REKARINA_FOLLOW_SHAPE");
        on = !(e && *e == '0');
        if (!on) rt_log("vk: a written descriptor keeps its own image when one of the same shape is written later (REKARINA_FOLLOW_SHAPE=0)");
    }
    return on;
}

// An entry whose image can stand for a slot's descriptor: the same size, the same depth or layer count, one sample or
// several alike, and a format of the same texel size and block (an R11G11B10F target read through an RGB9E5
// descriptor, a depth image through an R32F one). Another size at the storage is the game reusing the memory: a
// lookup of that image with the slot's coordinates reads the wrong texels. REKARINA_FOLLOW_ANY=1: any entry, as before
// 2026-11-15.
static int fits_slot(const TexKey *p, const TexKey *k) {
    static int any = -1;
    if (any < 0) { const char *e = cfg_get("REKARINA_FOLLOW_ANY"); any = e && *e == '1'; }
    if (any) return 1;
    const nvn_texdesc *a = &p->d, *b = &k->d;
    if (a->width != b->width || a->height != b->height || a->depth != b->depth || (a->samples > 1) != (b->samples > 1))
        return 0;
    int aw, ah, bw, bh, ab = nvn_fmt_bpb(a->format, &aw, &ah), bb = nvn_fmt_bpb(b->format, &bw, &bh);
    return ab && ab == bb && aw == bw && ah == bh;
}

// newest_writer among the entries that fit the slot's descriptor k
static Tex *newest_writer_fit(const TexKey *k, Tex *own, int cpu_content) {
    Tex *best = NULL;
    for (Tex *p = nvn_hm_get(&by_addr, &k->addr); p; p = p->next_at) {
        if (p->released || (cpu_content && !p->pub.gpu_owned && !same_but_swizzle(&p->key, k))) continue;
        if (p->key.d.target != k->d.target || !fits_slot(&p->key, k)) continue;
        if (!best || wr_at(p) > wr_at(best) ||
            (wr_at(p) == wr_at(best) && same_but_swizzle(&p->key, k) && !same_but_swizzle(&best->key, k)))
            best = p;
    }
    return best ? best : own;
}

static Tex *slot_resolve(TexSlot *s, view_t *view, int *has_view) {
    int no_make = 0;   // the pool's lookup leaves the slot unresolved rather than make an image (below)
    *view = s->v;
    *has_view = s->has_view;
    TexKey k;
    make_key(&s->t, &k);
    Tex *e = nvn_hm_get(&by_key, &k);
    tex_t half;
    int drops = (!e || e->released) && texq_on() ? texq_shape(&s->t, &half) : 0;
    if (drops) {   // "texture quality": the slot's texture, smaller
        TexKey hk;
        make_key(&half, &hk);
        Tex *he = nvn_hm_get(&by_key, &hk);
        if ((!he || he->released) && (cold_kept_out(&hk, half.L.size) || pool_holds_back(&half, (int)(s - tslots)))) return NULL;
        if (!he || he->released) {
            he = entry_of(&half);
            if (he) {
                texq_n++, texq_bytes += s->t.L.size - half.L.size;
                if (!(texq_n & 1023))
                    rt_log("vk: texture quality: %llu textures made smaller so far, %.0f MiB of ASTC left out",
                           (unsigned long long)texq_n, texq_bytes / 1048576.0);
            }
        }
        if (he) {
            {   // tex_gen_bump_entry: this slot hears when he goes. A texture and its own second level registered as a
                // texture of its own reach the same smaller entry from two addresses.
                uint64_t base = (uintptr_t)s->t.mem;
                int at = -1;
                for (int i = 0; i < 3 && at != -2; i++)
                    if (he->texq_base[i] == base) at = -2;
                    else if (!he->texq_base[i] && at < 0) at = i;
                if (at == -1) at = 0, tex_gen_bump(he->texq_base[0]);   // full: the one replaced resolves again
                if (at >= 0) he->texq_base[at] = base;
            }
            if (*has_view && view->set_levels)
                view->base_level = (uint8_t)(view->base_level > drops ? view->base_level - drops : 0);
            tex_watch("slot", &hk, he);   // DEBUG-TEXWATCH
            return he;
        }
    }
    if (!e || e->released) {   // MNG hle_nvn.c: a finalized slot follows the live texture at its storage
        // Prefer the live entry that differs from the slot descriptor only in its swizzle: transient render targets
        // share storage, and the newest one at the address is usually another size. The map screen samples its
        // 2250x1080 map target through an (R,G,B,1) descriptor and followed a 960x60 target at the same address.
        // A texture whose content the CPU writes (tex_mirrored) follows another descriptor only when the GPU rendered
        // that one's content: otherwise the bytes are in the storage, laid out for the slot's own descriptor. OoT's
        // subtitles rasterize each line on the CPU into a linear R8 strip at the same storage; a line of another size
        // followed the previous line's 256x45 entry, read its 1300-texel rows 256 at a time and came out as stripes.
        // Content follows the memory, as on the console: of the live entries there, the one the GPU wrote last (a
        // same-but-swizzle one wins a tie). The first live entry of the list was often a target of another size
        // that this frame had not rendered yet, and the whole 3D scene came out black for a frame (the composite
        // read it: one black frame every 600-1,300, flight.c flash watch).
        // Only an entry whose image fits the slot's descriptor (fits_slot): when the game moves its transient targets,
        // an R8 1280x720 mask of a compute chain had its own entry released and followed an RGBA8 2560x1440 target
        // written later at that storage; the chain wrote and read its 1280x720 grid in the corner of the larger
        // image, the sun-shadow pass took a quarter-size copy of the scene for its mask, and the lit objects lost
        // their sunlight for several frames (2026-11-15).
        int cpu_content = tex_mirrored(&s->t);
        Tex *own = e;
        e = newest_writer_fit(&k, e, cpu_content);
        // The pool's lookup of a slot whose own entry is gone, at storage only entries of another shape hold: no new
        // image (the stale slots the pool resolves would be made again after every sweep); a draw that binds the slot
        // makes it. (pool_slot_locked leaves the slot unresolved meanwhile.)
        if (!e && pool_lookup && newest_writer(&k, NULL, cpu_content)) no_make = 1;
        {   // DEBUG-FITLOG: REKARINA_FITLOG=1: lookups the fit rule sent to the slot's own entry (or a new one) where any
            // entry at the storage would have been followed before
            static int on = -1, nl;
            if (on < 0) on = cfg_get("REKARINA_FITLOG") != NULL;
            Tex *any = on && e == own && nl < 3000 ? newest_writer(&k, NULL, cpu_content) : NULL;
            if (any && any != e) {
                nl++;
                rt_log("FITLOG frame %llu addr %#llx slot %ux%u fmt %#x tgt %d -> %s; other %ux%u fmt %#x wr %llu",
                       (unsigned long long)__atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED),
                       (unsigned long long)k.addr, k.d.width, k.d.height, k.d.format, k.d.target,
                       !own ? "a new entry" : own->released ? "its released entry" : "its entry", any->key.d.width,
                       any->key.d.height, any->key.d.format, (unsigned long long)wr_at(any));
            }
        }
    } else if (!wr_at(e) && !e->pub.mirrored) {
        // A live descriptor the GPU never wrote, at storage another descriptor rendered: it reads that render (a new
        // descriptor over a transient target the game rendered through another one), when its image fits.
        Tex *w = newest_writer_fit(&k, e, 0);
        if (w && wr_at(w)) e = w;
    } else if (!e->pub.mirrored && __atomic_load_n(&awr[agen_idx(e->key.addr)], __ATOMIC_ACQUIRE) > wr_at(e) &&
               follow_same_shape()) {   // (the bucket's newest write first: most lookups have nothing newer to walk to)
        // A live descriptor the GPU wrote some frames ago, at storage that another descriptor of the same shape (format,
        // size, levels) was written through since: it reads that newer write. When the game moves its transient
        // targets to other addresses (a frame that switches an effect on or off), the sun-shadow pass reads an R8
        // 1280x720 mask that a compute pass of the same frame wrote through a second descriptor at the same storage;
        // the reader kept its own image, filled frames earlier at another layout, and the objects lost their sunlight
        // for several frames (the dark frames while the camera turns, 2026-11-15). Only the same shape: a target of
        // another size written later at the address is the game's reuse of the memory, not this content.
        Tex *w = newest_writer(&k, e, 0);
        if (w && w != e && wr_at(w) > wr_at(e) && same_shape(&w->key, &e->key)) e = w;
    }
    {   // DEBUG-SPIKE: entries made while a slot resolves
        uint64_t spk0 = plat_now_ns();
        int made = !e && s->t.mem;
        int mk0 = sec_mk;   // not the pool: a draw's binding
        if (sec_on && !mk0) sec_mk = 1;
        if (!e && !no_make)
            e = s->t.mem && !cold_kept_out(&k, s->t.L.size) && !pool_holds_back(&s->t, (int)(s - tslots)) ? entry_of(&s->t) : NULL;
        if (sec_on) sec_mk = mk0;
        if (made) vk_spike.tex += plat_now_ns() - spk0, vk_spike.ntex++;
    }
    tex_watch("slot", &k, e);   // DEBUG-TEXWATCH
    // A followed texture is another descriptor at the same storage: its image holds the content, but sampling takes the
    // slot descriptor's swizzle. The UI renders text into RGBA8 and samples it through a (A,A,A,A) descriptor; with the
    // render target's RGBA swizzle the text blur saw R = 1 everywhere and drew solid boxes behind tips and labels.
    if (e && !(s->has_view && s->v.set_swizzle)) {
        const nvn_texdesc *d = &s->t.d;
        uint8_t sw[4] = {(uint8_t)d->swizzle_r, (uint8_t)d->swizzle_g, (uint8_t)d->swizzle_b, (uint8_t)d->swizzle_a};
        if (!(sw[0] | sw[1] | sw[2] | sw[3]))   // descriptors built without nvnTextureBuilderSetDefaults
            sw[0] = NVN_TEXTURE_SWIZZLE_R, sw[1] = NVN_TEXTURE_SWIZZLE_G, sw[2] = NVN_TEXTURE_SWIZZLE_B,
            sw[3] = NVN_TEXTURE_SWIZZLE_A;
        if (memcmp(sw, e->pub.swz, 4)) {
            if (!s->has_view) memset(view, 0, sizeof *view);
            memcpy(view->swz, sw, 4);
            view->set_swizzle = 1;
            *has_view = 1;
        }
    }
#ifdef GAME_NS2
    // The same for the format: OoT samples the RGB9E5 words its 3D frame packed into an R11G11B10F image through an
    // RGB9E5 descriptor at that storage, which follows the R11G11B10F entry. The slot's format goes in as a view format
    // (vk_exec_draw.c reinterpret_format makes the view when both have one texel size).
    if (e && e->t.d.format != s->t.d.format && !(*has_view && view->set_format)) {
        if (!*has_view) memset(view, 0, sizeof *view);
        view->format = s->t.d.format;
        view->set_format = 1;
        *has_view = 1;
    }
#endif
    return e;
}

// ---- uploads ahead of first use (REKARINA_TEX_PREP=1; off by default)
// A texture reaches the GPU at its first use: the pool set's update, or a draw's binding, creates its image and fills
// it, a file read or a copy out of the game's memory of up to 14 MiB, on the replay thread. Arriving on a map the game
// registers textures for about three seconds (15 new ones a frame, 1 to 5 ms of copies and as much again in the pool
// update: 55 to 65 fps where the place then runs at 95), and running through the Market it brings in 45 MiB a
// second the same way (the frames of 20 ms and more there). The game names a texture in a pool slot some time before a
// frame draws with it, so a thread of its own takes each newly registered slot, resolves it as the pool update would
// and uploads it, with the texture lock released while the bytes are read (upload: fill_enter). The replay then finds
// the texture there. What the thread does not do stays as it was: the GPU's ASTC decode of a texture the cache does not
// hold, render targets, and whatever the replay reaches first.
// Off by default (2026-11-13): the 180 frames after an arrival in the field went from 18.4 to 17.4 ms and running
// was the same, and of six two-minute runs of the field with it one lost the device (a read of an image destroyed ten
// seconds before; eight runs without it were clean). docs/oot-notes.md, "Uploads ahead of first use".
#define UPQ_N 8192
static struct { uint32_t id, serial; } upq[UPQ_N];
static uint32_t upq_head, upq_tail;   // upq_lock
static PlatMutex upq_lock = PLAT_MUTEX_INIT;
static PlatEvent upq_ev;
static volatile int32_t up_started;
static uint64_t up_done, up_dropped;

static int up_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *v = cfg_env("REKARINA_TEX_PREP");
        on = v && *v == '1';
    }
    return on;
}

static void up_range_gone(void) {   // no fill reads a pool's memory once this returns (the entries there are gone)
    if (!__atomic_load_n(&up_started, __ATOMIC_ACQUIRE)) return;
    plat_mutex_lock(&fill_lock);
    plat_mutex_unlock(&fill_lock);
}

static Tex *slot_resolve(TexSlot *s, view_t *view, int *has_view);

static void up_main(void *arg) {
    (void)arg;
    up_worker = 1;
    sec_thread_off = 1;
    for (;;) {
        uint32_t id = 0, serial = 0;
        int have = 0;
        plat_mutex_lock(&upq_lock);
        if (upq_head != upq_tail) id = upq[upq_head % UPQ_N].id, serial = upq[upq_head % UPQ_N].serial, upq_head++, have = 1;
        plat_mutex_unlock(&upq_lock);
        if (!have) {
            plat_event_wait(&upq_ev);
            continue;
        }
        lock();
        if ((int)id < ntslots && tslots[id].used && sinfo && (uint32_t)(sinfo[id] >> 16) == serial) {
            TexSlot *s = &tslots[id];
            VkMemSpan m;
            if (tex_mirrored(&s->t) && vk_mem_resolve((uintptr_t)s->t.mem, s->t.L.size ? s->t.L.size : 1, &m)) {
                // the storage's hash, which a new entry needs, with the lock released (as upload's fill)
                TexKey k;
                make_key(&s->t, &k);
                up_pre.valid = 0;
                if (!nvn_hm_get(&by_key, &k) && s->t.L.size >= 4096) {
                    uint64_t addr = (uintptr_t)s->t.mem, size = s->t.L.size;
                    fill_enter();
                    up_pre.addr = addr, up_pre.size = size;
                    up_pre.cprint = content_print((const uint8_t *)(uintptr_t)addr, size);
                    up_pre.chash = content_hash((const uint8_t *)(uintptr_t)addr, size);
                    lock();
                    plat_mutex_unlock(&fill_lock);
                    up_pre.valid = 1;
                }
                // (the slot table may have moved and the slot been registered again meanwhile)
                if ((int)id < ntslots && tslots[id].used && (uint32_t)(sinfo[id] >> 16) == serial &&
                    vk_mem_resolve((uintptr_t)tslots[id].t.mem, tslots[id].t.L.size ? tslots[id].t.L.size : 1, &m)) {
                    view_t v;
                    int hv;
                    Tex *e = slot_resolve(&tslots[id], &v, &hv);
                    if (e && !e->pub.gone && !e->uploaded) make_fresh(e);
                    up_done++;
                }
                up_pre.valid = 0;
            }
        }
        unlock();
    }
}

// vk_texture_register, lock held: slot id names another texture from now on.
static void up_queue(int id, uint32_t serial) {
    if (!up_on() || !vk_ctx()) return;
    if (!__atomic_load_n(&up_started, __ATOMIC_ACQUIRE)) {
        plat_mutex_lock(&upq_lock);
        if (!up_started) {
            plat_event_init(&upq_ev, 0, 0);
            if (plat_thread_spawn(up_main, NULL, "vk uploads ahead", 0)) {
                __atomic_store_n(&up_started, 1, __ATOMIC_RELEASE);
                rt_log("vk: textures are uploaded ahead of their first use by a thread of their own (REKARINA_TEX_PREP=1)");
            } else {
                __atomic_store_n(&up_started, -1, __ATOMIC_RELEASE);
            }
        }
        plat_mutex_unlock(&upq_lock);
    }
    if (up_started <= 0) return;
    plat_mutex_lock(&upq_lock);
    if (upq_tail - upq_head < UPQ_N) upq[upq_tail % UPQ_N].id = (uint32_t)id, upq[upq_tail % UPQ_N].serial = serial, upq_tail++;
    else up_dropped++;
    plat_mutex_unlock(&upq_lock);
    plat_event_set(&upq_ev);
}

void vk_texture_lock(void) { lock(); }
void vk_texture_unlock(void) { unlock(); }
uint32_t vk_texture_slots_gen(void) { return slots_gen; }
int vk_texture_slot_count(void) { return __atomic_load_n(&ntslots, __ATOMIC_RELAXED); }

// vk_texpool.c: vk_texture_by_id_locked for a slot nothing binds, only while its storage lies in a live memory pool.
// The game leaves slots registered after it finalized their pool; a draw never names those, but resolving one would
// upload from freed memory (an ASTC decode read unmapped pages in the forest).
static const VkTexImage *pool_slot_locked(int id, view_t *view, int *has_view);
const VkTexImage *vk_texture_pool_slot_locked(int id, view_t *view, int *has_view) {
    if (__builtin_expect(sec_on, 0)) {
        sec_mk = 2;
        const VkTexImage *r = pool_slot_locked(id, view, has_view);
        sec_mk = 0;
        return r;
    }
    return pool_slot_locked(id, view, has_view);
}

static const VkTexImage *pool_slot_locked(int id, view_t *view, int *has_view) {
    if (id < 0 || id >= ntslots || !tslots[id].used) return NULL;
    const tex_t *t = &tslots[id].t;
    if (!t->mem) return NULL;
    VkMemSpan m;
    if (!nvn_gpu_only((uintptr_t)t->mem) && !vk_mem_resolve((uintptr_t)t->mem, t->L.size ? t->L.size : 1, &m)) return NULL;
    // REKARINA_TPOOL_NO_GPU=0 turns this off. A slot of a GPU-only target with no live image gets a stand-in. The pool otherwise makes
    // a new image for every registered slot, so targets dropped or recycled came back as permanent live images and memory never fell.
    static int no_gpu = -1;
    if (no_gpu < 0) { const char *s = cfg_env("REKARINA_TPOOL_NO_GPU"); no_gpu = !(s && s[0] == '0'); }
    if (no_gpu && nvn_gpu_only((uintptr_t)t->mem)) {
        TexKey k;
        make_key(t, &k);
        Tex *e = nvn_hm_get(&by_key, &k);
        if ((!e || e->released) && !newest_writer(&k, NULL, 0)) return NULL;
    }
    pool_lookup = 1;   // not a use: cold_kept_out, drawn
    const VkTexImage *r = vk_texture_by_id_locked(id, view, has_view);
    pool_lookup = 0;
    return r;
}

int vk_texture_pool_class(const VkTexImage *img) {
    const Tex *e = (const Tex *)img;   // pub is the first member
    if (!comes_back_intact(e)) return 0;
    if (e->pub.layers > 1 || e->pub.depth > 1) return 4;
    return (e->f.astc_bc ? 1 : 2) | (e->ever_drawn ? 8 : 0);
}

uint64_t vk_texture_slot_token(int id) {
    const uint64_t *si = __atomic_load_n(&sinfo, __ATOMIC_ACQUIRE);
    if (!si || id < 0 || id >= (1 << 20)) return 0;
    uint64_t v = __atomic_load_n(&si[id], __ATOMIC_ACQUIRE);
    return v ? (v >> 16) << 32 | __atomic_load_n(&agen[v & 0xffff], __ATOMIC_ACQUIRE) : 0;
}
uint32_t vk_sampler_gen(void) { return __atomic_load_n(&samp_gen, __ATOMIC_RELAXED); }

// vk_exec_draw.c, when a draw's handle is bound: slot id's cached resolution and registration word are asked into the
// cache for the lookups of the draw that follows (vk_texture_by_id_locked, vk_texture_slot_token). The tables are
// megabytes and a frame's handles are scattered over them. Read without the lock: a table moved meanwhile only wastes
// the prefetch, which reads nothing.
void vk_texture_slot_prefetch(int id) {
    const TexSlot *s = __atomic_load_n(&tslots, __ATOMIC_RELAXED);
    const uint64_t *si = __atomic_load_n(&sinfo, __ATOMIC_RELAXED);
    if ((unsigned)id >= (unsigned)__atomic_load_n(&ntslots, __ATOMIC_RELAXED)) return;
    if (s) __builtin_prefetch(&s[id]);
    if (si) __builtin_prefetch(&si[id]);
}

// vk_texpool.c: the slots' registration words, NULL before the first registration. Bits 16 and up of word id are the
// slot's registration serial, the upper half of vk_texture_slot_token(id); the pool's update compares only that half,
// for every slot of the pool at every submission, and the token's other half costs a second, scattered load.
const uint64_t *vk_texture_slot_regs(void) { return __atomic_load_n(&sinfo, __ATOMIC_ACQUIRE); }

const VkTexImage *vk_texture_by_id(int id, view_t *view, int *has_view) {
    lock();
    const VkTexImage *r = vk_texture_by_id_locked(id, view, has_view);
    unlock();
    return r;
}

const VkTexImage *vk_texture_by_id_locked(int id, view_t *view, int *has_view) {
    if (id < 0 || id >= ntslots || !tslots[id].used) return NULL;
    TexSlot *s = &tslots[id];
    if (__builtin_expect(asof != 0, 0) && awr[sinfo[id] & 0xffff] > asof) {
        // "lookups as of a place": the GPU wrote at the slot's storage (or at an address of its bucket) after this
        // thread's limit. Resolved for the limit and not kept: the slot's kept resolution is the replay thread's.
        view_t v;
        int hv;
        Tex *e = slot_resolve(s, &v, &hv);
        if (e) {
            if (!e->uploaded || e->checked != submit_epoch) {
                make_fresh_snap(e);
                e->checked = submit_epoch;
                touch(e);
            }
            if (!pool_lookup) drawn(e), e->ever_drawn = 1;
        }
        if (!(++n_asof & 0xfffff))
            rt_log("vk: %llu texture lookups of draw stretches so far resolved as of their stretch's place (REKARINA_CHUNK_ASOF)",
                   (unsigned long long)n_asof);
        if (view) *view = has_view ? v : s->v;
        if (has_view) *has_view = view ? hv : s->has_view;
        return e ? &e->pub : NULL;
    }
    uint32_t g = agen[sinfo[id] & 0xffff];
    if (!s->ce || s->cgen != g) {   // resolved again: an entry at its address was created, dropped or released, or the
        view_t v;                   // slot registered (s->ce cleared); an empty result is not kept: image creation may
        int hv;                     // succeed later
        s->ce = slot_resolve(s, &v, &hv);   // entry_of may create an entry and bump g: g read before it
        s->cview = v, s->chas_view = (uint8_t)hv;
        s->cgen = g;
        s->cepoch = 0;
    }
    if (s->ce && s->ce->pub.gone) {   // must not happen (the rule above tex_gen_bump_entry): resolved again, and said
        static volatile int32_t n_gone;
        log_limited(&n_gone, "vk: slot %d kept a destroyed entry (texture %#llx %ux%u, entry at %#llx %ux%u, %u levels): resolved again",
                    id, (unsigned long long)(uintptr_t)s->t.mem, s->t.d.width, s->t.d.height, (unsigned long long)s->ce->key.addr,
                    s->ce->key.d.width, s->ce->key.d.height, s->ce->key.d.levels);
        view_t v;
        int hv;
        s->ce = slot_resolve(s, &v, &hv);
        s->cview = v, s->chas_view = (uint8_t)hv;
        s->cgen = agen[sinfo[id] & 0xffff];
        s->cepoch = 0;
    }
    Tex *e = s->ce;
    // Bindings of one submit share one check, answered from the submit's write-watch snapshot (guest writes during a
    // replay race the GPU read on hardware too). After one check an entry is either uploaded or never uploads (not
    // mirrored, GPU-owned or no CPU layout), so a slot that checked its entry in this submit has nothing left to do.
    if (e && s->cepoch != submit_epoch) {
        round_checks++;
        if (!e->uploaded || e->checked != submit_epoch) {
            make_fresh_snap(e);
            e->checked = submit_epoch;
            touch(e);
        }
        s->cepoch = submit_epoch;
    }
    if (e && !pool_lookup && s->depoch != submit_epoch) {   // a draw's first lookup through the slot in this submission
        s->depoch = submit_epoch;
        draw_checks++;
        drawn(e);
        e->ever_drawn = 1;
    }
    if (view) *view = has_view ? s->cview : s->v;
    if (has_view) *has_view = view ? s->chas_view : s->has_view;
    return e ? &e->pub : NULL;
}

// vk_dump: the swizzle selectors of the descriptor registered in slot id; 0 when the slot is empty.
int vk_texture_slot_swz(int id, uint8_t swz[4]) {
    lock();
    int ok = id >= 0 && id < ntslots && tslots[id].used;
    if (ok) {
        const nvn_texdesc *d = &tslots[id].t.d;
        swz[0] = (uint8_t)d->swizzle_r, swz[1] = (uint8_t)d->swizzle_g, swz[2] = (uint8_t)d->swizzle_b;
        swz[3] = (uint8_t)d->swizzle_a;
    }
    unlock();
    return ok;
}

uint32_t vk_texture_epoch(void) { return __atomic_load_n(&submit_epoch, __ATOMIC_RELAXED); }

void vk_texture_new_submit(void) {
    lock();
    if (round_checks && (uint64_t)round_checks * 2 >= pool_slots) look_round++;   // the last one walked the pool
    round_checks = 0;
    if (draw_checks >= DRAW_SUBMIT_MIN)   // the last one drew a scene
        draw_round++, scene_frame = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    draw_checks = 0;
    ht_take();
    if (!++submit_epoch) submit_epoch = 1;
    sweep_pressure();
    pm_gone(0, 0, NULL, PM_KEEP);
    static uint64_t log_at;
    uint64_t f = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) / 60;
    if ((rt_trace & RT_TRACE_PERF) && f != log_at && tex_alias > 0) {
        log_at = f;
        rt_log("perf: frame %llu: texture aliases: content copied %llu, filled from storage %llu, left uninitialised %llu",
               (unsigned long long)f * 60, (unsigned long long)n_alias_copied, (unsigned long long)n_alias_upload,
               (unsigned long long)n_alias_uninit);
        n_alias_copied = n_alias_upload = n_alias_uninit = 0;
        rt_log("perf: frame %llu: texture re-uploads %llu (%.1f MiB, %.2f ms), after WriteTexels %llu (%.1f MiB, %.2f ms)",
               (unsigned long long)f * 60, (unsigned long long)ru_n, ru_bytes / 1048576.0, ru_ns / 1e6,
               (unsigned long long)ru_wt_n, ru_wt_bytes / 1048576.0, ru_wt_ns / 1e6);
        ru_n = ru_bytes = ru_ns = ru_wt_n = ru_wt_bytes = ru_wt_ns = 0;
        uint64_t ck, calls, untr;
        vk_mem_snap_stats(&ck, &calls, &untr);
        rt_log("perf: frame %llu: binding storage checks %llu (texture %llu), write-watch snapshot queries %llu, untracked %llu",
               (unsigned long long)f * 60, (unsigned long long)ck, (unsigned long long)n_checks, (unsigned long long)calls,
               (unsigned long long)untr);
        n_checks = 0;
        if (!(f % 10)) texture_census(f * 60);
        if (n_evict) rt_log("perf: frame %llu: cold textures evicted %llu (%.1f MiB)", (unsigned long long)f * 60, (unsigned long long)n_evict, n_evict_bytes / 1048576.0);
        if (ht_started > 0 && !(f % 10)) {
            plat_mutex_lock(&ht_lock);
            int nt = 0;
            for (int i = 0; i < HT_N; i++) nt += htab[i].addr && (int64_t)(f * 60 - htab[i].frame) <= HT_KEEP_FRAMES;
            rt_log("perf: frame %llu: handle tables: %d read, %llu readings of %.1f MiB each, %llu handle-like words a reading, "
                   "%llu reads failed; slots named %llu a reading, entries kept in use %llu, evicted ones brought back %llu",
                   (unsigned long long)f * 60, nt, (unsigned long long)HS.scans, HS.scans ? HS.bytes / 1048576.0 / HS.scans : 0.0,
                   (unsigned long long)(HS.scans ? HS.words / HS.scans : 0), (unsigned long long)HS.fails,
                   (unsigned long long)(HS.scans ? HS.ids / HS.scans : 0), (unsigned long long)(HS.scans ? HS.marked / HS.scans : 0),
                   (unsigned long long)HS.revived);
            memset(&HS, 0, sizeof HS);
            plat_mutex_unlock(&ht_lock);
        }
        n_evict = n_evict_bytes = 0;
        rt_log("perf: frame %llu: released textures kept %d, dropped under VRAM pressure %llu", (unsigned long long)f * 60,
               nreleased, (unsigned long long)n_pressure_drops);
        if (__atomic_load_n(&pm_started, __ATOMIC_ACQUIRE)) {
            plat_mutex_lock(&pm_lock);
            rt_log("perf: frame %llu: images made ahead: asked %llu, made %llu, not bindable %llu, taken %llu (waited for %llu), "
                   "made at first use instead %llu, never used %llu, address moved %llu, table full %llu", (unsigned long long)f * 60,
                   (unsigned long long)PMS.asked, (unsigned long long)PMS.made, (unsigned long long)PMS.failed,
                   (unsigned long long)PMS.taken, (unsigned long long)PMS.waited, (unsigned long long)PMS.stolen,
                   (unsigned long long)PMS.stale, (unsigned long long)PMS.moved, (unsigned long long)PMS.full);
            memset(&PMS, 0, sizeof PMS);
            plat_mutex_unlock(&pm_lock);
        }
        n_pressure_drops = 0;
        released_census(f * 60);
    }
    unlock();
    // Video memory was short at this submission: the compute pipelines built ahead that nothing has dispatched go as
    // well (vk_pipeline.c vk_pipeline_compute_trim), at most once in ten seconds. NVIDIA's driver does not hand their
    // memory back to the system then; it uses it for what it allocates next, which is still less than keeping them.
    static uint64_t trim_at;
    if (cs_trim_due && f >= trim_at) {
        cs_trim_due = 0, trim_at = f + 10;
        vk_pipeline_compute_trim();
    }
}

// ---------------------------------------------------------------- texel reads and writes
typedef struct { unsigned level, layer0; int nz, is3d, bw, bh, bpb; size_t wb, hb, row, img, x0, y0; } Region;

static int region_of(const tex_t *t, const view_t *v, const NVNcopyRegion *r, uint64_t row, uint64_t img, Region *g) {
    g->bpb = nvn_fmt_bpb(t->d.format, &g->bw, &g->bh);
    if (!g->bpb || r->width <= 0 || r->height <= 0 || r->depth < 0 || r->xoffset < 0 || r->yoffset < 0 || r->zoffset < 0 ||
        r->xoffset % g->bw || r->yoffset % g->bh)
        return 0;
    g->level = v && v->set_levels ? v->base_level : 0;
    g->layer0 = v && v->set_layers ? v->min_layer : 0;
    g->nz = r->depth ? r->depth : 1;
    g->is3d = t->d.target == NVN_TEXTURE_TARGET_3D;
    g->wb = (size_t)((r->width + g->bw - 1) / g->bw) * (size_t)g->bpb;
    g->hb = (size_t)(r->height + g->bh - 1) / (size_t)g->bh;
    g->row = row ? row : g->wb;
    g->img = img ? img : g->row * g->hb;
    g->x0 = (size_t)(r->xoffset / g->bw) * (size_t)g->bpb;
    g->y0 = (size_t)(r->yoffset / g->bh);
    return 1;
}

// Pool bytes <-> caller rows of the region alone (nvn_texels_box). MacNativeGames' nvn_mtl.m goes through a
// linear copy of the whole level; for a small region of a large texture that cost tens of milliseconds while the guest
// thread held its virtual core and the caller the texture lock (monsters2 replay, frames 765-790).
static int storage_texels(const tex_t *t, const Region *g, const NVNcopyRegion *r, uint8_t *mem, int write) {
    for (int z = 0; z < g->nz; z++) {
        unsigned layer = g->is3d ? 0 : g->layer0 + (unsigned)r->zoffset + (unsigned)z;
        uint32_t slice = g->is3d ? (uint32_t)(r->zoffset + z) : 0;
        if (nvn_texels_box(&t->d, g->level, layer, (uint32_t)g->x0, (uint32_t)g->y0, slice, (uint32_t)g->wb,
                           (uint32_t)g->hb, t->mem, mem + (size_t)z * g->img, g->row, !write))
            return 0;
    }
    return 1;
}

static int read_aspect(const Tex *e, VkImageAspectFlags *a) {
    if (!e->f.depth && !e->f.stencil) { *a = VK_IMAGE_ASPECT_COLOR_BIT; return 1; }
    switch (e->t.d.format) {   // packed depth-stencil storage has no single-aspect copy
    case 0x31: *a = VK_IMAGE_ASPECT_STENCIL_BIT; return 1;
    case 0x32: case 0x34: *a = VK_IMAGE_ASPECT_DEPTH_BIT; return 1;
    default: return 0;
    }
}

// An image region read through a staging buffer. texels_read_record records the copy into the open upload batch;
// texels_read_take copies the rows out once that batch is done (vk_upload_flush(1)) and frees the buffer. Several
// records before one flush cost one GPU round trip instead of one each.
typedef struct {
    VkBuffer sb;
    VmaAllocation sa;
    const uint8_t *stg;
    size_t slice, ib;   // ib: bytes per texel of an expanded RGB image, 0 when the image holds the storage layout
    uint32_t w;
} TexRead;

static int texels_read_record(Tex *e, const Region *g, const NVNcopyRegion *r, TexRead *t) {
    static volatile int32_t n_astc, n_ds;
    VkImageAspectFlags aspect;
    if (e->f.astc_bw) {
        log_limited(&n_astc, "ReadTexels of ASTC texture %#llx: the image holds decoded RGBA8; zeros returned",
                    (unsigned long long)e->pub.addr);
        return 0;
    }
    if (!read_aspect(e, &aspect)) {
        log_limited(&n_ds, "texels of packed depth-stencil format %#x are not copied", e->t.d.format);
        return 0;
    }
    if (g->level >= e->pub.levels) return 0;
    uint32_t w = (uint32_t)r->width, h = (uint32_t)r->height;
    t->w = w;
    t->ib = e->f.expand_rgb ? (size_t)g->bpb / 3 * 4 : 0;
    t->slice = t->ib ? (size_t)w * h * t->ib : g->wb * g->hb;
    t->stg = vk_staging(t->slice * (size_t)g->nz, &t->sb, &t->sa);
    VkBufferImageCopy *rg = calloc((size_t)g->nz, sizeof *rg);
    if (!rg) rt_fatal("vk: out of memory for a texture read");
    for (int z = 0; z < g->nz; z++)
        rg[z] = (VkBufferImageCopy){.bufferOffset = t->slice * (size_t)z,
                                    .imageSubresource = {aspect, g->level, g->is3d ? 0 : g->layer0 + (uint32_t)r->zoffset + (uint32_t)z, 1},
                                    .imageOffset = {r->xoffset, r->yoffset, g->is3d ? r->zoffset + z : 0},
                                    .imageExtent = {w, h, 1}};
    VkCommandBuffer cb = vk_upload_begin();
    barrier(cb, ALL_RW, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    vkCmdCopyImageToBuffer(cb, e->pub.image, VK_IMAGE_LAYOUT_GENERAL, t->sb, (uint32_t)g->nz, rg);
    barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
            VK_ACCESS_2_HOST_READ_BIT);
    vk_upload_end();
    free(rg);
    return 1;
}

static void texels_read_take(const Tex *e, const Region *g, const TexRead *t, uint8_t *mem) {
    VkCtx *c = vk_ctx();
    vmaInvalidateAllocation(c->vma, t->sa, 0, VK_WHOLE_SIZE);
    for (int z = 0; z < g->nz; z++) {
        const uint8_t *s = t->stg + t->slice * (size_t)z;
        uint8_t *m = mem + (size_t)z * g->img;
        for (size_t y = 0; y < g->hb; y++) {
            if (t->ib) shrink_rgb(e->t.d.format, s + y * t->w * t->ib, m + y * g->row, t->w);
            else memcpy(m + y * g->row, s + y * g->wb, g->wb);
        }
    }
    vmaDestroyBuffer(c->vma, t->sb, t->sa);
}

// Image <-> caller rows through a staging buffer; the GPU copy is waited for.
static int image_texels(Tex *e, const Region *g, const NVNcopyRegion *r, uint8_t *mem, int write) {
    static volatile int32_t n_astc, n_ds;
    VkCtx *c = vk_ctx();
    VkImageAspectFlags aspect;
    if (!write) {
        TexRead t;
        if (!texels_read_record(e, g, r, &t)) return 0;
        vk_upload_flush(1);
        texels_read_take(e, g, &t, mem);
        return 1;
    }
    if (!read_aspect(e, &aspect)) {
        log_limited(&n_ds, "texels of packed depth-stencil format %#x are not copied", e->t.d.format);
        return 0;
    }
    if (g->level >= e->pub.levels) return 0;
    uint32_t w = (uint32_t)r->width, h = (uint32_t)r->height;
    int rgb = e->f.expand_rgb, astc = e->f.astc_bw != 0, agpu = astc && vk_astc_gpu();
    size_t texels = (size_t)w * h, ib = agpu ? 0 : astc ? 4 : rgb ? (size_t)g->bpb / 3 * 4 : 0;
    size_t slice = ib ? texels * ib : g->wb * g->hb;   // agpu: the raw blocks, decoded by vk_astc_record
    VkBuffer sb;
    VmaAllocation sa;
    uint8_t *stg = vk_staging(slice * (size_t)g->nz, &sb, &sa);
    VkBufferImageCopy *rg = calloc((size_t)g->nz, sizeof *rg);
    for (int z = 0; z < g->nz; z++) {
        uint8_t *s = stg + slice * (size_t)z;
        const uint8_t *m = mem + (size_t)z * g->img;
        if (agpu) {
            for (size_t y = 0; y < g->hb; y++) memcpy(s + y * g->wb, m + y * g->row, g->wb);
        } else if (astc) {
            uint8_t *tight = malloc(g->wb * g->hb);
            for (size_t y = 0; y < g->hb; y++) memcpy(tight + y * g->wb, m + y * g->row, g->wb);
            uint64_t a0 = perf_begin();
            astc_decode_ldr_mt(tight, w, h, e->f.astc_bw, e->f.astc_bh, e->f.astc_srgb, s);
            perf_end(PERF_ASTC, a0);
            free(tight);
        } else if (rgb) {
            for (size_t y = 0; y < g->hb; y++) expand_rgb(e->t.d.format, m + y * g->row, s + y * w * ib, w);
        } else {
            for (size_t y = 0; y < g->hb; y++) memcpy(s + y * g->wb, m + y * g->row, g->wb);
        }
        rg[z] = (VkBufferImageCopy){.bufferOffset = slice * (size_t)z,
                                    .imageSubresource = {aspect, g->level, g->is3d ? 0 : g->layer0 + (uint32_t)r->zoffset + (uint32_t)z, 1},
                                    .imageOffset = {r->xoffset, r->yoffset, g->is3d ? r->zoffset + z : 0},
                                    .imageExtent = {w, h, 1}};
    }
    VkCommandBuffer cb = vk_upload_begin();
    vmaFlushAllocation(c->vma, sa, 0, VK_WHOLE_SIZE);
    barrier(cb, ALL_RW, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    int done = 0;
    // A BC3 image takes whole 4x4 blocks, or a part that ends on the mip's edge; a region of a 5x5 or 6x6 footprint
    // may end inside a block (vk_astc_record checks the start corner).
    uint32_t mw = e->t.d.width >> g->level, mh = e->t.d.height >> g->level;
    int bc_cut = e->f.astc_bc && (((w & 3) && (uint32_t)r->xoffset + w != (mw ? mw : 1)) ||
                                  ((h & 3) && (uint32_t)r->yoffset + h != (mh ? mh : 1)));
    if (agpu && !bc_cut) {   // bc_cut: nothing recorded, logged below as not decoded
        VkAstcRegion *ar = calloc((size_t)g->nz, sizeof *ar);
        for (int z = 0; z < g->nz; z++)
            ar[z] = (VkAstcRegion){.src = slice * (size_t)z, .w = w, .h = h, .slices = 1, .level = rg[z].imageSubresource.mipLevel,
                                   .layer = rg[z].imageSubresource.baseArrayLayer, .offset = rg[z].imageOffset};
        uint64_t a0 = perf_begin();
        done = vk_astc_record(cb, sb, slice * (size_t)g->nz, ar, g->nz, e->f.astc_bw, e->f.astc_bh, e->f.astc_srgb, e->f.astc_bc,
                              e->pub.image, aspect, VK_NULL_HANDLE);
        perf_end(PERF_ASTC, a0);
        free(ar);
    }
    if (!agpu) vkCmdCopyBufferToImage(cb, sb, e->pub.image, VK_IMAGE_LAYOUT_GENERAL, (uint32_t)g->nz, rg);
    else if (!done) log_limited(&n_astc, "WriteTexels of ASTC texture %#llx not decoded on the GPU: left as it was",
                                (unsigned long long)e->pub.addr);
    barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, ALL_RW);
    vk_upload_end();
    vk_upload_flush(1);
    free(rg);
    vmaDestroyBuffer(c->vma, sb, sa);
    return 1;
}

static void zero_rows(const Region *g, uint8_t *dst) {
    for (int z = 0; z < g->nz; z++)
        for (size_t y = 0; y < g->hb; y++) memset(dst + (size_t)z * g->img + y * g->row, 0, g->wb);
}

int vk_texture_read_image(const tex_t *t, const view_t *v, const NVNcopyRegion *r, uint8_t *dst, uint64_t row, uint64_t img) {
    Region g;
    if (!t || !region_of(t, v, r, row, img, &g)) return 0;
    lock();
    Tex *e = entry_of(t);
    int ok = 0;
    if (e) {
        make_fresh(e);
        ok = image_texels(e, &g, r, dst, 0);
    }
    unlock();
    if (!ok) zero_rows(&g, dst);
    return ok;
}

// ReadTexels: the pool bytes are the content of a mirrored texture the GPU has not rendered into; every other texture
// is read back from its image.
void vk_read_texels(const tex_t *t, const view_t *v, const NVNcopyRegion *r, uint8_t *dst, uint64_t row, uint64_t img) {
    static volatile int32_t n_bad;
    Region g;
    if (!region_of(t, v, r, row, img, &g)) {
        log_limited(&n_bad, "ReadTexels: invalid region %d,%d,%d %dx%dx%d (format %#x)", r->xoffset, r->yoffset,
                    r->zoffset, r->width, r->height, r->depth, t->d.format);
        return;
    }
    lock();
    Tex *e = entry_of(t);
    int ok = 0;
    if (e && e->pub.mirrored && !e->pub.gpu_owned) ok = storage_texels(t, &g, r, dst, 0);
    else if (e) {
        make_fresh(e);
        ok = image_texels(e, &g, r, dst, 0);
    }
    unlock();
    if (!ok) zero_rows(&g, dst);
}

// WriteTexels: a mirrored texture gets the texels in its pool bytes (and in its image once the GPU owns it); every
// other texture in its image.
void vk_write_texels(const tex_t *t, const view_t *v, const NVNcopyRegion *r, const uint8_t *src, uint64_t row, uint64_t img) {
    static volatile int32_t n_bad;
    Region g;
    if (!region_of(t, v, r, row, img, &g) || (v && v->set_format && v->format != t->d.format)) {
        log_limited(&n_bad, "WriteTexels: invalid region %d,%d,%d %dx%dx%d or view format (format %#x)", r->xoffset,
                    r->yoffset, r->zoffset, r->width, r->height, r->depth, t->d.format);
        return;
    }
    lock();
    Tex *e = entry_of(t);
    if (e && e->pub.mirrored) {
        if (!storage_texels(t, &g, r, (uint8_t *)src, 1))
            log_limited(&n_bad, "WriteTexels: region outside level %u of texture %#llx", g.level, (unsigned long long)e->pub.addr);
        vk_guest_written((uintptr_t)t->mem, t->L.size);
        if (e->pub.gpu_owned) image_texels(e, &g, r, (uint8_t *)src, 1);
        else if (e->uploaded) e->wt_n++;
    } else if (e) {
        image_texels(e, &g, r, (uint8_t *)src, 1);
    }
    unlock();
}

// ---------------------------------------------------------------- CPU reads of GPU-written storage
// A mirrored texture the GPU rendered into holds its texels in its image only; its storage keeps what the CPU last put
// there. The game may still read that storage with the CPU: TotK moves its generated grass maps between pools with
// memcpy once the GPU is done, and the copy then carried zeros. Such a read writes the image back first.
//
// Every level and layer of every texture in v is copied to staging in one upload batch with one wait: one GPU round
// trip per level and layer took 0.1 ms each, and a 260x260 grass map has 9 levels. The storage is then written in the
// order of v, so where two textures share storage the later one wins.
typedef struct {
    Tex *e;
    Region g;
    NVNcopyRegion r;
    TexRead t;
} WbPart;

static void write_back_n(Tex **v, int n) {
    int np = 0, cap = 0;
    WbPart *p = NULL;
    for (int i = 0; i < n && mem_result_watch_on() && cfg_env("REKARINA_RESULT_WATCH_TEX"); i++)   // DEBUG-RESULTWATCH: the pages open for the write-back
        if (v[i]->t.L.size <= 65536) mem_result_release(v[i]->pub.addr, v[i]->t.L.size);
    for (int i = 0; i < n; i++) {
        Tex *e = v[i];
        const nvn_texdesc *d = &e->t.d;
        int linear = nvn_tex_is_linear(d), is3d = d->target == NVN_TEXTURE_TARGET_3D;
        uint32_t levels = linear ? 1 : e->pub.levels, layers = linear || is3d ? 1 : e->pub.layers;
        for (uint32_t lv = 0; lv < levels; lv++)
            for (uint32_t ly = 0; ly < layers; ly++) {
                if (np == cap) {
                    cap = cap ? cap * 2 : 16;
                    if (!(p = realloc(p, sizeof *p * (size_t)cap))) rt_fatal("vk: out of memory for a texture write-back");
                }
                WbPart *q = &p[np];
                view_t vw = {.set_levels = 1, .set_layers = 1, .base_level = (uint8_t)lv, .min_layer = (uint16_t)ly};
                q->e = e;
                q->r = (NVNcopyRegion){.width = (int)mip(d->width, lv),
                                       .height = d->target == 0 || d->target == 3 ? 1 : (int)mip(d->height, lv),
                                       .depth = is3d ? (int)mip(d->depth, lv) : 1};
                if (region_of(&e->t, &vw, &q->r, 0, 0, &q->g) && texels_read_record(e, &q->g, &q->r, &q->t)) np++;
            }
    }
    if (np) vk_upload_flush(1);
    for (int i = 0; i < np; i++) {
        WbPart *q = &p[i];
        uint8_t *buf = malloc(q->g.img * (size_t)q->g.nz);
        if (!buf) rt_fatal("vk: out of memory for a texture write-back");
        texels_read_take(q->e, &q->g, &q->t, buf);
        storage_texels(&q->e->t, &q->g, &q->r, buf, 1);
        free(buf);
    }
    free(p);
    for (int i = 0; i < n; i++) v[i]->gpu_dirty = 0;
    {   // DEBUG-LUMLOG: REKARINA_LUMLOG=1: the 1x1 RGBA32F targets the CPU reads back (OoT's auto-exposure luminance), every
        // 10th write-back of them
        static int on = -1, k;
        if (on < 0) on = cfg_get("REKARINA_LUMLOG") != NULL;
        for (int i = 0; on && i < n; i++)
            if (v[i]->t.d.format == 0x2e && v[i]->t.d.width == 1 && v[i]->t.d.height == 1 && !(k++ % 300)) {
                const float *x = (const float *)(uintptr_t)v[i]->pub.addr;
                rt_log("LUMLOG writeback#%d frame %llu %#llx: %g %g %g %g", k,
                       (unsigned long long)__atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED),
                       (unsigned long long)v[i]->pub.addr, x[0], x[1], x[2], x[3]);
            }
    }
}

// Mirrored textures overlapping [addr, addr+size) that the GPU wrote since their last write-back.
static int owned_dirty(uint64_t addr, uint64_t size, Tex **hit) {
    int n = 0;
    for (int i = owned_lower(addr > max_owned ? addr - max_owned : 0); i < nowned && owned[i]->pub.addr < addr + size; i++) {
        Tex *e = owned[i];
        if (__atomic_load_n(&e->gpu_dirty, __ATOMIC_RELAXED) && e->pub.addr + e->t.L.size > addr) {
            if (!hit) return 1;
            hit[n++] = e;
        }
    }
    return n;
}

// A storage image may be written by the shaders of the submission being recorded (texture lock held, vk_exec_draw.c
// tex_info). Its storage is not marked GPU-owned: only the write fence for ReadTexels.
// The colour of one of the game's fill-in textures: a CPU-filled RGBA8 texture of at most 8x8 texels (OoT keeps 4x4
// ones in black, white and clear, and binds them to whatever a material leaves empty, a 3D or cube sampler included).
// 1 with the first texel's bytes in rgba; 0 for any other texture. Texture lock held.
int vk_texture_fill_colour_locked(const VkTexImage *img, uint8_t rgba[4]) {
    const Tex *e = (const Tex *)img;   // pub is the entry's first member
    const nvn_texdesc *d = &e->t.d;
    int bw, bh;
    if (img->gpu_owned || img->gone || !e->t.mem || img->samples > 1 || img->width > 8 || img->height > 8 || img->depth > 1 ||
        (img->format != VK_FORMAT_R8G8B8A8_UNORM && img->format != VK_FORMAT_R8G8B8A8_SRGB) ||
        nvn_fmt_bpb(d->format, &bw, &bh) != 4 || bw != 1 || bh != 1)
        return 0;
    memcpy(rgba, (const uint8_t *)(uintptr_t)e->t.mem, 4);
    return 1;
}

void vk_texture_storage_bound_locked(const VkTexImage *img) {
    Tex *e = (Tex *)img;   // pub is the first member
    uint64_t f = vk_exec_recording_fence();
    if (f > e->wr_fence) e->wr_fence = f;
    note_writer(e);
}

uint64_t vk_texture_last_write(const tex_t *t) {
    lock();
    Tex *e = entry_of(t);
    uint64_t f = e ? e->wr_fence : 0;
    unlock();
    return f;
}

size_t vk_texture_texel_span(const tex_t *t, const view_t *v, const NVNcopyRegion *r, uint64_t row, uint64_t img) {
    Region g;
    if (!region_of(t, v, r, row, img, &g)) return 0;
    return (size_t)(g.nz - 1) * g.img + (g.hb - 1) * g.row + g.wb;
}

int vk_texture_cpu_stale(uint64_t addr, uint64_t size) {
    if (!size || !__atomic_load_n(&nowned, __ATOMIC_ACQUIRE)) return 0;
    plat_mutex_lock_shared(&owned_lock);
    int dirty = owned_dirty(addr, size, NULL);
    plat_mutex_unlock_shared(&owned_lock);
    return dirty;
}

// The newest submission recorded to write a GPU-written mirrored texture overlapping [addr, addr+size); 0: none.
uint64_t vk_texture_dirty_fence(uint64_t addr, uint64_t size) {
    if (!size || !__atomic_load_n(&nowned, __ATOMIC_ACQUIRE)) return 0;
    lock();
    uint64_t f = 0;
    for (int i = owned_lower(addr > max_owned ? addr - max_owned : 0); i < nowned && owned[i]->pub.addr < addr + size; i++) {
        const Tex *e = owned[i];
        if (!e->gpu_dirty || e->pub.addr + e->t.L.size <= addr) continue;
        if (e->gpu_fence > f) f = e->gpu_fence;
        if (e->wr_fence > f) f = e->wr_fence;
    }
    unlock();
    return f;
}

// The image is the newest content only while nothing wrote the storage since the GPU render: TotK finalizes grass maps,
// reuses their storage for new data and memcpys that data on, and writing the old image back first handed the copy
// a stale map or stale resource bytes (wrong textures in the distance). Every hit is checked before any is written
// back, since a write-back is itself a CPU write to storage another hit may share; the hits then go back oldest render
// first, so where two descriptors share storage the newer render wins.
// Readbacks (vk_texture_readback_hold). OoT tests the sun's visibility once after a load (a point draw
// into a 1x1 target, copied into CPU memory) and reads the copy in the next frame without a sync; on the console the
// GPU, a frame behind, has not run the copy by then, so the game keeps the copy's initial value (the sun counts as
// occluded under the forest canopy) and lights the scene with it for good. Delivered at once (the present waiting
// for the GPU, or a write-back on that read), the sun counted as visible and the sky, fog and aerial perspective came
// out 2.4 times too bright: the long-standing haze (docs/oot-notes.md). REKARINA_READBACK_LAG=0 delivers at once.
static int rb_lag = -1;
void vk_texture_readback_hold(const tex_t *t) {
    if (rb_lag < 0) rb_lag = rt_knob("READBACK_LAG", "0");   // DEBUG: on (1) it changed nothing visible
    if (!rb_lag) return;
    lock();
    Tex *e = entry_of(t);
    if (e) e->rb_hold = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) + 2;   // recorded in frames + 1
    unlock();
}
static int rb_held(const Tex *e) { return e->lagged || e->rb_hold > __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED); }

void vk_texture_write_back(uint64_t addr, uint64_t size) {
    lock();
    Tex **hit = malloc(sizeof *hit * (size_t)(nowned + 1));
    if (!hit) rt_fatal("vk: out of memory for a texture write-back");
    int n = owned_dirty(addr, size, hit), k = 0;   // owned[] cannot change while tex_lock is held
    for (int i = 0; i < n; i++) {
        uint32_t now;
        if (rb_held(hit[i])) {   // a readback the console's GPU has not delivered yet: the CPU reads the old bytes
            __atomic_store_n(&small_pending, 1, __ATOMIC_RELEASE);
            continue;
        }
        if (storage_rewritten(hit[i], hit[i]->gpu_gen, &now) == 1) hit[i]->gpu_dirty = 0;
        else hit[k++] = hit[i];
    }
    for (int i = 1; i < k; i++)
        for (int j = i; j > 0 && hit[j - 1]->gpu_seq > hit[j]->gpu_seq; j--) {
            Tex *x = hit[j];
            hit[j] = hit[j - 1], hit[j - 1] = x;
        }
    write_back_n(hit, k);
    for (int i = 0; i < k; i++) perf_count(PERF_CNT_WB);
    for (int i = k; i < n; i++) perf_count(PERF_CNT_WB_SKIP);
    free(hit);
    unlock();
}

// TotK's grass system renders a 128x128 RGBA8 map of cut and trampled grass into a CPU-cached pool every frame and its
// CPU code reads the pool bytes without nvnMemoryPoolInvalidateMappedRange (cut reactions: blades, insects, the cut
// itself). On the Switch the bytes are there once the frame's fence has passed. So when the
// CPU waits for a fence, mirrored targets rendered by work up to that fence go back to their storage (wb_wanted:
// targets over wb_limit only when not drawn every frame), unless the CPU rewrote it since. Targets drawn every frame
// still go back on an invalidate, memcpy or ReadTexels.
int vk_texture_small_dirty(void) { return __atomic_load_n(&small_pending, __ATOMIC_ACQUIRE); }

// REKARINA_TRACE=gfx: a dirty target the fence write-back leaves alone because it is over the limit and drawn every frame
// (what a CPU read after the wait would miss), or sends back although it is over the default limit. One line per
// target address and outcome; the table is small and a full one stops logging.
static void wb_note(const Tex *e, int skipped) {
    static uint64_t seen[2][64];
    static int n_seen[2];
    for (int i = 0; i < n_seen[skipped]; i++)
        if (seen[skipped][i] == e->pub.addr) return;
    if (n_seen[skipped] == 64) return;
    seen[skipped][n_seen[skipped]++] = e->pub.addr;
    const pool_t *p = e->t.pool;
    unsigned fl = p ? p->flags : 0;
    rt_log("vk: fence write-back %s %#llx %ux%u fmt %#x size %llu pool %s (limit %llu)",
           skipped ? "skipped-drawn-every-frame" : "over-default", (unsigned long long)e->pub.addr, e->t.d.width, e->t.d.height,
           e->t.d.format, (unsigned long long)e->t.L.size,
           !p ? "none" : (fl & NVN_MEMORY_POOL_FLAGS_CPU_CACHED) ? "cpu-cached" : (fl & NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED) ? "cpu-uncached" : "other",
           (unsigned long long)wb_limit);
}

void vk_texture_write_back_small(uint64_t fence) {
    lock();
    wb_cfg_init();
    __atomic_store_n(&small_pending, 0, __ATOMIC_RELAXED);
    plat_mutex_lock_shared(&owned_lock);
    int n = 0, later = 0;
    for (int i = 0; i < nowned; i++) n += owned[i]->gpu_dirty && wb_wanted(owned[i]);
    Tex **hit = n ? malloc(sizeof *hit * (size_t)n) : NULL;
    if (n && !hit) rt_fatal("vk: out of memory for a texture write-back");
    n = 0;
    for (int i = 0; i < nowned; i++) {
        Tex *e = owned[i];
        if (!e->gpu_dirty) continue;
        if (!wb_wanted(e)) {
            if (wb_trace && e->gpu_fence <= fence) {
                wb_note(e, 1);
                perf_count(PERF_CNT_WB_SKIP);
            }
            continue;
        }
        if (e->gpu_fence > fence || rb_held(e)) later = 1;   // held readbacks go back at a later sync
        else {
            if (wb_trace && e->t.L.size > WB_SMALL_DEFAULT) wb_note(e, 0);
            hit[n++] = e;
        }
    }
    plat_mutex_unlock_shared(&owned_lock);
    if (later) __atomic_store_n(&small_pending, 1, __ATOMIC_RELEASE);
    int k = 0;
    for (int i = 0; i < n; i++) {
        uint32_t now;
        if (storage_rewritten(hit[i], hit[i]->gpu_gen, &now) == 1) hit[i]->gpu_dirty = 0;
        else hit[k++] = hit[i];
    }
    for (int i = 1; i < k; i++)
        for (int j = i; j > 0 && hit[j - 1]->gpu_seq > hit[j]->gpu_seq; j--) {
            Tex *x = hit[j];
            hit[j] = hit[j - 1], hit[j - 1] = x;
        }
    write_back_n(hit, k);
    for (int i = 0; i < k; i++) perf_count(PERF_CNT_WB);
    for (int i = k; i < n; i++) perf_count(PERF_CNT_WB_SKIP);
    free(hit);
    unlock();
}

// ---------------------------------------------------------------- fence readback
// OoT reads some GPU results without an NVN sync, a frame or so after rendering them: the 1x1 luminance
// its auto exposure takes the next frame, among others. Here a render target's bytes reached its storage only when the
// CPU waited for a fence (vk_texture_write_back_small) or read the storage through NVN (vk_texture_write_back), and
// such a write-back is a readback on the graphics queue that waits for everything queued before it. Whenever the GPU
// ran late the game read old luminance, and while the camera moved every other frame came out white (with every
// present waiting for the GPU, REKARINA_PRESENT_FINISH=1: none in 699 shots). So the small targets (wb_wanted, at most
// FB_MAX bytes, plain formats) that a submission renders are copied into host buffers at the end of that
// submission's own command buffer, and the completion callback of its fence puts the bytes into their storage before
// the fence counts as done (vk_sync.c): what the console's GPU writes into memory as it renders, with no CPU wait.
// The target stays gpu_dirty; a later sync wait or NVN read finds its storage written since the render
// (storage_rewritten) and leaves it. Off by default (REKARINA_FENCE_READBACK=1): the white frames went with the blocking
// acquire (nvn_api_obj.c nvnWindowAcquireTexture) and stayed without it, with this on or off.
#define FB_MAX (64u << 10)
typedef struct { tex_t t; Region g; NVNcopyRegion r; VkBuffer sb; VmaAllocation sa; const uint8_t *stg; } FbPart;
typedef struct FbJob { struct FbJob *next; uint64_t frame; volatile int done; int n; FbPart p[]; } FbJob;
static int fb_on = -1;

// Frame-latency readbacks (REKARINA_READBACK_FRAMES=<k>, default 1 for OoT, 0 elsewhere). The console's GPU
// runs about a frame behind its CPU, and OoT's auto exposure is tuned to that: the CPU reads the 1x1
// luminance of its small targets (after nvnMemoryPoolInvalidateMappedRange) and takes whatever the GPU has written by
// then, the result of the frame before. Here such a read used to write back the newest render at once (the GPU done
// with it, or waited for), the controller settled 2.4 times brighter (sun x exposure ~23 instead of ~10) and the forest
// came out hazy and washed out against the publisher's own screenshots; with a frame in flight it came out right but not
// reliably. Now the small targets a submission renders are staged at its end (the fence readback below), and their
// bytes go into storage only once k more frames have been presented after the one that rendered them, in render
// order; the NVN read and fence-wait write-backs leave such a target alone (rb_held). The same latency in every present
// mode, whatever the GPU's own timing.
static int rb_frames = -1;
static int readback_frames(void) {
    if (rb_frames < 0) {
        const char *e = cfg_get("REKARINA_READBACK_FRAMES");
#ifdef GAME_OOT
        rb_frames = e && *e ? atoi(e) : 1;
#else
        rb_frames = e && *e ? atoi(e) : 0;
#endif
        if (rb_frames < 0) rb_frames = 0;
        if (rb_frames > 4) rb_frames = 4;
        if (rb_frames) rt_log("vk: small render targets reach their storage %d frame(s) after their render (REKARINA_READBACK_FRAMES)", rb_frames);
    }
    return rb_frames;
}
static FbJob *dq_head, *dq_tail;   // dq_lock: staged jobs in render order
static PlatMutex dq_lock = PLAT_MUTEX_INIT;

static void fb_write(FbJob *j) {
    VmaAllocator vma = vk_ctx()->vma;
    for (int i = 0; i < j->n; i++) {
        FbPart *q = &j->p[i];
        vmaInvalidateAllocation(vma, q->sa, 0, VK_WHOLE_SIZE);
        // the copy's rows are tight (wb bytes), as region_of makes row and img without a caller pitch
        mem_thaw((uint64_t)(uintptr_t)q->t.mem, q->t.L.size);
        storage_texels(&q->t, &q->g, &q->r, (uint8_t *)(uintptr_t)q->stg, 1);
        vmaDestroyBuffer(vma, q->sb, q->sa);
    }
    free(j);
}

// The staged jobs that are done and due at frame count `now`, in order (a job not done yet holds the later ones back).
static void fb_deliver(uint64_t now) {
    plat_mutex_lock(&dq_lock);
    while (dq_head && __atomic_load_n(&dq_head->done, __ATOMIC_ACQUIRE) &&
           dq_head->frame + 1 + (uint64_t)rb_frames <= now) {
        FbJob *j = dq_head;
        dq_head = j->next;
        if (!dq_head) dq_tail = NULL;
        fb_write(j);
    }
    plat_mutex_unlock(&dq_lock);
}

void vk_texture_readback_tick(uint64_t frames) {   // a present: frames is the new gfx_counters.frames
    if (rb_frames > 0) fb_deliver(frames);
}

static void fb_done(void *arg) {   // completion thread, before the fence counts as done
    FbJob *j = arg;
    if (readback_frames() > 0) {
        __atomic_store_n(&j->done, 1, __ATOMIC_RELEASE);
        fb_deliver(__atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED));
        return;
    }
    fb_write(j);
}

int vk_texture_fence_readback(VkCommandBuffer cb, uint64_t fence) {
    if (fb_on < 0) fb_on = rt_knob("FENCE_READBACK", readback_frames() ? "1" : "0");
    if (!fb_on || !__atomic_load_n(&small_pending, __ATOMIC_ACQUIRE)) return 0;
    lock();
    wb_cfg_init();
    FbJob *j = NULL;
    int n = 0, cap = 0, rec = 0;
    plat_mutex_lock_shared(&owned_lock);
    for (int i = 0; i < nowned; i++) {
        Tex *e = owned[i];
        VkImageAspectFlags aspect;
        if (!e->gpu_dirty || e->gpu_fence != fence || e->t.L.size > FB_MAX || !wb_wanted(e) || e->f.astc_bw ||
            e->f.expand_rgb || !read_aspect(e, &aspect))
            continue;
        const nvn_texdesc *d = &e->t.d;
        int linear = nvn_tex_is_linear(d), is3d = d->target == NVN_TEXTURE_TARGET_3D;
        uint32_t levels = linear ? 1 : e->pub.levels, layers = linear || is3d ? 1 : e->pub.layers;
        for (uint32_t lv = 0; lv < levels; lv++)
            for (uint32_t ly = 0; ly < layers; ly++) {
                FbPart q = {.t = e->t};
                view_t vw = {.set_levels = 1, .set_layers = 1, .base_level = (uint8_t)lv, .min_layer = (uint16_t)ly};
                q.r = (NVNcopyRegion){.width = (int)mip(d->width, lv),
                                      .height = d->target == 0 || d->target == 3 ? 1 : (int)mip(d->height, lv),
                                      .depth = is3d ? (int)mip(d->depth, lv) : 1};
                if (!region_of(&e->t, &vw, &q.r, 0, 0, &q.g) || q.g.nz > 8) continue;
                size_t slice = q.g.wb * q.g.hb, bytes = slice * (size_t)q.g.nz;
                VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = bytes,
                                         .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
                VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                              .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                                                       VMA_ALLOCATION_CREATE_MAPPED_BIT};
                VmaAllocationInfo info;
                if (vmaCreateBuffer(vk_ctx()->vma, &bi, &ai, &q.sb, &q.sa, &info) != VK_SUCCESS) continue;
                if (!info.pMappedData) {
                    vmaDestroyBuffer(vk_ctx()->vma, q.sb, q.sa);
                    continue;
                }
                q.stg = info.pMappedData;
                VkBufferImageCopy rg[8];
                for (int z = 0; z < q.g.nz; z++)
                    rg[z] = (VkBufferImageCopy){.bufferOffset = slice * (size_t)z,
                                                .imageSubresource = {aspect, q.g.level, q.g.is3d ? 0 : q.g.layer0 + (uint32_t)z, 1},
                                                .imageOffset = {0, 0, q.g.is3d ? z : 0},
                                                .imageExtent = {(uint32_t)q.r.width, (uint32_t)q.r.height, 1}};
                if (!rec) barrier(cb, ALL_RW, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT), rec++;
                vkCmdCopyImageToBuffer(cb, e->pub.image, VK_IMAGE_LAYOUT_GENERAL, q.sb, (uint32_t)q.g.nz, rg);
                rec++;
                if (n == cap) {
                    cap = cap ? cap * 2 : 8;
                    FbJob *nj = realloc(j, sizeof *j + sizeof(FbPart) * (size_t)cap);
                    if (!nj) rt_fatal("vk: out of memory for a fence readback");
                    j = nj;
                }
                j->p[n++] = q;
                if (rb_frames > 0) e->lagged = 1;   // its bytes go to storage through the staged jobs only
            }
    }
    plat_mutex_unlock_shared(&owned_lock);
    unlock();
    if (!n) return rec;
    barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
            VK_ACCESS_2_HOST_READ_BIT);
    rec++;
    j->n = n;
    j->next = NULL, j->done = 0, j->frame = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    if (rb_frames > 0) {
        plat_mutex_lock(&dq_lock);
        if (dq_tail) dq_tail->next = j;
        else dq_head = j;
        dq_tail = j;
        plat_mutex_unlock(&dq_lock);
    }
    vk_completion_add(fence, fb_done, j);
    static int said;
    if (!said++) rt_log("vk: fence readback: small render targets go back to their storage at their fence (first: %d parts)", n);
    return rec;
}

// ---------------------------------------------------------------- samplers (LAS textures.c)
static VkCompareOp compare_op(uint32_t f) {
    switch (f) {
    case NVN_COMPARE_FUNC_NEVER: return VK_COMPARE_OP_NEVER;
    case NVN_COMPARE_FUNC_LESS: return VK_COMPARE_OP_LESS;
    case NVN_COMPARE_FUNC_EQUAL: return VK_COMPARE_OP_EQUAL;
    case NVN_COMPARE_FUNC_LEQUAL: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case NVN_COMPARE_FUNC_GREATER: return VK_COMPARE_OP_GREATER;
    case NVN_COMPARE_FUNC_NOTEQUAL: return VK_COMPARE_OP_NOT_EQUAL;
    case NVN_COMPARE_FUNC_GEQUAL: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    default: return VK_COMPARE_OP_ALWAYS;
    }
}

static struct { int init, mirror_clamp_edge, minmax, border_no_format; float aniso; } sf;   // aniso: REKARINA_ANISO, 0 = the game's

static void sampler_features(VkCtx *c) {
    if (sf.init) return;
    VkPhysicalDeviceCustomBorderColorFeaturesEXT cbf = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT};
    VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                            .pNext = c->has_custom_border_color ? &cbf : NULL};
    VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f12};
    vkGetPhysicalDeviceFeatures2(c->phys, &f2);
    sf.mirror_clamp_edge = f12.samplerMirrorClampToEdge;
    sf.minmax = f12.samplerFilterMinmax;
    sf.border_no_format = c->has_custom_border_color && cbf.customBorderColors && cbf.customBorderColorWithoutFormat;
    const char *an = cfg_value("REKARINA_ANISO");
    sf.aniso = strcmp(an, "game") ? (float)atoi(an) : 0;
    rt_log("vk: anisotropic filtering: %s", sf.aniso > 1 ? an : "the game's");
    sf.init = 1;
}

static VkSampler make_sampler(VkCtx *c, const samp_par *d) {
    sampler_features(c);
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = d->magf == NVN_MAG_FILTER_LINEAR ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    uint32_t mf = d->minf;
    si.minFilter = mf == 1 || mf == 3 || mf == 5 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    si.mipmapMode = mf == 4 || mf == 5 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    VkSamplerAddressMode modes[3];
    const uint8_t wrap[3] = {d->ws, d->wt, d->wr};
    int border = 0;
    for (int i = 0; i < 3; i++) {
        switch (wrap[i]) {
        case NVN_WRAP_MODE_CLAMP: case NVN_WRAP_MODE_CLAMP_TO_EDGE: modes[i] = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; break;
        case NVN_WRAP_MODE_MIRROR_CLAMP: case NVN_WRAP_MODE_MIRROR_CLAMP_TO_EDGE:
            modes[i] = sf.mirror_clamp_edge ? VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
            break;
        case NVN_WRAP_MODE_MIRROR_CLAMP_TO_BORDER: case NVN_WRAP_MODE_CLAMP_TO_BORDER:
            modes[i] = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
            border = 1;
            break;
        case NVN_WRAP_MODE_MIRRORED_REPEAT: modes[i] = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT; break;
        default: modes[i] = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        }
    }
    si.addressModeU = modes[0], si.addressModeV = modes[1], si.addressModeW = modes[2];
    si.minLod = d->lod_min;
    si.maxLod = d->lod_max > 1000 ? VK_LOD_CLAMP_NONE : d->lod_max;
    if (mf <= NVN_MIN_FILTER_LINEAR) si.maxLod = si.minLod + 0.25f;   // not mipmapped: stay on the base level
    if (si.maxLod < si.minLod) si.maxLod = si.minLod;
    float lim = c->props.limits.maxSamplerLodBias;
    si.mipLodBias = fmaxf(-lim, fminf(lim, d->lod_bias));
    // REKARINA_ANISO raises the level for filtered, mipmapped colour samplers that repeat on U and V: the surface textures,
    // which tile, so ground and walls seen at a shallow angle stay sharp into the distance. Depth-compare (shadow)
    // samplers, nearest or unmipmapped ones (data maps, LUTs) and clamped ones keep the game's level: clamped samplers
    // read screen-sized buffers, whose coordinates can stretch steeply at an object's edge, where anisotropic taps reach
    // across it and leave a speckled fringe (the inventory's character preview).
    float aniso = d->aniso;
    // The game's own wrap modes: a mirror-clamp one maps to MIRRORED_REPEAT where the device lacks mirror-clamp-to-edge.
    int tiled = (d->ws == NVN_WRAP_MODE_REPEAT || d->ws == NVN_WRAP_MODE_MIRRORED_REPEAT) &&
                (d->wt == NVN_WRAP_MODE_REPEAT || d->wt == NVN_WRAP_MODE_MIRRORED_REPEAT);
    if (sf.aniso > aniso && tiled && !d->cmp_mode && si.magFilter == VK_FILTER_LINEAR && si.minFilter == VK_FILTER_LINEAR &&
        (mf == NVN_MIN_FILTER_LINEAR_MIPMAP_NEAREST || mf == NVN_MIN_FILTER_LINEAR_MIPMAP_LINEAR))
        aniso = sf.aniso;
    if (aniso > 1) {
        si.anisotropyEnable = VK_TRUE;
        si.maxAnisotropy = fminf(aniso, c->props.limits.maxSamplerAnisotropy);
    }
    if (d->cmp_mode) {
        si.compareEnable = VK_TRUE;
        si.compareOp = compare_op(d->cmp_func);
        // A depth-compare sampler set to nearest returns 0 or 1 per tap, and the game's shaders smooth shadow edges by
        // a few taps spaced for its own shadow map texels. With REKARINA_SHADOW_SCALE those taps cover a fraction of the
        // area and the edges turn hard and stepped; linear filtering makes every tap a 2x2 PCF instead.
        if (shadow_scale() > 1) si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    }
    VkSamplerCustomBorderColorCreateInfoEXT cb = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT};
    VkSamplerReductionModeCreateInfo rm = {.sType = VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO};
    const void **tail = &si.pNext;
    if (border) {
        const float *b = d->border;
        if (b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 0) si.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        else if (b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 1) si.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
        else if (b[0] == 1 && b[1] == 1 && b[2] == 1 && b[3] == 1) si.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        else if (sf.border_no_format) {
            memcpy(cb.customBorderColor.float32, b, 16);
            si.borderColor = VK_BORDER_COLOR_FLOAT_CUSTOM_EXT;
            *tail = &cb;
            tail = &cb.pNext;
        } else si.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    }
    if (d->reduction && sf.minmax && !si.compareEnable) {
        rm.reductionMode = d->reduction == NVN_SAMPLER_REDUCTION_MIN ? VK_SAMPLER_REDUCTION_MODE_MIN : VK_SAMPLER_REDUCTION_MODE_MAX;
        *tail = &rm;
    }
    VkSampler s;
    VkResult r = vkCreateSampler(c->device, &si, NULL, &s);
    if (r != VK_SUCCESS) rt_fatal("vk: vkCreateSampler failed (%d)", (int)r);
    return s;
}

void vk_sampler_register(int id, const samp_par *s) {
    if (id < 0 || id >= (1 << 12)) rt_fatal("vk: sampler slot %d out of range", id);
    lock();
    if (id >= nsslots) {
        int n = nsslots ? nsslots : 256;
        while (n <= id) n *= 2;
        SampSlot *p = realloc(sslots, sizeof *p * (size_t)n);
        if (!p) rt_fatal("vk: out of memory for sampler slots");
        memset(p + nsslots, 0, sizeof *p * (size_t)(n - nsslots));
        sslots = p, nsslots = n;
    }
    sslots[id].p = *s;
    sslots[id].s = sslots[id].sp = sslots[id].sc = VK_NULL_HANDLE;
    sslots[id].used = 1;
    if (id >= 0 && id < 4096) samp_reg[id] = 1;
    samp_gen++;
    unlock();
}

// DLSS upscaling (vk_sampler_dlss_bias): the scene's material textures, the filtered mipmapped ones that repeat, are
// sampled for the output's resolution; clamped ones (post effects, LUTs, the UI) keep the game's bias.
static float dlss_bias;

static int dlss_biased(const samp_par *k) {
    int rep_s = k->ws == NVN_WRAP_MODE_REPEAT || k->ws == NVN_WRAP_MODE_MIRRORED_REPEAT;
    int rep_t = k->wt == NVN_WRAP_MODE_REPEAT || k->wt == NVN_WRAP_MODE_MIRRORED_REPEAT;
    static int all = -1;   // DEBUG-BIAS: REKARINA_DLSS_BIAS_ALL=1 also takes the clamped trilinear samplers
    if (all < 0) { const char *e = cfg_get("REKARINA_DLSS_BIAS_ALL"); all = !e || atoi(e) > 0; }
    return !k->cmp_mode && k->minf == NVN_MIN_FILTER_LINEAR_MIPMAP_LINEAR && (rep_s || rep_t || all);
}

void vk_sampler_dlss_bias(float b) {
    lock();
    if (b != dlss_bias) {
        dlss_bias = b;
        for (int i = 0; i < nsslots; i++) sslots[i].s = sslots[i].sp = sslots[i].sc = VK_NULL_HANDLE;   // looked up again (the key moves)
        samp_gen++;
    }
    unlock();
}

// Caller holds the lock.
static VkSampler sampler_for(VkCtx *c, const samp_par *s) {
    samp_par k;
    memset(&k, 0, sizeof k);
    k = *s;
    if (dlss_bias != 0 && dlss_biased(&k)) k.lod_bias += dlss_bias;   // part of the key: samplers live until shutdown
    VkSampler v = (VkSampler)nvn_hm_get(&samplers, &k);
    if (!v) {
        v = make_sampler(c, &k);
        nvn_hm_put(&samplers, &k, (void *)v);
    }
    return v;
}

VkSampler vk_sampler_for(const samp_par *s) {
    VkCtx *c = vk_ctx();
    if (!c || !s) return VK_NULL_HANDLE;
    lock();
    VkSampler v = sampler_for(c, s);
    unlock();
    return v;
}

// Samplers live until shutdown, so a slot keeps the one its parameters map to until it is registered again.
VkSampler vk_sampler_by_id_locked(int id) {
    VkCtx *c = vk_ctx();
    if (!c) return VK_NULL_HANDLE;
    VkSampler v = VK_NULL_HANDLE;
    if (id >= 0 && id < nsslots && sslots[id].used) {
        SampSlot *s = &sslots[id];
        if (!s->s) s->s = sampler_for(c, &s->p);
        v = s->s;
    }
    return v;
}

// DEBUG-SAMP3D: REKARINA_SAMP3D=1 logs the filters and wrap modes of each sampler paired with a 3D texture, once per sampler.
void vk_debug_samp3d_locked(int id, int separate, int format) {
    static uint8_t seen[4096];
    if (id < 0 || id >= nsslots || id >= 4096 || seen[id] || !sslots[id].used || !cfg_env("REKARINA_SAMP3D")) return;
    seen[id] = 1;
    const samp_par *p = &sslots[id].p;
    rt_log("SAMP3D sampler %d separate %d format %d: min %u mag %u wrap %u %u %u", id, separate, format, p->minf, p->magf, p->ws, p->wt, p->wr);
}

// DEBUG-IMGWHERE: where the image of texture slot id lives (own memory or bound into pool memory, the memory type's flags).
void vk_debug_image_where_locked(int id, const char *what) {
    view_t v;
    int hv;
    const VkTexImage *img = vk_texture_by_id_locked(id, &v, &hv);
    if (!img) return rt_log("IMGWHERE %s slot %d: no image", what, id);
    const Tex *e = (const Tex *)((const char *)img - offsetof(Tex, pub));
    VmaAllocation a = e->alloc ? e->alloc : e->bound_to;
    VkMemoryPropertyFlags f = 0;
    VmaAllocationInfo info = {0};
    if (a) vmaGetAllocationInfo(vk_ctx()->vma, a, &info), vmaGetMemoryTypeProperties(vk_ctx()->vma, info.memoryType, &f);
    rt_log("IMGWHERE %s addr %#llx %ux%ux%u vk fmt %d levels %u usage %#x mirrored %d %s memtype %u flags %#x offset %#llx", what,
           (unsigned long long)img->addr, img->width, img->height, img->depth, (int)img->format, img->levels, (unsigned)img->usage,
           img->mirrored, e->alloc ? "own memory" : e->bound_to ? "pool memory" : "none", info.memoryType, (unsigned)f,
           (unsigned long long)info.offset);
}

// A depth-compare read through a sampler without a compare is undefined in Vulkan. NVIDIA's driver compares anyway; AMD's
// returns the raw depth, so the game's shadow tests come out wrong there. Such a read gets the slot's sampler with the
// compare on: the game's function when it set a valid one, LEQUAL otherwise. REKARINA_SHADOW_COMPARE=0: the game's sampler.
VkSampler vk_sampler_compare_by_id_locked(int id) {
    VkCtx *c = vk_ctx();
    if (!c || id < 0 || id >= nsslots || !sslots[id].used) return VK_NULL_HANDLE;
    SampSlot *s = &sslots[id];
    static int on = -1;
    if (on < 0) on = rt_knob("SHADOW_COMPARE", "1");
    if (s->p.cmp_mode || !on) return vk_sampler_by_id_locked(id);
    if (!s->sc) {
        samp_par k = s->p;
        k.cmp_mode = 1;
        if (k.cmp_func < NVN_COMPARE_FUNC_NEVER || k.cmp_func > NVN_COMPARE_FUNC_ALWAYS) k.cmp_func = NVN_COMPARE_FUNC_LEQUAL;
        s->sc = sampler_for(c, &k);
    }
    return s->sc;
}

VkSampler vk_sampler_point_by_id_locked(int id) {
    VkCtx *c = vk_ctx();
    if (!c || id < 0 || id >= nsslots || !sslots[id].used) return VK_NULL_HANDLE;
    SampSlot *s = &sslots[id];
    if (!s->sp) {
        samp_par k = s->p;
        k.magf = NVN_MAG_FILTER_NEAREST;
        k.minf = k.minf == NVN_MIN_FILTER_LINEAR ? NVN_MIN_FILTER_NEAREST :
                 k.minf > NVN_MIN_FILTER_LINEAR ? NVN_MIN_FILTER_NEAREST_MIPMAP_NEAREST : k.minf;
        k.aniso = 1;
        s->sp = sampler_for(c, &k);
    }
    return s->sp;
}

VkSampler vk_sampler_by_id(int id) {
    lock();
    VkSampler v = vk_sampler_by_id_locked(id);
    unlock();
    return v;
}

int vk_sampler_params(int id, samp_par *out) {
    lock();
    int ok = id >= 0 && id < nsslots && sslots[id].used;
    if (ok) *out = sslots[id].p;
    unlock();
    return ok;
}

// ---------------------------------------------------------------- statistics and shutdown
void vk_texture_stats(uint64_t *images, uint64_t *uploads, uint64_t *reuploads) {
    lock();
    if (images) *images = by_key.n;
    if (uploads) *uploads = n_uploads;
    if (reuploads) *reuploads = n_reuploads;
    unlock();
}

void vk_texture_census(VkTexCensus *s) {
    VkCtx *c = vk_ctx();
    memset(s, 0, sizeof *s);
    lock();
    for (uint32_t i = 0; i < by_key.cap; i++) {
        const Tex *e = by_key.vals[i];
        if (!e) continue;
        uint64_t bytes = 0;
        if (e->alloc && c) {
            VmaAllocationInfo ai;
            vmaGetAllocationInfo(c->vma, e->alloc, &ai);
            bytes = ai.size;
        }
        if (e->released) s->released++, s->own_released += e->alloc != NULL, s->bytes_released += bytes;
        else s->live++, s->own_live += e->alloc != NULL, s->bytes_live += bytes;
    }
    unlock();
}

void vk_texture_shutdown(void) {
    VkCtx *c = vk_ctx();
    pm_gone(0, ~0ull, NULL, 0);
    lock();
    Tex **list = malloc(sizeof *list * (size_t)(by_key.n + 1));
    int n = 0;
    for (uint32_t i = 0; i < by_key.cap; i++)
        if (by_key.vals[i]) list[n++] = by_key.vals[i];
    for (int i = 0; i < n; i++) destroy_tex(list[i]);
    free(list);
    park_clear();
    for (uint32_t i = 0; i < samplers.cap; i++)
        if (samplers.vals[i] && c) vkDestroySampler(c->device, (VkSampler)samplers.vals[i], NULL);
    free(by_key.keys), free(by_key.vals), free(by_addr.keys), free(by_addr.vals), free(samplers.keys), free(samplers.vals);
    free(cold_keys.keys), free(cold_keys.vals);
    cold_keys = (nvn_hmap){.klen = sizeof(TexKey)}, n_cold = 0;
    by_key = (nvn_hmap){.klen = sizeof(TexKey)};
    by_addr = (nvn_hmap){.klen = 8};
    samplers = (nvn_hmap){.klen = sizeof(samp_par)};
    free(tslots), free(sslots);
    tslots = NULL, sslots = NULL, ntslots = nsslots = 0, nreleased = 0, pool_slots = 0;
    rel_head = rel_tail = NULL;
    if (sinfo) memset(sinfo, 0, sizeof *sinfo << 20);   // no slot is registered: tokens 0
    unlock();
}

// DEBUG-LIVECHECK: what slot id is and resolves to (the caller holds the texture lock or not: it is recursive)
void vk_texture_slot_log(int id) {
    lock();
    if (id < 0 || id >= ntslots || !tslots[id].used) {
        rt_log("LIVE CHECK: slot %d is not registered", id);
        unlock();
        return;
    }
    const TexSlot *s = &tslots[id];
    const Tex *e = s->ce;
    rt_log("LIVE CHECK: slot %d: texture at %#llx %ux%u fmt %#x levels %u%s, serial %u, resolved under generation %u (now %u); "
           "entry %p%s%s image %#llx addr %#llx %ux%u, bound into pool memory %d, gpu-owned %d, uploaded %d",
           id, (unsigned long long)(uintptr_t)s->t.mem, s->t.d.width, s->t.d.height, s->t.d.format, s->t.d.levels,
           s->has_view ? " (a view)" : "", (uint32_t)(sinfo[id] >> 16), s->cgen, agen[sinfo[id] & 0xffff], (const void *)e,
           e && e->released ? " RELEASED" : "", e && e->pub.gone ? " GONE" : "",
           e ? (unsigned long long)(uintptr_t)e->pub.image : 0ull, e ? (unsigned long long)e->key.addr : 0ull,
           e ? e->pub.width : 0, e ? e->pub.height : 0, e ? e->bound_to != NULL : 0, e ? e->pub.gpu_owned : 0, e ? e->uploaded : 0);
    unlock();
}

// DEBUG-WHONAMES (the device fault report): entries, kept images and slots that name the image
void vk_texture_who_names(uint64_t image) {
    lock();
    const Tex *found[8];
    int nf = 0;
    for (uint32_t i = 0; i < by_key.cap; i++) {
        const Tex *e = by_key.vals[i];
        if (!e || (uint64_t)(uintptr_t)e->pub.image != image) continue;
        if (nf < 8) found[nf++] = e;
        rt_log("  entry %p names it: addr %#llx %ux%u fmt %#x%s%s uploaded %d gpu_owned %d bound %d wr %llu", (const void *)e,
               (unsigned long long)e->key.addr, e->key.d.width, e->key.d.height, e->key.d.format, e->released ? " RELEASED" : " live",
               e->pub.gone ? " GONE" : "", e->uploaded, e->pub.gpu_owned, e->bound_to != NULL, (unsigned long long)e->wr_serial);
    }
    for (int i = 0; i < nparked; i++)
        if ((uint64_t)(uintptr_t)parked[i].image == image) rt_log("  the image cache holds it (kept for a move: %d)", (int)parked[i].moved);
    int ns = 0;
    for (int id = 0; id < ntslots; id++) {
        if (!tslots[id].used || !tslots[id].ce) continue;
        for (int k = 0; k < nf; k++)
            if (tslots[id].ce == found[k] && ns++ < 8)
                rt_log("  slot %d is resolved to entry %p (resolved under generation %u, now %u)", id, (const void *)found[k],
                       tslots[id].cgen, agen[sinfo[id] & 0xffff]);
    }
    rt_log("  texture module: %d entries name it, %d slots resolved to them", nf, ns);
    unlock();
}
