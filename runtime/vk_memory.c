// Vulkan memory of the graphics layer: NVN memory pools as Vulkan
// buffers, the virtual-pool page table, GPU address resolution, page write generations for re-uploads, and the
// upload batch (command buffer, staging buffers, deferred destruction) that vk_texture.c records into.
//
// Behaviour ported from MacNativeGames games/totk/third_party/d2r_gfx/gfx/nvn/totk_vpool.c (request checks, unmap
// semantics, 64 KiB pages) and nvn_dirty.c (generation stamps: a texture is stale when a covered page carries a
// generation newer than its upload), @ b75be8cf (MIT, Copyright (c) 2026 jhaizhou; origin d2r-mobile); and from LAS
// Recomp runtime/nvn/vulkan/exec.c:2094-2131 (vk_pages_changed: every write-watch query resets the OS bits, so each
// query stamps the pages it saw with a new generation shared by all users) and textures.c:240-270 (deferred
// destruction) @ da35b83a (MIT, Copyright (c) 2026 Peiikai). MNG aliases host VM (mach_vm_remap) and protects pages
// (fault handler); here virtual pools are a GPU-side page table over device-local memory and dirty pages come from
// GetWriteWatch (mem_written_pages), so neither is needed.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "gfx.h"
#include "ftrace.h"
#include "host.h"
#include "nvn_api.h"
#include "nvn_pool_index.h"
#include "vk.h"
#include "vk_exec.h"
#include "wb_merge_spv.h"
#include "platform/platform.h"

// Timestamps for the tracking cost, in ns; qpc_ms converts.
static uint64_t qpc(void) { return plat_now_ns(); }

static double qpc_ms(uint64_t ticks) { return (double)ticks / 1e6; }

// ---------------------------------------------------------------- pools
enum { POOL_IMPORT = 1, POOL_PHYS, POOL_VIRT };
enum { MS_NONE, MS_ON, MS_REFUSED };

typedef struct { struct PoolRec *phys; uint64_t off; } VPage;   // virtual page -> physical pool and offset

#define FLUSH_RANGES 32                // flush ranges a mirrored pool keeps between two syncs (flush_add)
typedef struct { uint64_t lo, hi; } FlushRange;

typedef struct PoolRec {
    const pool_t *key;
    int kind;
    uint32_t flags;
    uint64_t lo, size;                 // GPU address range of the pool
    uint64_t import_lo, import_hi;     // POOL_IMPORT: the imported host range (aligned outwards)
    VkBuffer buf;
    VkDeviceMemory mem;                // POOL_IMPORT
    VmaAllocation alloc;               // POOL_PHYS
    VmaAllocation alloc_tw;            // POOL_PHYS: a second block of the same size for images whose offset the first cannot
                                       // take (vk_mem_phys_twin)
    VPage *pt;                         // POOL_VIRT: size / 64 KiB entries
    uint64_t npages;
    volatile int32_t used;                // resolved at least once: the GPU may still read it at finalize
    // TOTK_TRACE=perf dirty census (vk_mem_dirty_submit), replay thread under the submit lock only:
    uint32_t bound_sub;                // last submit that bound it as a buffer (dirty_sub), 0 never
    uint32_t synced_gen;               // page generation its lazy-mirror count covers; DIRTY_UNSYNCED until scanned
    uint8_t bufbound;                  // ever bound as a vertex, index, uniform, storage or indirect buffer
    uint8_t untracked;                 // the last dirty scan found no write watch over it
    // VRAM mirror (POOL_IMPORT, "VRAM mirror" below). mstate and mbuf change on the replay thread only.
    uint8_t mstate;                    // MS_NONE until the first buffer use, then MS_ON or MS_REFUSED
    uint8_t mwhy;                      // MS_REFUSED: MR_* (MR_DEMOTED: gave its mirror up, gmem_demote_pending)
    uint32_t mtry;                     // MR_BUDGET: the frame of the refusal (vk_mem_use asks again 300 frames later)
    uint64_t up_bytes;                 // replay: CPU -> mirror bytes copied since the last mirror log (TOTK_TRACE=perf)
    uint64_t hot_bytes;                // replay: CPU -> mirror bytes this frame (hot pools, vk_mem_census)
    uint32_t hot_frames;               // frames in a row it was copied (nearly) whole
    VkBuffer mbuf;                     // MS_ON: device-local copy of [import_lo, import_hi), same offsets as buf
    VkBuffer da_of[2];                 // replay thread: the buffer da[i] is the device address of (0 buf, 1 mbuf)
    VkDeviceAddress da[2];
    VmaAllocation malloc_;
    uint32_t acc_sub;                  // replay: the last submission that synced the mirror (mirror_sync)
    uint32_t wb_sub;                   // replay: the last submission that wrote mirror bytes back to guest memory
    uint32_t mgen;                     // gen_lock: every page write stamped up to this generation is in the mirror
                                       // (or in a copy queued before its next use)
    FlushRange fl[FLUSH_RANGES + 1];   // flush_lock: nvnMemoryPoolFlushMappedRange pages not copied yet (flush_add)
    int fl_n;                          // flush_lock: ranges in fl; read without the lock to skip an empty list
    uint64_t fl_calls, fl_sum;         // flush_lock: the calls and bytes behind fl, for mirror_sync's statistics
    // Shadow storage (POOL_IMPORT in shadow mode, "shadow storage" below): buf is a host-visible copy of the guest pages.
    VmaAllocation salloc;
    uint8_t *smap;                     // its mapping; offsets as in buf (from import_lo)
    uint32_t sgen;                     // gen_lock: every page write stamped up to this generation is in the shadow
    volatile uint8_t stouched;         // on the touched list: the GPU may use it in the submission being recorded
    uint8_t shader_written;            // a shader wrote into its mirror: its uploads read guest memory late (snap_late)
} PoolRec;

static void mirror_forget(PoolRec *r);   // pool_lock exclusive: off the mirrored list
static void gmem_demote_pending(void);   // vk_mem_mirror_begin: small pools the GPU writes through the table lose their mirror
static void flush_range_add(PoolRec *r, uint64_t a, uint64_t b);   // mirror_sync: a flush range, less the GPU's pieces
static int gmem_hot_page(uint64_t a);   // DEBUG-HOTNOSYNC
static int gmem_merge_page(PoolRec *r, uint64_t x);
static int nawbr;                                             // async write-back: pending ranges (awb_note, below)
static void awb_check(PoolRec *r, uint64_t off, uint64_t size);   // a write into a range a pending write-back reads
static void awb_prune(void);   // mirror_sync: only the words the CPU changed in a GPU-written page
static int shadow_on;                    // shadow mode: imported pools get shadow storage instead (vk_mem_class_log)
static int shadow_pool(VkCtx *c, PoolRec *r, uint64_t addr, uint64_t size);
static void shadow_touch(PoolRec *r);
static void shadow_forget(PoolRec *r);
static void mirror_retiring(PoolRec *r);  // vk_pool_fini: kept until mirror_retire (or vk_mem_shutdown)
static void mirror_retire(void *arg);    // completion thread: destroys a finalized mirrored pool's record
static void grow(void **arr, int *cap, size_t elem);

static PlatMutex pool_lock = PLAT_MUTEX_INIT;
// Bumped under pool_lock (exclusive) by every change to the index or a page table: vk_mem_resolve's per-thread hits
// stay valid while it is unchanged.
static uint64_t pool_gen = 1;
// Bumped when a buffer use may get another buffer for the same span (a mirror created, refused or forgotten) and at
// every submission's start (mirror syncs are per submission). vk_mem_gen: both together.
static uint64_t mirror_gen;
static pidx pool_index;                // POOL_IMPORT, POOL_PHYS and POOL_VIRT records by GPU address
static PoolRec **phys_list;            // physical pools by key, for MapVirtual requests
static int nphys, cap_phys;
static int32_t n_imports, n_import_failures;
static PFN_vkGetMemoryHostPointerPropertiesEXT p_host_props;

static VkBufferUsageFlags pool_usage(const VkCtx *c) {
    VkBufferUsageFlags u = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                           VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT |
                           VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                           VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                           VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    if (c->has_transform_feedback)
        u |= VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT | VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_COUNTER_BUFFER_BIT_EXT;
    if (c->has_conditional_rendering) u |= VK_BUFFER_USAGE_CONDITIONAL_RENDERING_BIT_EXT;
#ifdef GAME_NS2
    // NVN2 shaders reach pools through device addresses (vk_mem_gmem_table): every pool buffer has one
    if (c->has_buffer_device_address) u |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
#endif
    return u;
}

static int memory_type(const VkCtx *c, uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < c->mem_props.memoryTypeCount; i++)
        if ((bits >> i & 1) && (c->mem_props.memoryTypes[i].propertyFlags & want) == want) return (int)i;
    for (uint32_t i = 0; i < c->mem_props.memoryTypeCount; i++)
        if (bits >> i & 1) return (int)i;
    return -1;
}

// CPU-visible storage: the guest pages themselves become the buffer's memory (VK_EXT_external_memory_host).
static int import_pool(VkCtx *c, PoolRec *r, uint64_t addr, uint64_t size) {
    if (shadow_on) return shadow_pool(c, r, addr, size);
    if (!p_host_props)
        p_host_props = (PFN_vkGetMemoryHostPointerPropertiesEXT)vkGetDeviceProcAddr(c->device, "vkGetMemoryHostPointerPropertiesEXT");
    uint64_t al = c->min_imported_host_alignment ? c->min_imported_host_alignment : 0x1000;
    r->import_lo = addr & ~(al - 1);
    r->import_hi = (addr + size + al - 1) & ~(al - 1);
    uint64_t n = r->import_hi - r->import_lo;
    VkMemoryHostPointerPropertiesEXT hp = {.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    VkResult res = p_host_props ? p_host_props(c->device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                               (void *)(uintptr_t)r->import_lo, &hp)
                                : VK_ERROR_EXTENSION_NOT_PRESENT;
    if (res != VK_SUCCESS || !hp.memoryTypeBits) {
        rt_log("vk: pool %#llx+%#llx: host pointer not importable (%d)", (unsigned long long)addr, (unsigned long long)size, (int)res);
        return 0;
    }
    VkExternalMemoryBufferCreateInfo eb = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
                                           .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &eb, .size = n,
                             .usage = pool_usage(c), .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    if ((res = vkCreateBuffer(c->device, &bi, NULL, &r->buf)) != VK_SUCCESS) {
        rt_log("vk: pool %#llx: vkCreateBuffer for the import failed (%d)", (unsigned long long)addr, (int)res);
        return 0;
    }
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(c->device, r->buf, &req);
    int type = memory_type(c, req.memoryTypeBits & hp.memoryTypeBits,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type < 0 || req.size > n) {
        rt_log("vk: pool %#llx: no memory type for the import (buffer %#x, host %#x, size %#llx of %#llx)",
               (unsigned long long)addr, req.memoryTypeBits, hp.memoryTypeBits, (unsigned long long)req.size,
               (unsigned long long)n);
        vkDestroyBuffer(c->device, r->buf, NULL);
        r->buf = VK_NULL_HANDLE;
        return 0;
    }
    VkImportMemoryHostPointerInfoEXT im = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
                                           .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                           .pHostPointer = (void *)(uintptr_t)r->import_lo};
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &im, .allocationSize = n,
                               .memoryTypeIndex = (uint32_t)type};
#ifdef GAME_NS2
    VkMemoryAllocateFlagsInfo af = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, .pNext = &im,
                                    .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    if (c->has_buffer_device_address) ai.pNext = &af;   // the buffer has SHADER_DEVICE_ADDRESS usage (pool_usage)
#endif
    if ((res = vkAllocateMemory(c->device, &ai, NULL, &r->mem)) != VK_SUCCESS ||
        (res = vkBindBufferMemory(c->device, r->buf, r->mem, 0)) != VK_SUCCESS) {
        rt_log("vk: pool %#llx+%#llx: host memory import failed (%d)", (unsigned long long)addr, (unsigned long long)size, (int)res);
        if (r->mem) vkFreeMemory(c->device, r->mem, NULL);
        vkDestroyBuffer(c->device, r->buf, NULL);
        r->buf = VK_NULL_HANDLE;
        r->mem = VK_NULL_HANDLE;
        return 0;
    }
    plat_inc32(&n_imports);
    return 1;
}

// Physical pool: device-local memory of the pool size that both buffers and optimal-tiling images can be bound to.
static int alloc_physical(VkCtx *c, PoolRec *r) {
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = r->size, .usage = pool_usage(c),
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkResult res = vkCreateBuffer(c->device, &bi, NULL, &r->buf);
    if (res != VK_SUCCESS) { rt_log("vk: physical pool: vkCreateBuffer failed (%d)", (int)res); return 0; }
    VkMemoryRequirements rb;
    vkGetBufferMemoryRequirements(c->device, r->buf, &rb);
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                            .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {256, 256, 1}, .mipLevels = 1, .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT};
    VkDeviceImageMemoryRequirements di = {.sType = VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS, .pCreateInfo = &ii};
    VkMemoryRequirements2 ri = {.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
    vkGetDeviceImageMemoryRequirements(c->device, &di, &ri);
    uint32_t bits = rb.memoryTypeBits & ri.memoryRequirements.memoryTypeBits;
    if (!bits) bits = rb.memoryTypeBits;
    VkDeviceSize align = NVN_VPOOL_PAGE;
    if (rb.alignment > align) align = rb.alignment;
    if (ri.memoryRequirements.alignment > align) align = ri.memoryRequirements.alignment;
    VkMemoryRequirements req = {.size = (rb.size + align - 1) & ~(align - 1), .alignment = align, .memoryTypeBits = bits};
    VmaAllocationCreateInfo aci = {.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT,
                                   .requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT};
    if ((res = vmaAllocateMemory(c->vma, &req, &aci, &r->alloc, NULL)) != VK_SUCCESS ||
        (res = vmaBindBufferMemory(c->vma, r->alloc, r->buf)) != VK_SUCCESS) {
        rt_log("vk: physical pool of %#llx bytes: device-local allocation failed (%d)", (unsigned long long)r->size, (int)res);
        if (r->alloc) vmaFreeMemory(c->vma, r->alloc);
        vkDestroyBuffer(c->device, r->buf, NULL);
        r->buf = VK_NULL_HANDLE;
        r->alloc = NULL;
        return 0;
    }
    if (vk_fix_zero()) {   // Switch pool memory starts zeroed; once per pool, not per image placed in it
        VkCommandBuffer cb = vk_upload_begin();
        vkCmdFillBuffer(cb, r->buf, 0, VK_WHOLE_SIZE, 0);
        VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                               .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
        VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
        vkCmdPipelineBarrier2(cb, &di);
        vk_upload_end();
    }
    return 1;
}

int vk_fix_zero(void) {
    static int on = -1;
    if (on < 0) {
        on = cfg_bool("TOTK_FIX_ZERO");
    }
    return on;
}

static void index_put(PoolRec *r) {
    __atomic_add_fetch(&pool_gen, 1, __ATOMIC_RELEASE);
    pidx_ent e = {.lo = r->lo, .hi = r->lo + r->size, .base = r->lo, .buf = r, .gpu = r->lo, .key = r->key};
    pidx_remove(&pool_index, r->key, NULL);   // an object initialized again without Finalize
    if (!pidx_insert(&pool_index, &e)) rt_fatal("vk: out of memory for the pool index");
}

static PoolRec *phys_find(const pool_t *key) {
    for (int i = 0; i < nphys; i++) if (phys_list[i]->key == key) return phys_list[i];
    return NULL;
}

// A device fault names a GPU address. The pool buffers are what shaders reach by address (the pool table,
// vk_mem_gmem_table), so the report says which of them holds the address or held it: the buffers alive now, and the
// last GONE_N destroyed with the time of their end (vk_mem_fault_report, called by vk_device_fault_report). An address
// in none of them is an image's or another buffer's.
#define GONE_N 1024
static struct { uint64_t a, n, lo, at_ns; uint8_t which, kind; } gone[GONE_N];
static uint32_t ngone;   // pool_lock

static uint64_t rec_span(const PoolRec *r, int which) {
    return which || r->kind == POOL_IMPORT ? r->import_hi - r->import_lo : r->size;
}

void vk_mem_fault_report(uint64_t addr, uint64_t precision) {
    if (!precision) precision = 1;
    uint64_t lo = addr & ~(precision - 1), hi = lo + precision;
    int locked = plat_mutex_trylock_shared(&pool_lock);   // a thread that lost the device inside the lock reports unlocked
    int hits = 0;
    uint64_t below = 0, above = ~0ull;
    for (int i = 0; i < pool_index.n; i++) {
        const PoolRec *r = pool_index.e[i].buf;
        for (int w = 0; w < 2; w++) {
            if (!r->da_of[w] || r->da_of[w] != (w ? r->mbuf : r->buf)) continue;
            uint64_t a = r->da[w], b = a + rec_span(r, w);
            if (a < hi && b > lo) {
                hits++;
                rt_log("vk: fault address %#llx lies in a live pool buffer: pool %#llx+%#llx kind %d, its %s at %#llx+%#llx, "
                       "mirror state %d", (unsigned long long)addr, (unsigned long long)r->lo, (unsigned long long)r->size,
                       (int)r->kind, w ? "mirror" : "buffer", (unsigned long long)a, (unsigned long long)(b - a), (int)r->mstate);
            } else if (b <= lo && b > below) below = b;
            else if (a >= hi && a < above) above = a;
        }
    }
    uint64_t now = plat_now_ns();
    for (uint32_t k = 0; k < GONE_N && k < ngone; k++) {
        uint32_t i = (ngone - 1 - k) % GONE_N;
        if (gone[i].a < hi && gone[i].a + gone[i].n > lo) {
            hits++;
            rt_log("vk: fault address %#llx lies in a pool buffer destroyed %.1f ms before: pool at %#llx kind %d, its %s at "
                   "%#llx+%#llx", (unsigned long long)addr, (now - gone[i].at_ns) / 1e6, (unsigned long long)gone[i].lo,
                   (int)gone[i].kind, gone[i].which ? "mirror" : "buffer", (unsigned long long)gone[i].a,
                   (unsigned long long)gone[i].n);
        }
    }
    if (!hits)
        rt_log("vk: fault address %#llx is in no pool buffer, live or among the last %u destroyed (the nearest live ones end "
               "at %#llx and start at %#llx): an image or another buffer", (unsigned long long)addr,
               ngone < GONE_N ? ngone : GONE_N, (unsigned long long)below, (unsigned long long)above);
    if (locked) plat_mutex_unlock_shared(&pool_lock);
}

static void destroy_rec(VkCtx *c, PoolRec *r) {
    for (int w = 0; w < 2; w++)
        if (r->da_of[w] && r->da_of[w] == (w ? r->mbuf : r->buf)) {
            uint32_t i = ngone++ % GONE_N;
            gone[i].a = r->da[w], gone[i].n = rec_span(r, w), gone[i].lo = r->lo, gone[i].at_ns = plat_now_ns();
            gone[i].which = (uint8_t)w, gone[i].kind = (uint8_t)r->kind;
        }
    if (r->mbuf) vmaDestroyBuffer(c->vma, r->mbuf, r->malloc_);
    if (r->salloc) vmaDestroyBuffer(c->vma, r->buf, r->salloc), r->buf = VK_NULL_HANDLE;
    if (r->buf) vkDestroyBuffer(c->device, r->buf, NULL);
    if (r->mem) vkFreeMemory(c->device, r->mem, NULL);
    if (r->alloc) vmaFreeMemory(c->vma, r->alloc);
    if (r->alloc_tw) vmaFreeMemory(c->vma, r->alloc_tw);
    free(r->pt);
    free(r);
}

// The twin of a physical pool's memory (vk_texture.c bind_pool_memory): a second block of the same size and memory type,
// made at the first call. An image of the pool at offset o binds into it at o - VK_MEM_TWIN_SHIFT. OoT places
// its render targets at multiples of 512 bytes and the driver binds an image at multiples of 1024 only, so six in ten
// could not alias the pool and took memory of their own: 1.1 to 2.5 GiB at a time in the village, changing with the
// camera, which with frame generation crossed the VRAM budget every few seconds (frames of 26-56 ms while it did).
// Images in the twin share bytes with each other as images in the pool do, not with the pool's buffer; an image with
// memory of its own shared with nothing either. NULL when it cannot be made.
VmaAllocation vk_mem_phys_twin(VmaAllocation main) {
    VkCtx *c = vk_ctx();
    VmaAllocation tw = NULL;
    plat_mutex_lock(&pool_lock);
    for (int i = 0; i < nphys; i++) {
        PoolRec *r = phys_list[i];
        if (r->alloc != main) continue;
        if (!r->alloc_tw) {
            VmaAllocationInfo info;
            vmaGetAllocationInfo(c->vma, main, &info);
            VkMemoryRequirements req = {.size = info.size, .alignment = 65536, .memoryTypeBits = 1u << info.memoryType};
            VmaAllocationCreateInfo ai = {.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT,
                                          .memoryTypeBits = 1u << info.memoryType};
            if (vmaAllocateMemory(c->vma, &req, &ai, &r->alloc_tw, NULL) != VK_SUCCESS) r->alloc_tw = NULL;
            rt_log("vk: physical pool %#llx: a twin block of %llu MiB for images at odd offsets%s", (unsigned long long)r->lo,
                   (unsigned long long)(info.size >> 20), r->alloc_tw ? "" : " could not be allocated");
        }
        tw = r->alloc_tw;
        break;
    }
    plat_mutex_unlock(&pool_lock);
    return tw;
}

int vk_pool_init(pool_t *p) {
    VkCtx *c = vk_ctx();
    if (!c) { rt_log("vk: MemoryPoolInitialize before vk_init"); return 0; }
    // The GPU never sees a GPU_NO_ACCESS pool, except a physical one (0x189; 1.4.3 also 0x18c, CPU-visible) through the
    // virtual pools that map it. Either gets device-local memory: 1.4.3's CPU never writes its 0x18c pool's storage
    // (unchanged over 3600 frames of the intro), so the guest storage and the GPU copy need no sync.
    if ((p->flags & NVN_MEMORY_POOL_FLAGS_GPU_NO_ACCESS) && !(p->flags & NVN_MEMORY_POOL_FLAGS_PHYSICAL)) return 1;
    // A pool of zero bytes (TotK makes some now and then) holds no address the GPU can use: it gets no record. A record
    // would import 0 bytes (invalid usage; the driver refuses the memory) and put an empty range into the index at the
    // pool's address, where a lookup of the pool that starts there found the empty one first. vk_pool_fini finds no
    // record for it and does nothing.
    if (!p->size) return 1;
    PoolRec *r = calloc(1, sizeof *r);
    if (!r) rt_fatal("vk: out of memory for a pool record");
    r->key = p;
    r->flags = p->flags;
    r->lo = (uintptr_t)p->mem;
    r->size = p->size;
    int ok;
    if (p->flags & NVN_MEMORY_POOL_FLAGS_VIRTUAL) {
        r->kind = POOL_VIRT;
        r->npages = p->size / NVN_VPOOL_PAGE;
        r->pt = calloc(r->npages ? r->npages : 1, sizeof *r->pt);
        ok = r->pt != NULL;
    } else if (p->flags & NVN_MEMORY_POOL_FLAGS_PHYSICAL) {
        r->kind = POOL_PHYS;
        ok = alloc_physical(c, r);
    } else {
        r->kind = POOL_IMPORT;
        mem_thaw((uintptr_t)p->mem, p->size);   // the import may need write access to the pages
        ok = import_pool(c, r, (uintptr_t)p->mem, p->size);
        if (!ok) {
            // Not fatal for the game: the pool works for the CPU, and vk_mem_resolve reports its addresses as unbacked.
            long n = plat_inc32(&n_import_failures);
            if (n <= 8) rt_log("vk: pool %p (flags %#x, %#llx bytes) has no GPU buffer (%ld so far)", (void *)p->mem,
                               p->flags, (unsigned long long)p->size, n);
            r->buf = VK_NULL_HANDLE;
            ok = 1;
        }
    }
    if (!ok) { free(r->pt); free(r); return 0; }
    plat_mutex_lock(&pool_lock);
    index_put(r);
    if (r->kind == POOL_PHYS) {
        if (nphys == cap_phys) {
            cap_phys = cap_phys ? cap_phys * 2 : 16;
            phys_list = realloc(phys_list, (size_t)cap_phys * sizeof *phys_list);
            if (!phys_list) rt_fatal("vk: out of memory for the physical pool list");
        }
        phys_list[nphys++] = r;
    }
    plat_mutex_unlock(&pool_lock);
    return 1;
}

void vk_pool_fini(pool_t *p) {
    VkCtx *c = vk_ctx();
    if (!c) return;
    pidx_ent e;
    plat_mutex_lock(&pool_lock);
    int had = pidx_remove(&pool_index, p, &e);
    __atomic_add_fetch(&pool_gen, 1, __ATOMIC_RELEASE);
    PoolRec *r = had ? e.buf : NULL;
    if (r && r->mstate == MS_ON) mirror_forget(r);
    if (r && r->kind == POOL_PHYS) {
        for (int i = 0; i < nphys; i++)
            if (phys_list[i] == r) { phys_list[i] = phys_list[--nphys]; break; }
        for (int i = 0; i < pool_index.n; i++) {   // virtual pages that still map it become unmapped
            PoolRec *v = pool_index.e[i].buf;
            if (v->kind != POOL_VIRT) continue;
            for (uint64_t k = 0; k < v->npages; k++)
                if (v->pt[k].phys == r) v->pt[k].phys = NULL;
        }
    }
    plat_mutex_unlock(&pool_lock);
    if (!r) return;
    if (r->smap) shadow_forget(r);
    if (r->kind == POOL_IMPORT) mem_thaw(r->lo, r->size);   // the storage goes back to the guest writable
    // Images cut from this range go first; a used buffer or memory waits until the GPU is done with it, since the
    // guest gets the storage back when this returns.
    vk_texture_range_gone(r->lo, r->size, r->alloc);
    if (r->alloc_tw) vk_texture_range_gone(0, 0, r->alloc_tw);
    if (r->used && r->kind != POOL_VIRT) {
        vk_upload_flush(1);
        vk_queue_lock();
        vkQueueWaitIdle(c->gfx_queue);
        vk_queue_unlock();
    }
    vk_mem_collect();
    // A mirrored pool's record and buffers outlive the submission being recorded, whose sync copies and write-backs may
    // name them (the game frees a pool only once the GPU is done with it, but its CPU writes may still be queued).
    if (r->mbuf) mirror_retiring(r), vk_completion_add(vk_sync_issued() + 1, mirror_retire, r);
    else destroy_rec(c, r);
}

// Requests were checked by the front end (nvn_api_core.c, MNG totk_vpool.c); apply them in order.
int vk_pool_map_virtual(pool_t *v, int n, const void *requests) {
    const nvn_mapping_t *rq = requests;
    plat_mutex_lock(&pool_lock);
    const pidx_ent *e = pidx_find(&pool_index, (uintptr_t)v->mem, 0);
    PoolRec *r = e && e->key == v ? e->buf : NULL;
    int ok = r && r->kind == POOL_VIRT;
    __atomic_add_fetch(&pool_gen, 1, __ATOMIC_RELEASE);
    for (int i = 0; ok && i < n; i++) {
        PoolRec *ph = rq[i].physical_pool ? phys_find((const pool_t *)(const void *)rq[i].physical_pool) : NULL;
        if (rq[i].physical_pool && !ph) {
            rt_log("vk: MapVirtual request %d names physical pool %p that has no Vulkan memory", i,
                   (const void *)rq[i].physical_pool);
            ok = 0;
            break;
        }
        uint64_t first = (uint64_t)rq[i].virtual_offset / NVN_VPOOL_PAGE, count = rq[i].size / NVN_VPOOL_PAGE;
        if (first + count > r->npages) { ok = 0; break; }
        for (uint64_t k = 0; k < count; k++)
            r->pt[first + k] = (VPage){ph, ph ? (uint64_t)rq[i].physical_offset + k * NVN_VPOOL_PAGE : 0};
    }
    plat_mutex_unlock(&pool_lock);
    if (!ok) rt_log("vk: MapVirtual on pool %p failed", (void *)v->mem);
    return ok;
}

// Per-thread copies of resolved import and physical pools that overlap no other index entry (so pidx_find picks them for
// any range inside), valid while pool_gen is unchanged: the replay resolves a few dozen pools thousands of times per
// frame (16 vertex streams and every buffer binding of each draw). Direct-mapped by the 4 KiB page of the address, so
// a hit is one compare however many pools a frame touches. TOTK_RESOLVE_CHECK=1 compares every hit with the locked
// lookup and stops on a difference.
static int rec_class(const PoolRec *r) {
    if (r->kind != POOL_IMPORT) return VK_MCLS_DEVICE;
    return r->flags & NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS ? VK_MCLS_HOST_NOACC
         : r->flags & NVN_MEMORY_POOL_FLAGS_CPU_CACHED ? VK_MCLS_HOST_CACHED : VK_MCLS_HOST_UNC;
}

typedef struct { uint64_t gen, lo, hi, base, end; VkBuffer buf; VmaAllocation alloc; int cls; void *pool; } ResolveHit;
#define RHIT_SLOTS 512
static _Thread_local ResolveHit rhit[RHIT_SLOTS];

static ResolveHit *rhit_slot(uint64_t addr) { return &rhit[(addr >> 12 ^ addr >> 21) & (RHIT_SLOTS - 1)]; }
// The pools this thread resolved last under the lock. The slots above are per page, and a frame's vertex, index and
// uniform ranges lie on thousands of pages of a few large pools (one pool of 88 MiB in forest village): a page seen
// for the first time, or pushed out of its slot, went to the index under the pool lock, ~0.25 ms a frame on the replay
// thread. A range inside one of these pools fills its page's slot from here instead.
#define RMRU_N 8
static _Thread_local ResolveHit rmru[RMRU_N];
static _Thread_local unsigned rmru_next;

static int resolve_check_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = cfg_get("TOTK_RESOLVE_CHECK");
        on = e && *e && strcmp(e, "0");
    }
    return on;
}

static int resolve_locked(uint64_t addr, uint64_t size, VkMemSpan *out);

int vk_mem_resolve(uint64_t addr, uint64_t size, VkMemSpan *out) {
    memset(out, 0, sizeof *out);
    uint64_t g = __atomic_load_n(&pool_gen, __ATOMIC_ACQUIRE), end = addr + size;
    ResolveHit *h = rhit_slot(addr);
    if (end >= addr && !(h->gen == g && addr >= h->lo && end <= h->hi))
        for (int i = 0; i < RMRU_N; i++)
            if (rmru[i].gen == g && addr >= rmru[i].lo && end <= rmru[i].hi) {
                *h = rmru[i];
                break;
            }
    if (end >= addr && h->gen == g && addr >= h->lo && end <= h->hi) {
        out->buffer = h->buf;
        out->offset = addr - h->base;
        out->avail = h->end - addr;
        out->alloc = h->alloc;
        out->cls = h->cls;
        out->pool = h->pool;
        if (h->alloc) out->alloc_offset = out->offset;
        if (shadow_on && h->pool && !((PoolRec *)h->pool)->stouched) shadow_touch(h->pool);
        if (resolve_check_on()) {
            VkMemSpan s;
            if (!resolve_locked(addr, size, &s) || memcmp(&s, out, sizeof s))
                rt_fatal("vk: resolve cache of 0x%llx differs from the pool index", (unsigned long long)addr);
        }
        return 1;
    }
    return resolve_locked(addr, size, out);
}

// The pool index lookup under the lock; a non-overlapped import or physical pool goes into the address's cache slot.
static int resolve_locked(uint64_t addr, uint64_t size, VkMemSpan *out) {
    memset(out, 0, sizeof *out);
    plat_mutex_lock_shared(&pool_lock);
    const pidx_ent *e = pidx_find(&pool_index, (uintptr_t)addr, size);
    PoolRec *r = e ? e->buf : NULL;
    int ok = 0;
    if (r && (r->kind == POOL_IMPORT ? r->buf != VK_NULL_HANDLE : r->kind == POOL_PHYS)) {
        int i = (int)(e - pool_index.e);
        if ((i == 0 || pool_index.maxhi[i - 1] <= e->lo) && (i + 1 == pool_index.n || pool_index.e[i + 1].lo >= e->hi)) {
            ResolveHit *h = rhit_slot(addr);
            h->gen = __atomic_load_n(&pool_gen, __ATOMIC_RELAXED);   // stable: changed only under the exclusive lock
            h->lo = e->lo, h->hi = e->hi, h->buf = r->buf, h->cls = rec_class(r);
            if (r->kind == POOL_IMPORT) h->base = r->import_lo, h->end = r->import_hi, h->alloc = NULL, h->pool = r;
            else h->base = r->lo, h->end = r->lo + r->size, h->alloc = r->alloc, h->pool = NULL;
            rmru[rmru_next++ % RMRU_N] = *h;
        }
    }
    if (r && r->kind == POOL_IMPORT && r->buf) {
        out->cls = rec_class(r);
        out->pool = r;
        out->buffer = r->buf;
        out->offset = addr - r->import_lo;
        out->avail = r->import_hi - addr;
        if (r->smap && !r->stouched) shadow_touch(r);
        ok = 1;
    } else if (r && r->kind == POOL_PHYS) {
        out->buffer = r->buf;
        out->offset = out->alloc_offset = addr - r->lo;
        out->avail = r->size - out->offset;
        out->alloc = r->alloc;
        out->cls = VK_MCLS_DEVICE;
        ok = 1;
    } else if (r && r->kind == POOL_VIRT) {
        uint64_t rel = addr - r->lo, k = rel / NVN_VPOOL_PAGE;
        if (k < r->npages && r->pt[k].phys) {
            PoolRec *ph = r->pt[k].phys;
            uint64_t off = r->pt[k].off + rel % NVN_VPOOL_PAGE, end = r->pt[k].off + NVN_VPOOL_PAGE;
            for (uint64_t j = k + 1; j < r->npages && r->pt[j].phys == ph && r->pt[j].off == end; j++) end += NVN_VPOOL_PAGE;
            out->buffer = ph->buf;
            out->offset = out->alloc_offset = off;
            out->avail = end - off;
            out->alloc = ph->alloc;
            out->cls = VK_MCLS_DEVICE;
            ok = out->avail >= size;
            if (ok && !ph->used) plat_xchg32(&ph->used, 1);   // read first: the flag is only ever set
        }
    }
    if (ok && !r->used) plat_xchg32(&r->used, 1);
    plat_mutex_unlock_shared(&pool_lock);
    if (!ok) memset(out, 0, sizeof *out);
    return ok;
}

uint64_t vk_mem_gen(void) {
    return __atomic_load_n(&pool_gen, __ATOMIC_ACQUIRE) + __atomic_load_n(&mirror_gen, __ATOMIC_ACQUIRE);
}

int vk_mem_class_of(uint64_t addr) {
    VkMemSpan m;
    return vk_mem_resolve(addr, 1, &m) ? m.cls : VK_MCLS_NONE;
}

// ---------------------------------------------------------------- census
static volatile int64_t traffic[VK_MCLS_N][VK_MUSE_N];

// Every caller is the replay (vk_exec*, under vk_sync_submit's lock), about ten adds per draw: one writer at a time, so
// a plain add, not a locked one. An add racing the census's reset may be lost.
void vk_mem_count(int cls, int use, uint64_t bytes) {
    if (vk_par_worker) return;   // a chunk of a draw stretch: ten writes a draw to lines every chunk's thread writes
    if ((unsigned)cls < VK_MCLS_N && (unsigned)use < VK_MUSE_N)
        __atomic_store_n(&traffic[cls][use], __atomic_load_n(&traffic[cls][use], __ATOMIC_RELAXED) + (int64_t)bytes,
                         __ATOMIC_RELAXED);
}

static void flag_names(uint32_t f, char *buf, size_t n) {
    static const char *const names[] = {"CPU_NO_ACCESS", "CPU_UNCACHED", "CPU_CACHED", "GPU_NO_ACCESS", "GPU_UNCACHED",
                                        "GPU_CACHED", "SHADER_CODE", "COMPRESSIBLE", "PHYSICAL", "VIRTUAL"};
    size_t k = 0;
    buf[0] = 0;
    for (int i = 0; i < 10; i++)
        if (f >> i & 1) k += (size_t)snprintf(buf + k, k < n ? n - k : 0, "%s%s", k ? "|" : "", names[i]);
    if (!k) snprintf(buf, n, "none");
}

// One line per (kind, flags) group of the live pools, then the bytes the replay bound or copied per frame by pool
// class since the previous census (the first one: since frame 1200). Logged at frame 1800 (in the world from the
// run-game saves), at frame 600, and with TOTK_TRACE=perf every 1800 frames.
static void dirty_frame(uint64_t frame);

static void mirror_log(uint64_t frame);
static void snap_log(uint64_t frame, double frames);   // vk_mem_mirror_prologue section
static void shadow_log(uint64_t frame);
static int dead_pending(void);

static void vram_log(uint64_t frame);

static volatile uint64_t mirror_frame;   // the frame vk_mem_census last saw, for mirror_sync's log lines

static void hot_census(void);          // hot pools (end of file)
static void hot_demote_pending(void);

void vk_mem_census(uint64_t frame) {
    mirror_frame = frame;
    if (vk_mirror_on) hot_census();
    static uint64_t window_start;
    if ((rt_trace & RT_TRACE_PERF) && frame % 600 == 0) vram_log(frame);
    if (frame % 600 == 0) mem_result_log(frame);   // TOTK_RESULT_WATCH
    if ((rt_trace & RT_TRACE_PERF) && !vk_mirror_on) dirty_frame(frame);
    if ((rt_trace & RT_TRACE_PERF) ? frame % 60 == 0 : frame == 600 || frame == 1800)
        mirror_log(frame), shadow_log(frame);   // mirror_log also logs the write-watch line
    if (frame == 1200) {   // loading is over: the first window starts here
        for (int c = 0; c < VK_MCLS_N; c++)
            for (int u = 0; u < VK_MUSE_N; u++) plat_xchg64(&traffic[c][u], 0);
        window_start = frame;
        return;
    }
    // Frame 600 as well: runs on slow machines end before 1800.
    if (frame != 600 && (frame % 1800 || !(frame == 1800 || (rt_trace & RT_TRACE_PERF)))) return;
    typedef struct { int kind; uint32_t flags; uint64_t n, bytes; } Group;
    Group g[64];
    int ng = 0;
    uint64_t total[4] = {0}, count[4] = {0};
    plat_mutex_lock_shared(&pool_lock);
    for (int i = 0; i < pool_index.n; i++) {
        const PoolRec *r = pool_index.e[i].buf;
        int j = 0;
        while (j < ng && (g[j].kind != r->kind || g[j].flags != r->flags)) j++;
        if (j == ng && ng < 64) g[ng++] = (Group){r->kind, r->flags, 0, 0};
        if (j < ng) g[j].n++, g[j].bytes += r->size;
        total[r->kind] += r->size, count[r->kind]++;
    }
    plat_mutex_unlock_shared(&pool_lock);
    rt_log("mem: census at frame %llu: import %llu pools %.1f MiB (%d without a GPU buffer), physical %llu pools %.1f MiB, "
           "virtual %llu pools %.1f MiB of address space",
           (unsigned long long)frame, (unsigned long long)count[POOL_IMPORT], total[POOL_IMPORT] / 1048576.0,
           n_import_failures, (unsigned long long)count[POOL_PHYS], total[POOL_PHYS] / 1048576.0,
           (unsigned long long)count[POOL_VIRT], total[POOL_VIRT] / 1048576.0);
    static const char *const kinds[] = {"", "import", "physical", "virtual"};
    for (int j = 0; j < ng; j++) {
        char f[160];
        flag_names(g[j].flags, f, sizeof f);
        rt_log("mem:   %s %#x %s: %llu pools, %.1f MiB", kinds[g[j].kind], g[j].flags, f, (unsigned long long)g[j].n,
               g[j].bytes / 1048576.0);
    }
    uint64_t frames = window_start && frame > window_start ? frame - window_start : frame;
    static const char *const cls[] = {"", "host CPU_NO_ACCESS", "host CPU_UNCACHED", "host CPU_CACHED", "device", "scratch"};
    rt_log("mem: traffic per frame over %llu frames, MiB (bound ranges, not bytes read): class: vertex index uniform "
           "storage indirect copy-src gpu-write cpu-read", (unsigned long long)frames);
    for (int c = 1; c < VK_MCLS_N; c++) {
        double v[VK_MUSE_N];
        for (int u = 0; u < VK_MUSE_N; u++) v[u] = plat_xchg64(&traffic[c][u], 0) / 1048576.0 / (double)(frames ? frames : 1);
        rt_log("mem:   %s: %.2f %.2f %.2f %.2f %.3f %.2f %.2f %.3f", cls[c], v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
    }
    window_start = frame;
}

// 1 while the device-local memory the game holds is over, or right at, the budget the system gives this process, which
// is what is left of the card once other programs took theirs (VK_EXT_memory_budget). Over it, Windows moves the game's images to
// system memory and the frame rate falls to a fraction: with After Effects holding 7 GiB of a 12 GiB card the village
// ran at 13 fps against 104 (6.5 GiB in use, 5.4 GiB of budget; 2026-11-11). The menu layer shows a notice then
// (ui/menu.cpp vram_notice_draw). Asked once a second, from the present path only; three answers in a row decide.
int vk_mem_vram_short(double *used_gb, double *budget_gb) {
    static uint64_t next_ns;
    static int over, state;
    static double u, b;
    uint64_t now = plat_now_ns();
    if (now >= next_ns) {
        uint64_t used, budget;
        int exact;
        next_ns = now + 1000000000ull;
        // over the budget, or at it with under a twentieth to spare while the budget is under 7 GiB (a card another
        // program fills: the budget then follows what the game holds, and every new image pushes one out)
        int o = vk_mem_vram(&used, &budget, &exact) && exact &&
                (used > budget + (192ull << 20) || (budget < (7ull << 30) && used + budget / 20 > budget));
        over = o ? over + 1 : 0;
        if (over == 3 && !state)
            rt_log("mem: video memory is short: %.0f MiB in use, %.0f MiB of budget (another program holds the rest of the card)",
                   used / 1048576.0, budget / 1048576.0);
        state = over >= 3;
        u = used / 1073741824.0, b = budget / 1073741824.0;
    }
    *used_gb = u, *budget_gb = b;
    return state;
}

// Device-local memory the allocator holds in its blocks without an allocation in it: room for new textures that
// costs no video memory, and what stays taken after textures are freed until a whole block is empty.
uint64_t vk_mem_vma_free(void) {
    VkCtx *c = vk_ctx();
    if (!c || !c->vma) return 0;
    VmaBudget vb[VK_MAX_MEMORY_HEAPS];
    vmaGetHeapBudgets(c->vma, vb);
    uint64_t n = 0;
    for (uint32_t h = 0; h < c->mem_props.memoryHeapCount && h < VK_MAX_MEMORY_HEAPS; h++)
        if (c->mem_props.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            n += vb[h].statistics.blockBytes - vb[h].statistics.allocationBytes;
    return n;
}

// DEBUG-VRAMCAP: TOTK_VRAM_BUDGET_MB=<n>: the budget every caller sees is at most n MiB, to watch on a large card
// what the eviction and the texture quality do on a small one (the memory itself is still there, so the frame rate
// of such a run says nothing about the small card's). 0: no cap.
uint64_t vk_mem_vram_cap(void) {
    static int64_t cap = -1;
    if (cap < 0) {
        const char *e = cfg_get("TOTK_VRAM_BUDGET_MB");
        cap = e && *e && atoll(e) > 0 ? atoll(e) << 20 : 0;
    }
    return (uint64_t)cap;
}

int vk_mem_vram(uint64_t *used, uint64_t *budget, int *exact) {
    VkCtx *c = vk_ctx();
    *used = *budget = 0, *exact = 0;
    if (!c) return 0;
    uint64_t cap = vk_mem_vram_cap();
    const VkPhysicalDeviceMemoryProperties *mp = &c->mem_props;
    if (c->has_memory_budget) {
        VkPhysicalDeviceMemoryBudgetPropertiesEXT b = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
        VkPhysicalDeviceMemoryProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, .pNext = &b};
        vkGetPhysicalDeviceMemoryProperties2(c->phys, &p2);
        for (uint32_t h = 0; h < p2.memoryProperties.memoryHeapCount && h < VK_MAX_MEMORY_HEAPS; h++)
            if (p2.memoryProperties.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
                *used += b.heapUsage[h], *budget += b.heapBudget[h];
        *exact = 1;
        if (cap && *budget > cap) *budget = cap;
        return *budget != 0;
    }
    VmaBudget vb[VK_MAX_MEMORY_HEAPS];
    vmaGetHeapBudgets(c->vma, vb);
    for (uint32_t h = 0; h < mp->memoryHeapCount && h < VK_MAX_MEMORY_HEAPS; h++)
        if (mp->memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            *used += vb[h].statistics.blockBytes, *budget += mp->memoryHeaps[h].size;
    if (cap && *budget > cap) *budget = cap;
    return *budget != 0;
}

// ---------------------------------------------------------------- page write generations
// Two-level table over the guest address space (below 2^39): 256 MiB chunks of 65,536 page generations, allocated
// when a page of the chunk is first stamped. 0 = never stamped.
#define GEN_CHUNK_SHIFT 28
#define GEN_PAGE_SHIFT 12
#define GEN_CHUNKS (RT_ADDRESS_LIMIT >> GEN_CHUNK_SHIFT)
#define GEN_PER_CHUNK (1u << (GEN_CHUNK_SHIFT - GEN_PAGE_SHIFT))
static uint32_t *gen_chunk[GEN_CHUNKS];
// gen_lock: per page, the stamp of a GPU write a VRAM mirror already holds (vk_gpu_written_mirror); a page whose stamp
// equals it is not copied into the mirror again
static uint32_t *own_chunk[GEN_CHUNKS];
// The frame (+1) in which a write-watch query last found each page written by the CPU (snap_regions; GPU writes into
// imported memory do not set the write-watch bits), for the write-back merge's choice (cpu_wrote_lately).
static uint32_t *cpuw_chunk[GEN_CHUNKS];
static int lz_cold(uint64_t page_addr);   // write-back on demand: the page's bytes are in the mirror only
static int page_owned(uint64_t a, uint32_t stamp) {
    const uint32_t *o = own_chunk[a >> GEN_CHUNK_SHIFT];
    return o && o[(a >> GEN_PAGE_SHIFT) & (GEN_PER_CHUNK - 1)] == stamp;
}
static uint32_t gen_now;                 // last generation handed out
static PlatMutex gen_lock = PLAT_MUTEX_INIT;
static uint64_t gen_pages[1024];
// The newest generation stamped on any page of each 2 MiB region (the write-watch snapshot regions below): the mirror
// sync skips regions with nothing newer than its pool's mirror.
#define REG_SHIFT 21
static uint32_t reg_max[RT_ADDRESS_LIMIT >> REG_SHIFT];

// Every write-watch query of the generation table (gen_lock held exclusively): calls, bytes and time since the last
// mirror summary, all consumers together; the queries that found written pages (they also flush TLBs) apart.
static uint64_t ww_calls, ww_bytes, ww_ticks, ww_dirty_calls, ww_dirty_ticks, ww_pages;

static uint64_t ww_query(uint64_t addr, uint64_t size) {
    uint64_t t0 = qpc();
    uint64_t n = mem_written_pages(addr, size, gen_pages, sizeof gen_pages / sizeof *gen_pages);
    uint64_t dt = qpc() - t0;
    ww_ticks += dt, ww_calls++, ww_bytes += size;
    if (n && n != UINT64_MAX) ww_dirty_calls++, ww_dirty_ticks += dt, ww_pages += n;
    return n;
}

// Frozen regions: a 2 MiB region whose pages nothing wrote for freeze_idle submissions is made read-only
// (memory.c mem_freeze), and write-watch queries skip it until a write or a file read thaws it. Most of the pool memory
// the mirror and the texture checks cover is static (meshes, textures), and a query costs about 1 us per MiB (4-5 on an
// 8th-gen i7), so this keeps the queries to the regions the CPU writes. The region need not lie wholly in pools: NVN
// pools are small and not 2 MiB aligned, so most regions also hold other guest memory, and a rule that refused those
// left 340 of 436 queried MiB a frame unfrozen in town although nothing had written them for 300 submissions. The
// other memory is safe to freeze for the same reason pools are: the only kernel writes into guest memory are file
// reads, which thaw their buffer first (fs_host.c, romfs.c), and every user-mode write faults and thaws.
#define FREEZE_PER_SUB 32
static int freeze_idle;                       // 0: off
static uint32_t ww_ep = 1;                    // gen_lock: the submission being recorded (vk_mem_mirror_begin)
static uint32_t reg_last[RT_ADDRESS_LIMIT >> REG_SHIFT];    // gen_lock: ww_ep of the region's last stamp
static uint32_t reg_tried[RT_ADDRESS_LIMIT >> REG_SHIFT];   // gen_lock: ww_ep of its last freeze candidacy
static uint32_t *fcand;                       // gen_lock: regions to try at the next freeze pass
static int nfcand, cfcand;
static uint64_t n_frz_refused;                // gen_lock: candidates mem_freeze refused

static uint32_t *gen_slot(uint64_t page_addr, int create) {
    uint64_t ci = page_addr >> GEN_CHUNK_SHIFT;
    if (ci >= GEN_CHUNKS) return NULL;
    if (!gen_chunk[ci]) {
        if (!create) return NULL;
        gen_chunk[ci] = calloc(GEN_PER_CHUNK, sizeof(uint32_t));
        if (!gen_chunk[ci]) rt_fatal("vk: out of memory for page generations");
    }
    return &gen_chunk[ci][(page_addr >> GEN_PAGE_SHIFT) & (GEN_PER_CHUNK - 1)];
}

// gen_lock held exclusively; g is the newest generation handed out.
static void stamp_page(uint64_t a, uint32_t g) {
    uint32_t *s = gen_slot(a, 1);
    if (!s) return;
    *s = g;
    reg_max[a >> REG_SHIFT] = g;
    reg_last[a >> REG_SHIFT] = ww_ep;
}

static void stamp_range(uint64_t lo, uint64_t hi, uint32_t g) {
    for (uint64_t a = lo; a < hi; a += 1ull << GEN_PAGE_SHIFT) stamp_page(a, g);
}

static void guest_written_(uint64_t addr, uint64_t size, int owned) {
    if (!size || nvn_gpu_only(addr)) return;
    {   // DEBUG-OWNSKIP: TOTK_OWNED_SKIP=<lo>-<hi>: GPU writes into that address range are not mirror-owned (A/B)
        static uint64_t slo = 1, shi;
        if (slo == 1) {
            const char *e = cfg_get("TOTK_OWNED_SKIP");
            char *x;
            slo = e ? strtoull(e, &x, 0) : 0, shi = e && *x == '-' ? strtoull(x + 1, NULL, 0) : 0;
        }
        if (owned && addr >= slo && addr < shi) owned = 0;
    }
    uint64_t lo = addr & ~0xfffull, hi = (addr + size + 0xfff) & ~0xfffull;
    // owned: pages the CPU has written since the last query (not stamped yet) stay the CPU's
    uint64_t cpu[64], ncpu = owned ? mem_written_peek(lo, hi - lo, cpu, 64) : 0;
    if (ncpu > 64) owned = 0;   // more than the list holds, or not watched (UINT64_MAX): no page counts as owned
    plat_mutex_lock(&gen_lock);
    uint32_t g = ++gen_now;
    stamp_range(lo, hi, g);
    for (uint64_t a = lo; owned && a < hi; a += 1ull << GEN_PAGE_SHIFT) {
        int w = 0;
        for (uint64_t i = 0; i < ncpu && !w; i++) w = cpu[i] == a;
        if (w) continue;
        uint64_t ci = a >> GEN_CHUNK_SHIFT;
        if (ci >= GEN_CHUNKS) break;
        if (!own_chunk[ci] && !(own_chunk[ci] = calloc(GEN_PER_CHUNK, sizeof(uint32_t)))) break;
        own_chunk[ci][(a >> GEN_PAGE_SHIFT) & (GEN_PER_CHUNK - 1)] = g;
    }
    plat_mutex_unlock(&gen_lock);
}
void vk_guest_written(uint64_t addr, uint64_t size) { guest_written_(addr, size, 0); }

int vk_mem_changed(uint64_t addr, uint64_t size, uint32_t since, uint32_t *now) {
    if (!size) { *now = since; return 0; }
    uint64_t lo = addr & ~0xfffull, hi = (addr + size + 0xfff) & ~0xfffull;
    plat_mutex_lock(&gen_lock);
    uint32_t g = ++gen_now;
    int tracked = 1, frozen = 1;
    for (uint64_t a = lo & ~((1ull << REG_SHIFT) - 1); a < hi && frozen; a += 1ull << REG_SHIFT) frozen = mem_frozen(a);
    while (!frozen) {
        uint64_t n = ww_query(lo, hi - lo);
        if (n == UINT64_MAX) { tracked = 0; break; }
        for (uint64_t k = 0; k < n; k++) stamp_page(gen_pages[k], g);
        if (n < sizeof gen_pages / sizeof *gen_pages) break;   // a full answer may have left pages for the next call
    }
    int changed = 0;
    for (uint64_t a = lo; tracked && a < hi && !changed; a += 1ull << GEN_PAGE_SHIFT) {
        uint32_t *s = gen_slot(a, 0);
        if (s && *s > since) changed = 1;
    }
    plat_mutex_unlock(&gen_lock);
    *now = g;
    return tracked ? changed : -1;
}

// Write-watch snapshots per submission: the guest address space in 2 MiB regions, each queried at most once per epoch
// (a guest submission), at the first check that covers it. A query costs about 1.3 us plus 0.55 us per MiB, so one per
// region instead of one per texture. The snapshot only adds observations to the generation table: a page written
// before it carries a generation newer than any check before it, and a page written after it keeps its OS bit until
// the next query over it (at the latest the next epoch's snapshot).
#define SNAP_SHIFT 21
#define SNAP_REGIONS (RT_ADDRESS_LIMIT >> SNAP_SHIFT)
static uint32_t snap_at[SNAP_REGIONS];      // epoch of the region's last snapshot; 0 never
static uint8_t snap_untracked[SNAP_REGIONS];
static uint64_t n_snap_checks, n_snap_calls, n_snap_untracked;

// gen_lock held exclusively. Queries the regions r0..r1 that were not snapped in epoch, one query per run of them;
// returns the number of queries.
static void cpuw_mark(uint64_t page_addr) {
    uint64_t ci = page_addr >> GEN_CHUNK_SHIFT;
    if (ci >= GEN_CHUNKS) return;
    if (!cpuw_chunk[ci] && !(cpuw_chunk[ci] = calloc(GEN_PER_CHUNK, sizeof(uint32_t)))) return;
    cpuw_chunk[ci][(page_addr >> GEN_PAGE_SHIFT) & (GEN_PER_CHUNK - 1)] =
        (uint32_t)__atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) + 1;
}

static uint64_t snap_regions(uint64_t r0, uint64_t r1, uint32_t epoch) {
    uint32_t g = 0;
    uint64_t calls = 0;
    for (uint64_t r = r0; r <= r1;) {
        if (snap_at[r] == epoch) { r++; continue; }
        if (mem_frozen(r << SNAP_SHIFT)) {   // nothing in it changed since its freeze
            snap_at[r] = epoch, snap_untracked[r] = 0;
            r++;
            continue;
        }
        uint64_t k = r;   // one query over a run of regions not seen in this epoch
        while (k + 1 <= r1 && snap_at[k + 1] != epoch && !mem_frozen((k + 1) << SNAP_SHIFT)) k++;
        uint64_t a = r << SNAP_SHIFT, b = (k + 1) << SNAP_SHIFT;
        int tracked = 1;
        if (!g) g = ++gen_now;
        for (;;) {
            uint64_t n = ww_query(a, b - a);
            if (n == UINT64_MAX) { tracked = 0; break; }
            for (uint64_t j = 0; j < n; j++) stamp_page(gen_pages[j], g), cpuw_mark(gen_pages[j]);
            if (n < sizeof gen_pages / sizeof *gen_pages) break;
        }
        calls++;
        for (uint64_t j = r; j <= k; j++) {
            snap_at[j] = epoch, snap_untracked[j] = (uint8_t)!tracked;
            if (tracked && freeze_idle && ww_ep - reg_last[j] >= (uint32_t)freeze_idle &&
                ww_ep - reg_tried[j] >= (uint32_t)freeze_idle) {
                reg_tried[j] = ww_ep;
                if (nfcand == cfcand) grow((void **)&fcand, &cfcand, sizeof *fcand);
                fcand[nfcand++] = (uint32_t)j;
            }
        }
        r = k + 1;
    }
    return calls;
}

int vk_mem_changed_snap(uint64_t addr, uint64_t size, uint32_t since, uint32_t epoch, uint32_t *now) {
    if (!size) { *now = since; return 0; }
    uint64_t lo = addr & ~0xfffull, hi = (addr + size + 0xfff) & ~0xfffull;
    uint64_t r0 = lo >> SNAP_SHIFT, r1 = (hi - 1) >> SNAP_SHIFT;
    if (!epoch || r1 >= SNAP_REGIONS) return vk_mem_changed(addr, size, since, now);
    int excl = 0;
    plat_mutex_lock_shared(&gen_lock);
    for (uint64_t r = r0; r <= r1; r++)
        if (snap_at[r] != epoch) { excl = 1; break; }
    if (excl) {
        plat_mutex_unlock_shared(&gen_lock);
        plat_mutex_lock(&gen_lock);
        n_snap_calls += snap_regions(r0, r1, epoch);
    }
    int untracked = 0, changed = 0;
    for (uint64_t r = r0; r <= r1; r++) untracked |= snap_untracked[r];
    // Pages only in regions stamped after since (reg_max; REG_SHIFT is SNAP_SHIFT): an unchanged texture of 4 MiB was
    // 1,024 page reads, at about 1,100 texture checks a frame.
    for (uint64_t r = r0; !untracked && r <= r1 && !changed; r++) {
        if (reg_max[r] <= since) continue;
        uint64_t a = r << REG_SHIFT > lo ? r << REG_SHIFT : lo, b = (r + 1) << REG_SHIFT < hi ? (r + 1) << REG_SHIFT : hi;
        for (; a < b && !changed; a += 1ull << GEN_PAGE_SHIFT) {
            uint32_t *s = gen_slot(a, 0);
            if (s && *s > since) changed = 1;
        }
    }
    *now = gen_now;   // every observation so far is in the table
    __atomic_add_fetch(&n_snap_checks, 1, __ATOMIC_RELAXED);
    if (untracked) __atomic_add_fetch(&n_snap_untracked, 1, __ATOMIC_RELAXED);
    if (excl) plat_mutex_unlock(&gen_lock);
    else plat_mutex_unlock_shared(&gen_lock);
    return untracked ? -1 : changed;
}

void vk_mem_snap_stats(uint64_t *checks, uint64_t *calls, uint64_t *untracked) {
    *checks = __atomic_exchange_n(&n_snap_checks, 0, __ATOMIC_RELAXED);
    *calls = __atomic_exchange_n(&n_snap_calls, 0, __ATOMIC_RELAXED);
    *untracked = __atomic_exchange_n(&n_snap_untracked, 0, __ATOMIC_RELAXED);
}

// ---------------------------------------------------------------- dirty census (TOTK_TRACE=perf)
// What a device-local mirror of the buffer-bound pools would upload. The replay notes every
// imported pool it binds as a vertex, index, uniform, storage or indirect buffer (vk_mem_bound_note). At the start of
// each submit one write-watch query per run of such pools stamps the pages written since with a new generation, in the
// shared generation table (no second consumer of the OS bits: texture checks see the stamps), and the pages of those
// pools are counted:
// - eager: pages stamped after the previous submit's scan, in every buffer-bound pool (upload at every submit);
// - lazy: pages stamped after the pool's previous count, in pools the previous submit bound (upload when bound);
// - first use: the size of a pool the first time it is bound (a mirror copies the whole pool then).
// Classes: CPU_CACHED, CPU_UNCACHED, other (CPU_NO_ACCESS). Logged as `mem: dirty` every 60 frames.
#define DIRTY_UNSYNCED UINT32_MAX
#define DIRTY_GAP 0x100000ull   // pools closer than this share one query (the gap's pages are stamped too)
#define DIRTY_WIN 60
enum { DC_CACHED, DC_UNC, DC_OTHER, DC_N };
typedef struct {
    uint64_t eager[DC_N], lazy[DC_N], first[DC_N];
    uint64_t submits, queries, scan_ns, sub_max, frames;
} DirtyAcc;
static uint32_t dirty_sub = 1, dirty_gen;   // submit lock: the current submit number, generation of the last scan
static uint64_t first_pending[DC_N], ever_n[DC_N], ever_bytes[DC_N];   // submit lock
static PoolRec **dlist;                     // submit lock: the buffer-bound pools of a scan
static int dcap;
static PlatMutex dirty_lock = PLAT_MUTEX_INIT;   // everything below
static DirtyAcc dfr, dwin;                  // this frame, this window
static uint32_t dwin_kib[DIRTY_WIN][DC_N];  // eager KiB of each frame of the window
static uint64_t live_n[DC_N], live_bytes[DC_N], live_untracked, ever_snap_n[DC_N], ever_snap_bytes[DC_N];

static int dc_class(const PoolRec *r) {
    int c = rec_class(r);
    return c == VK_MCLS_HOST_CACHED ? DC_CACHED : c == VK_MCLS_HOST_UNC ? DC_UNC : DC_OTHER;
}

void vk_mem_bound_note(void *pool) {
    if (vk_par_worker) return;   // TOTK_TRACE=perf statistics under a lock: the replay thread's binds only
    PoolRec *r = pool;
    if (r->bound_sub == dirty_sub) return;
    r->bound_sub = dirty_sub;
    if (r->bufbound) return;
    r->bufbound = 1, r->synced_gen = DIRTY_UNSYNCED;
    int k = dc_class(r);
    ever_n[k]++, ever_bytes[k] += r->size, first_pending[k] += r->size;
}

static uint64_t page_lo(const PoolRec *r) { return r->lo & ~0xfffull; }
static uint64_t page_hi(const PoolRec *r) { return (r->lo + r->size + 0xfff) & ~0xfffull; }

// gen_lock held exclusively. 0: the range is not write-watched.
static int query_stamp(uint64_t lo, uint64_t hi, uint32_t g) {
    for (;;) {
        uint64_t n = ww_query(lo, hi - lo);
        if (n == UINT64_MAX) return 0;
        for (uint64_t k = 0; k < n; k++) stamp_page(gen_pages[k], g);
        if (n < sizeof gen_pages / sizeof *gen_pages) return 1;
    }
}

// ---------------------------------------------------------------- shadow storage
// Where importing guest memory costs too much (Linux: re-protecting pages that amdgpu imported as a userptr
// invalidates the buffer, about 1.3 ms per 16 MiB at the next submit, and the import's write pinning clears the
// write watch), a CPU-visible pool gets a host-visible buffer of its own instead, at the offsets the import would have:
// - CPU -> shadow: right before each submit (vk_mem_shadow_sync), the pools the replay resolved since the last one are
//   queried for written pages; pages stamped after a pool's sgen are copied in by the CPU.
// - GPU -> guest: the replay notes each GPU write into CPU-visible memory at record time (vk_mem_gpu_pending) and
//   reports it at the fence (vk_gpu_written), which copies those bytes back into guest memory. Until then the CPU
//   copies skip them: a page the CPU and the GPU both write keeps the GPU's bytes.
// TOTK_SHADOW=1/0 forces the mode on or off; the default is on for Linux and Wine.
typedef struct { uint64_t lo, hi; } SRange;
static PlatMutex shadow_lock = PLAT_MUTEX_INIT;   // after pool_lock; before gen_lock
static PoolRec **stouch;                          // shadow_lock: pools resolved since the last sync
static int nstouch, cstouch;
static SRange *spend;                             // shadow_lock: GPU writes recorded and not copied back yet
static int nspend, cspend;
static int64_t s_live_n, s_live_bytes;            // shadow_lock
static uint64_t s_syncs, s_copied, s_queries, s_untracked, s_wb_bytes, s_ticks;   // shadow_lock: since the last log

static int shadow_pool(VkCtx *c, PoolRec *r, uint64_t addr, uint64_t size) {
    r->import_lo = addr & ~0xfffull;
    r->import_hi = (addr + size + 0xfff) & ~0xfffull;
    uint64_t n = r->import_hi - r->import_lo;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = n, .usage = pool_usage(c),
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    // Cached where the device has it: the fence copies read it back on the CPU.
    VmaAllocationCreateInfo ai = {.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                  .requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                  .preferredFlags = VK_MEMORY_PROPERTY_HOST_CACHED_BIT};
    VmaAllocationInfo info;
    VkResult res = vmaCreateBuffer(c->vma, &bi, &ai, &r->buf, &r->salloc, &info);
    if (res != VK_SUCCESS) {
        rt_log("vk: pool %#llx+%#llx: shadow allocation failed (%d)", (unsigned long long)addr, (unsigned long long)size,
               (int)res);
        r->buf = VK_NULL_HANDLE, r->salloc = NULL;
        return 0;
    }
    r->smap = info.pMappedData;
    plat_mutex_lock(&gen_lock);
    r->sgen = gen_now;   // the copy below holds every write stamped so far; later stamps are copied again
    plat_mutex_unlock(&gen_lock);
    memcpy(r->smap, (const void *)(uintptr_t)r->import_lo, n);
    plat_mutex_lock(&shadow_lock);
    s_live_n++, s_live_bytes += (int64_t)n;
    plat_mutex_unlock(&shadow_lock);
    plat_inc32(&n_imports);
    return 1;
}

static void shadow_touch(PoolRec *r) {
    plat_mutex_lock(&shadow_lock);
    if (!r->stouched) {
        if (nstouch == cstouch) grow((void **)&stouch, &cstouch, sizeof *stouch);
        stouch[nstouch++] = r;
        r->stouched = 1;
    }
    plat_mutex_unlock(&shadow_lock);
}

// vk_pool_fini, after the pool left the index: off the touched list before its record goes.
static void shadow_forget(PoolRec *r) {
    plat_mutex_lock(&shadow_lock);
    for (int i = 0; i < nstouch; i++)
        if (stouch[i] == r) { stouch[i] = stouch[--nstouch]; break; }
    s_live_n--, s_live_bytes -= (int64_t)(r->import_hi - r->import_lo);
    plat_mutex_unlock(&shadow_lock);
}

// shadow_lock held: copies guest [a, b) into the shadow, leaving out the bytes of GPU writes not copied back yet
// (spend from index i on).
static void shadow_copy(PoolRec *r, uint64_t a, uint64_t b, int i) {
    for (; i < nspend; i++)
        if (spend[i].lo < b && spend[i].hi > a) {
            if (spend[i].lo > a) shadow_copy(r, a, spend[i].lo, i + 1);
            if (spend[i].hi < b) shadow_copy(r, spend[i].hi, b, i + 1);
            return;
        }
    memcpy(r->smap + (a - r->import_lo), (const void *)(uintptr_t)a, b - a);
    s_copied += b - a;
}

// shadow_lock and gen_lock held: stamps the pool's written pages and copies those newer than its shadow.
static void shadow_sync_pool(PoolRec *r) {
    uint64_t lo = r->import_lo, hi = r->import_hi;
    uint32_t g = ++gen_now;
    int tracked = 1;
    for (uint64_t a = lo; a < hi;) {   // one query per run of regions that are not frozen
        uint64_t b = (a | ((1ull << REG_SHIFT) - 1)) + 1;
        if (b > hi) b = hi;
        if (mem_frozen(a)) { a = b; continue; }
        while (b < hi && !mem_frozen(b)) b = b + (1ull << REG_SHIFT) < hi ? b + (1ull << REG_SHIFT) : hi;
        s_queries++;
        if (!query_stamp(a, b, g)) tracked = 0;
        a = b;
    }
    uint32_t since = r->sgen;
    r->sgen = gen_now;
    if (!tracked) {   // nothing tells which pages changed: all of them
        s_untracked++;
        shadow_copy(r, lo, hi, 0);
        return;
    }
    uint64_t run = 0;
    for (uint64_t k = lo >> REG_SHIFT; k <= (hi - 1) >> REG_SHIFT; k++) {
        uint64_t a = k << REG_SHIFT, b = a + (1ull << REG_SHIFT);
        if (a < lo) a = lo;
        if (b > hi) b = hi;
        const uint32_t *ch = gen_chunk[a >> GEN_CHUNK_SHIFT];   // a 2 MiB region lies in one chunk
        if (reg_max[k] <= since || !ch) {
            if (run) shadow_copy(r, run, a, 0), run = 0;
            continue;
        }
        for (uint64_t x = a; x < b; x += 1ull << GEN_PAGE_SHIFT) {
            int dirty = ch[(x >> GEN_PAGE_SHIFT) & (GEN_PER_CHUNK - 1)] > since;
            if (dirty && !run) run = x;
            else if (!dirty && run) shadow_copy(r, run, x, 0), run = 0;
        }
    }
    if (run) shadow_copy(r, run, hi, 0);
}

void vk_mem_shadow_sync(void) {
    if (!shadow_on) return;
    uint64_t t0 = qpc();
    plat_mutex_lock(&shadow_lock);
    if (nstouch) {
        plat_mutex_lock(&gen_lock);
        for (int i = 0; i < nstouch; i++) shadow_sync_pool(stouch[i]), stouch[i]->stouched = 0;
        plat_mutex_unlock(&gen_lock);
        s_syncs += (uint64_t)nstouch;
        nstouch = 0;
    }
    s_ticks += qpc() - t0;
    plat_mutex_unlock(&shadow_lock);
}

// GPU writes to guest memory the replay recorded, with their submission (vk_mem_gpu_write_pending): a ring, newest last.
#define GWP_N 8192
static struct { uint64_t lo, hi, fence; } gwp[GWP_N];
static unsigned gwp_head;

// Replay thread: [addr, addr + size) overlaps a GPU write recorded in a submission the GPU has not finished.
int vk_mem_gpu_write_pending(uint64_t addr, uint64_t size) {
    uint64_t done = vk_sync_fence_done(), end = addr + size;
    for (unsigned k = 1; k <= GWP_N && k <= gwp_head; k++) {
        unsigned i = (gwp_head - k) % GWP_N;
        if (gwp[i].fence <= done) break;   // this one and every older one are done
        if (gwp[i].lo < end && addr < gwp[i].hi) return 1;
    }
    return 0;
}

void vk_mem_gpu_pending(uint64_t addr, uint64_t size) {
    if (mem_result_watch_on()) mem_result_watch(addr, size, vk_exec_recording_fence());   // TOTK_RESULT_WATCH
    if (size) {
        unsigned i = gwp_head++ % GWP_N;
        gwp[i].lo = addr, gwp[i].hi = addr + size, gwp[i].fence = vk_exec_recording_fence();
    }
    if (!shadow_on || !size) return;
    plat_mutex_lock(&shadow_lock);
    if (nspend == cspend) grow((void **)&spend, &cspend, sizeof *spend);
    spend[nspend++] = (SRange){addr, addr + size};
    plat_mutex_unlock(&shadow_lock);
}

static __thread int gpu_written_owned;   // vk_gpu_written_mirror, for the call below
void vk_gpu_written_mirror(uint64_t addr, uint64_t size) {
    void vk_gpu_written(uint64_t, uint64_t);
    gpu_written_owned = 1;
    vk_gpu_written(addr, size);
    gpu_written_owned = 0;
}
void vk_gpu_written(uint64_t addr, uint64_t size) {
    mem_result_release(addr, size);   // TOTK_RESULT_WATCH
    if (shadow_on && size) {
        plat_mutex_lock_shared(&pool_lock);
        const pidx_ent *e = pidx_find(&pool_index, addr, size);
        PoolRec *r = e ? e->buf : NULL;
        plat_mutex_lock(&shadow_lock);
        if (r && r->smap && addr >= r->import_lo && addr + size <= r->import_hi) {
            mem_thaw(addr, size);
            memcpy((void *)(uintptr_t)addr, r->smap + (addr - r->import_lo), size);
            s_wb_bytes += size;
        }
        for (int i = 0; i < nspend; i++)
            if (spend[i].lo == addr && spend[i].hi == addr + size) { spend[i] = spend[--nspend]; break; }
        plat_mutex_unlock(&shadow_lock);
        plat_mutex_unlock_shared(&pool_lock);
    }
    vk_usnap_gpuw(addr, size);   // DEBUG-USNAP-GPUW
    if (gpu_written_owned) {
        guest_written_(addr, size, 1);
        return;
    }
    {   // DEBUG-LISTLOG: the completion of a GPU write into CPU-visible memory of the list pool
        uint64_t vk_listlog_addr(void);
        double vk_listlog_ms(void);
        uint64_t la = vk_listlog_addr();
        if (la && addr < la + 0x980000 && addr + size > la)
            rt_log("LISTLOG gpuwritten %#llx+%#llx at %.3f", (unsigned long long)addr, (unsigned long long)size, vk_listlog_ms());
    }
    vk_guest_written(addr, size);
}

static void shadow_log(uint64_t frame) {
    if (!shadow_on) return;
    plat_mutex_lock(&shadow_lock);
    rt_log("mem: shadow at frame %llu: %lld pools %.1f MiB; since the last: %llu pool syncs, %llu queries, %.1f MiB "
           "copied in (%llu untracked), %.1f MiB written back, %.2f ms, %d GPU writes pending",
           (unsigned long long)frame, (long long)s_live_n, s_live_bytes / 1048576.0, (unsigned long long)s_syncs,
           (unsigned long long)s_queries, s_copied / 1048576.0, (unsigned long long)s_untracked, s_wb_bytes / 1048576.0,
           qpc_ms(s_ticks), nspend);
    s_syncs = s_queries = s_copied = s_untracked = s_wb_bytes = s_ticks = 0;
    plat_mutex_unlock(&shadow_lock);
}

#ifdef GAME_NS2
// ---------------------------------------------------------------- pool table for global memory (NVN2)
// NVN2 shaders load and store at addresses they read from memory, which the translator ties to no storage buffer
// (docs/oot-xlate.md "gmem_map"; seedds/nxnative dm/gfx/nvn/nvn_mtl.m gmem_map @ ee9b776, MIT): such an access looks the
// address up in a table of the pools and goes through a PhysicalStorageBuffer pointer at address + delta. Here each
// entry is a pool's guest range and delta the device address of the Vulkan buffer that holds those bytes minus the
// range's guest start: a physical pool's device memory, for a virtual pool each run of pages mapped contiguously from
// one physical pool, and for an imported pool the buffer its buffer bindings use: its VRAM mirror when it has one (the
// replay syncs the mirrors before a submission's first such access, vk_mem_gmem_sync), else its import buffer, the guest
// pages themselves. Through host memory every access crosses PCIe: OoT's movie network, about 40 MiB of feature maps
// read and written a frame, took 86 ms of GPU time a frame that way and 9 ms in the mirror. What such a shader writes
// into a mirror goes back to guest memory through the write-back merge once a write log has shown the piece written
// (vk_mem_gmem_writeback, below gpu_wrote); mirror entries are split into pieces as guest ones (vk_exec_draw.c
// gmem_snapshot, mirror 1). Writes into an import buffer are guest memory (cpu 1: vk_gpu_written at the fence). Ranges are made disjoint (a pool
// overlapping an earlier one keeps its part past that one's end) and sorted by start; entry 0 stays empty. The table
// changes with the pools and with each new mirror (gmem_mgen).
static uint64_t gmem_mgen;   // mirrors made, for vk_mem_gmem_gen (a finalized pool's mirror goes with a pool_gen bump)
uint64_t vk_mem_gmem_gen(void) {
    return __atomic_load_n(&pool_gen, __ATOMIC_ACQUIRE) + __atomic_load_n(&gmem_mgen, __ATOMIC_ACQUIRE);
}
uint64_t vk_mem_gmem_mgen(void) { return __atomic_load_n(&gmem_mgen, __ATOMIC_ACQUIRE); }

static PFN_vkGetBufferDeviceAddress p_bda;
static VkDeviceAddress buffer_address(VkCtx *c, VkBuffer b) {
    if (!p_bda) p_bda = (PFN_vkGetBufferDeviceAddress)vkGetDeviceProcAddr(c->device, "vkGetBufferDeviceAddress");
    VkBufferDeviceAddressInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = b};
    return b && p_bda ? p_bda(c->device, &bi) : 0;
}

// The device address of a pool's buffer (which 0) or of its mirror (1), asked from the driver once per buffer: the
// table is rebuilt at every pool change, several times a frame while a scene loads, and asked for some 2,000 ranges
// each time.
static VkDeviceAddress rec_address(VkCtx *c, PoolRec *r, int which) {
    VkBuffer b = which ? r->mbuf : r->buf;
    if (r->da_of[which] != b) r->da_of[which] = b, r->da[which] = buffer_address(c, b);
    return r->da[which];
}

static int gmem_add(VkMemGmemEnt *e, int *n, int cap, uint64_t lo, uint64_t hi, uint64_t delta, int cpu, uint64_t *end,
                    int *over) {
    if (lo < *end) lo = *end;
    if (lo >= hi) return 0;
    if (*n == cap) { ++*over; return 0; }
    e[(*n)++] = (VkMemGmemEnt){lo, hi, delta, cpu > 0, cpu < 0};
    *end = hi;
    return 1;
}

// The table holds GMEM_ENTRIES - 1 ranges (the translator's size). In the hero's house OoT has some 4,500
// imported pools, and filling the table in address order left the last ranges out: the device pool at 0x8000000000
// (render targets and the GPU's own buffers, 305 MiB) and the newest imported pools, a different set as pools came and
// went, so a shader's loads and stores through those addresses went nowhere. All ranges are gathered first; when
// they do not fit, the smallest imported pools (no mirror) stay out, smallest first. That is not harmless: forest
// Forest's 4,140 ranges overflowed the 4,096 entries the table had, and the pause menu's glyphs came out garbled, so the
// table has 8,192 entries now (vk_exec_draw.c GMEM_ENTRIES, translator patch 0029) and this is the last resort.
static int gmem_size_cmp(const void *x, const void *y) {
    const VkMemGmemEnt *a = *(VkMemGmemEnt *const *)x, *b = *(VkMemGmemEnt *const *)y;
    uint64_t sa = a->hi - a->lo, sb = b->hi - b->lo;
    return sa < sb ? -1 : sa > sb;
}

static int gmem_table_ordered(VkMemGmemEnt *e, int cap, uint64_t *gen);

int vk_mem_gmem_table(VkMemGmemEnt *e, int cap, uint64_t *gen) {
    static VkMemGmemEnt *all;
    static int call;
    int big = 1 << 16;
    if (!all && !(all = malloc(sizeof *all * (size_t)big))) rt_fatal("vk: out of memory (global memory pool table)");
    int n = gmem_table_ordered(all, big, gen);
    if (n <= cap) {
        memcpy(e, all, sizeof *e * (size_t)n);
        return n;
    }
    VkMemGmemEnt **by = malloc(sizeof *by * (size_t)n);
    if (!by) rt_fatal("vk: out of memory (global memory pool table)");
    int nb = 0;
    for (int i = 1; i < n; i++)
        if (all[i].cpu && !all[i].mirror) by[nb++] = &all[i];
    qsort(by, (size_t)nb, sizeof *by, gmem_size_cmp);
    int drop = n - cap;
    uint64_t dropped = 0;
    for (int i = 0; i < drop && i < nb; i++) dropped += by[i]->hi - by[i]->lo, by[i]->hi = by[i]->lo;   // empty: left out
    free(by);
    int m = 0;
    for (int i = 0; i < n && m < cap; i++)
        if (!i || all[i].hi > all[i].lo) e[m++] = all[i];
    if (call++ < 4)
        rt_log("vk: global memory pool table: %d ranges for %d entries; the %d smallest imported pools (%.1f KiB) left out",
               n - 1, cap - 1, drop, dropped / 1024.0);
    return m;
}

static int gmem_table_ordered(VkMemGmemEnt *e, int cap, uint64_t *gen) {
    VkCtx *c = vk_ctx();
    int n = 1, over = 0;
    uint64_t end = 0;
    memset(&e[0], 0, sizeof e[0]);
    if (!c || !c->has_buffer_device_address) return 1;
    plat_mutex_lock_shared(&pool_lock);
    *gen = vk_mem_gmem_gen();
    for (int i = 0; i < pool_index.n; i++) {
        PoolRec *r = pool_index.e[i].buf;
        if (r->kind == POOL_IMPORT && r->buf) {
            static int use_mirror = -1;   // TOTK_GMEM_MIRROR=0: through the import buffers (slow; for comparisons)
            if (use_mirror < 0) use_mirror = rt_knob("GMEM_MIRROR", "1");
            int mirror = use_mirror && r->mstate == MS_ON && r->mbuf;   // replay thread: mstate and mbuf change there only
            VkDeviceAddress a = rec_address(c, r, mirror);
            if (a) gmem_add(e, &n, cap, r->lo, r->lo + r->size, a - r->import_lo, mirror ? -1 : 1, &end, &over);
        } else if (r->kind == POOL_PHYS && r->buf) {
            VkDeviceAddress a = rec_address(c, r, 0);
            if (a) gmem_add(e, &n, cap, r->lo, r->lo + r->size, a - r->lo, 0, &end, &over);
        } else if (r->kind == POOL_VIRT && r->pt) {
            for (uint64_t k = 0; k < r->npages;) {
                PoolRec *ph = r->pt[k].phys;
                if (!ph || !ph->buf) { k++; continue; }
                uint64_t j = k + 1, off = r->pt[k].off;
                while (j < r->npages && r->pt[j].phys == ph && r->pt[j].off == off + (j - k) * NVN_VPOOL_PAGE) j++;
                VkDeviceAddress a = rec_address(c, ph, 0);
                uint64_t lo = r->lo + k * NVN_VPOOL_PAGE;
                if (a) gmem_add(e, &n, cap, lo, r->lo + j * NVN_VPOOL_PAGE, a + off - lo, 0, &end, &over);
                k = j;
            }
        }
    }
    plat_mutex_unlock_shared(&pool_lock);
    if (over) {
        static int32_t said;
        if (!plat_xchg32(&said, 1))
            rt_log("vk: global memory pool table: %d ranges past its %d entries are not reached", over, cap - 1);
    }
    return n;
}
#endif

void vk_mem_dirty_submit(void) {
    if (!(rt_trace & RT_TRACE_PERF) || vk_mirror_on) return;   // the mirror's own line replaces the census
    uint64_t t0 = kern_now_ns();
    uint32_t prev = dirty_sub;   // the submit whose replay noted the bindings
    dirty_sub = prev + 1 ? prev + 1 : 1;
    uint64_t eager[DC_N] = {0}, lazy[DC_N] = {0}, ln[DC_N] = {0}, lb[DC_N] = {0}, untracked = 0, queries = 0;
    plat_mutex_lock_shared(&pool_lock);
    if (dcap < pool_index.n) {
        dcap = pool_index.n * 2;
        dlist = realloc(dlist, (size_t)dcap * sizeof *dlist);
        if (!dlist) rt_fatal("vk: out of memory for the dirty census");
    }
    int n = 0;
    for (int i = 0; i < pool_index.n; i++) {
        PoolRec *r = pool_index.e[i].buf;
        if (r->kind == POOL_IMPORT && r->bufbound) dlist[n++] = r;
    }
    plat_mutex_lock(&gen_lock);
    uint32_t g = ++gen_now, since = dirty_gen;
    dirty_gen = g;
    for (int i = 0; i < n;) {   // one query per run of pools (sorted by address)
        uint64_t lo = page_lo(dlist[i]), hi = page_hi(dlist[i]);
        int j = i + 1;
        for (; j < n && page_lo(dlist[j]) <= hi + DIRTY_GAP; j++)
            if (page_hi(dlist[j]) > hi) hi = page_hi(dlist[j]);
        queries++;
        int ok = query_stamp(lo, hi, g);
        for (int k = i; k < j; k++) {
            dlist[k]->untracked = 0;
            if (!ok) queries++, dlist[k]->untracked = (uint8_t)!query_stamp(page_lo(dlist[k]), page_hi(dlist[k]), g);
        }
        i = j;
    }
    uint64_t covered = 0;
    for (int i = 0; i < n; i++) {
        PoolRec *r = dlist[i];
        int k = dc_class(r);
        ln[k]++, lb[k] += r->size;
        int fresh = r->synced_gen == DIRTY_UNSYNCED, lz = !fresh && r->bound_sub == prev;
        uint32_t ls = r->synced_gen;
        if (fresh || r->bound_sub == prev) r->synced_gen = g;
        if (r->untracked) { untracked += r->size; continue; }
        if (fresh) continue;   // counted as first use
        uint64_t lo = page_lo(r), hi = page_hi(r), ne = 0, nl = 0;
        if (lo < covered) lo = covered;   // overlapping pools: each page once
        if (lo >= hi) continue;
        covered = hi;
        for (uint64_t a = lo; a < hi;) {
            uint64_t ci = a >> GEN_CHUNK_SHIFT, ce = (ci + 1) << GEN_CHUNK_SHIFT;
            if (ce > hi) ce = hi;
            const uint32_t *ch = ci < GEN_CHUNKS ? gen_chunk[ci] : NULL;
            if (ch)
                for (uint64_t x = a; x < ce; x += 1ull << GEN_PAGE_SHIFT) {
                    uint32_t v = ch[(x >> GEN_PAGE_SHIFT) & (GEN_PER_CHUNK - 1)];
                    ne += v > since;
                    nl += lz && v > ls;
                }
            a = ce;
        }
        eager[k] += ne << GEN_PAGE_SHIFT, lazy[k] += nl << GEN_PAGE_SHIFT;
    }
    plat_mutex_unlock(&gen_lock);
    plat_mutex_unlock_shared(&pool_lock);
    uint64_t tot = 0;
    for (int k = 0; k < DC_N; k++) tot += eager[k];
    plat_mutex_lock(&dirty_lock);
    for (int k = 0; k < DC_N; k++) {
        dfr.eager[k] += eager[k], dfr.lazy[k] += lazy[k], dfr.first[k] += first_pending[k];
        first_pending[k] = 0;
        live_n[k] = ln[k], live_bytes[k] = lb[k], ever_snap_n[k] = ever_n[k], ever_snap_bytes[k] = ever_bytes[k];
    }
    live_untracked = untracked;
    dfr.submits++, dfr.queries += queries;
    if (tot > dfr.sub_max) dfr.sub_max = tot;
    dfr.scan_ns += kern_now_ns() - t0;
    plat_mutex_unlock(&dirty_lock);
}

static void dirty_frame(uint64_t frame) {
    plat_mutex_lock(&dirty_lock);
    if (dwin.frames < DIRTY_WIN)
        for (int k = 0; k < DC_N; k++) dwin_kib[dwin.frames][k] = (uint32_t)(dfr.eager[k] >> 10);
    for (int k = 0; k < DC_N; k++) dwin.eager[k] += dfr.eager[k], dwin.lazy[k] += dfr.lazy[k], dwin.first[k] += dfr.first[k];
    dwin.submits += dfr.submits, dwin.queries += dfr.queries, dwin.scan_ns += dfr.scan_ns, dwin.frames++;
    if (dfr.sub_max > dwin.sub_max) dwin.sub_max = dfr.sub_max;
    memset(&dfr, 0, sizeof dfr);
    if (frame % DIRTY_WIN) { plat_mutex_unlock(&dirty_lock); return; }
    DirtyAcc w = dwin;
    uint32_t kib[DIRTY_WIN][DC_N];
    memcpy(kib, dwin_kib, sizeof kib);
    uint64_t lv[DC_N], lvb[DC_N], ev[DC_N], evb[DC_N], unt = live_untracked;
    for (int k = 0; k < DC_N; k++) lv[k] = live_n[k], lvb[k] = live_bytes[k], ev[k] = ever_snap_n[k], evb[k] = ever_snap_bytes[k];
    memset(&dwin, 0, sizeof dwin);
    plat_mutex_unlock(&dirty_lock);
    if (!w.frames) return;
    uint64_t fl[2][3][2];
    nvn_flush_stats(fl);
    double f = (double)w.frames, M = 1048576.0;
    uint64_t sub_tot = 0;
    for (int k = 0; k < DC_N; k++) sub_tot += w.eager[k];
#define C3(a, d) (a)[0] / (d), (a)[1] / (d), (a)[2] / (d)
    rt_log("mem: dirty frame %llu: frames %llu submits_per_frame %.2f; per frame, cached/uncached/other: eager_mib "
           "%.3f/%.3f/%.3f lazy_mib %.3f/%.3f/%.3f first_mib %.3f/%.3f/%.3f flush_pool_calls %.1f/%.1f/%.1f "
           "flush_pool_mib %.3f/%.3f/%.3f flush_buf_calls %.1f/%.1f/%.1f flush_buf_mib %.3f/%.3f/%.3f; per submit: "
           "avg_mib %.3f max_mib %.3f; mirror: live_pools %llu/%llu/%llu live_mib %.1f/%.1f/%.1f ever_pools %llu/%llu/%llu "
           "ever_mib %.1f/%.1f/%.1f untracked_mib %.1f; cost: queries_per_frame %.1f scan_ms_per_frame %.3f",
           (unsigned long long)frame, (unsigned long long)w.frames, w.submits / f,
           C3(w.eager, f * M), C3(w.lazy, f * M), C3(w.first, f * M),
           fl[0][0][0] / f, fl[0][1][0] / f, fl[0][2][0] / f, fl[0][0][1] / f / M, fl[0][1][1] / f / M, fl[0][2][1] / f / M,
           fl[1][0][0] / f, fl[1][1][0] / f, fl[1][2][0] / f, fl[1][0][1] / f / M, fl[1][1][1] / f / M, fl[1][2][1] / f / M,
           w.submits ? sub_tot / (double)w.submits / M : 0.0, w.sub_max / M,
           (unsigned long long)lv[0], (unsigned long long)lv[1], (unsigned long long)lv[2], C3(lvb, M),
           (unsigned long long)ev[0], (unsigned long long)ev[1], (unsigned long long)ev[2], C3(evb, M), unt / M,
           w.queries / f, w.scan_ns / 1e6 / f);
#undef C3
    char line[DIRTY_WIN * 36 + 64];
    size_t k = (size_t)snprintf(line, sizeof line, "mem: dirty frames KiB cached/uncached/other:");
    for (uint64_t i = 0; i < w.frames && i < DIRTY_WIN; i++)
        k += (size_t)snprintf(line + k, k < sizeof line ? sizeof line - k : 0, " %u/%u/%u", kib[i][0], kib[i][1], kib[i][2]);
    rt_log("%s", line);
}

// ---------------------------------------------------------------- VRAM mirror of buffer-bound pools
// An imported pool keeps vertices, indices, constants
// and storage buffers in system memory, so a discrete GPU reads them over PCIe on every draw (GTX 1050 Ti and RTX
// 3090 Ti: about 110 ms of GPU time per frame in the field). In mirror mode (device class discrete, TOTK_MEMORY) an
// imported pool gets a device-local buffer over its imported range the first time the replay uses it as a vertex,
// index, uniform, storage or indirect buffer (vk_mem_use). From then on those bindings, and the buffer copies, fills
// and uniform updates that touch the pool (vk_mem_pick), use the mirror at the same offsets. Guest memory stays what
// the CPU reads and writes; textures and every host path that reads or writes pool bytes keep using it.
// - CPU -> mirror: the first access of a submission to a mirror syncs it (mirror_sync): the 2 MiB write-watch
//   snapshots of its regions for this epoch (snap_regions, shared with the texture checks: the OS bits keep one
//   consumer, the shared page generations) and every page stamped after the mirror's mgen goes into a copy; so do the
//   nvnMemoryPoolFlushMappedRange ranges, each on its own: one span from the lowest to the highest flush of a 252 MiB
//   pool copied up to the whole pool for a few MiB flushed, 30-50 ms of GPU time over PCIe 3.0 x8 in crowded towns
//   (TOTK_FLUSH_SPAN=1 restores it). Writes between the ranges need no copy: the write watch sees them. The first use
//   copies the whole range. The copies run in a prologue command
//   buffer that vk_sync.c submits right before the part that accessed the mirror first, between full barriers.
//   vk_mem_mirror_begin snaps the regions of the pools the previous submission accessed in a few merged queries.
// - GPU -> guest: what the replay writes into a mirror (storage buffers its shaders write, buffer copies, fills,
//   uniform updates, texture-to-buffer copies: vk_mem_gpu_wrote) goes back into the imported buffer at the end of the
//   submission, exact byte ranges, before the fence signals (vk_mem_mirror_end).
// - TOTK_MEMORY=verify: every submission also copies up to VERIFY_BYTES of mirror pages, round robin, into host memory;
//   at its fence the pages the CPU has not written since their sync are compared with guest memory.
// A pool stays imported when its mirror would not fit the VRAM budget, its range is not write-watched, it overlaps
// another pool or the allocation fails.
#define VERIFY_BYTES (4u << 20)
#define VERIFY_SAMPLES 64
#define VERIFY_LOG 16
enum { MR_BUDGET, MR_UNTRACKED, MR_OVERLAP, MR_ALLOC, MR_N, MR_DEMOTED = MR_N, MR_HOT };

int vk_mirror_on;
static int mirror_verify;
static int mirror_gap = 2;             // pre-snap: hot pools at most this many 2 MiB regions apart share one query
static PoolRec **mlist;                // pool_lock: mirrored pools by import_lo
static int nm, mcap;
static uint32_t msub;                  // replay: number of the submission being recorded
static uint32_t mepoch;                // replay: its write-watch snapshot epoch (vk_texture_epoch)
typedef struct { PoolRec *r; uint64_t off, size; } MRange;
static MRange *pend, *wbl;             // replay: copies for the next prologue; GPU writes into mirrors this submission
static int npend, cpend, nwbl, cwbl;
static VkBufferCopy *mregions;         // replay: vkCmdCopyBuffer regions
static int cregions;
static PlatMutex flush_lock = PLAT_MUTEX_INIT;
static int flush_cap = FLUSH_RANGES;   // ranges a pool keeps; 1 with TOTK_FLUSH_SPAN=1

typedef struct {
    uint64_t subs, syncs, upload_bytes, upload_copies, first_n, first_bytes, wb_bytes, wb_copies, wb_ranges;
    uint64_t presnap_queries, sync_queries, presnap_t, sync_t, prologues, presnap_bytes;
    uint64_t snap_bytes, merge_bytes, merge_dispatches, snap_allocs;   // write-back merge
    uint64_t early_flushes;   // write-backs recorded before a submission's end (vk_mem_mirror_flush)
    uint64_t page_bytes, flush_n, flush_bytes, flush_calls, flush_sum, flush_cut, merged_pages, merged_words, owned_pages, flush_skipped;   // what mirror_sync queues, by source; flush_cut: flush ranges cut around GPU pieces
} MirrorAcc;
static MirrorAcc mrep;                 // replay: this submission
static MirrorAcc macc;                 // mirror_stat_lock: since the last summary
static PlatMutex mirror_stat_lock = PLAT_MUTEX_INIT;
static volatile int64_t m_live_n, m_live_bytes, m_refused_n[MR_N], m_refused_bytes[MR_N];
static volatile int64_t m_flush_calls, m_flush_bytes, v_pages, v_skipped, v_mismatch;

static void mirror_mode(int discrete) {
    const char *e = cfg_value("TOTK_MEMORY");
    int mode = discrete ? 1 : 0;   // 0 import, 1 mirror, 2 verify
    const char *why = discrete ? "auto, discrete gpu" : "auto, uma";
    if (strcmp(e, "auto")) {
        if (!strcmp(e, "import")) mode = 0, why = "TOTK_MEMORY=import";
        else if (!strcmp(e, "mirror")) mode = 1, why = "TOTK_MEMORY=mirror";
        else if (!strcmp(e, "verify")) mode = 2, why = "TOTK_MEMORY=verify";
        else rt_log("mem: TOTK_MEMORY=%s is not import, mirror, verify or auto: auto", e);
    }
    vk_mirror_on = mode > 0;
    mirror_verify = mode == 2;
    const char *g = cfg_get("TOTK_MIRROR_GAP");
    mirror_gap = g && *g ? atoi(g) : 2;
    if (mirror_gap < 0) mirror_gap = 0;
    // Submissions a region stays unwritten before it is frozen. 60 rather than 300: in a fight 100 of the 257 MiB
    // queried a frame lay in regions idle for 30-299 submissions, and a thaw by a write costs about 60 us.
    if (rt_knob("FLUSH_SPAN", "0")) {
        flush_cap = 1;
        rt_log("mem: a pool's flushes copy one span from the lowest to the highest (TOTK_FLUSH_SPAN=1)");
    }
    const char *fi = cfg_get("TOTK_FREEZE_IDLE");
    freeze_idle = fi && *fi ? atoi(fi) : 60;
    if (freeze_idle < 0) freeze_idle = 0;
    const char *sh = cfg_get("TOTK_SHADOW");
#ifdef _WIN32
    shadow_on = sh && *sh ? strcmp(sh, "0") != 0 : plat_is_wine();
#else
    shadow_on = !sh || !*sh || strcmp(sh, "0");
#endif
    if (shadow_on) rt_log("mem: shadow storage: CPU-visible pools are host-visible copies of guest memory, not imports%s",
                          sh && *sh ? " (TOTK_SHADOW)" : "");
    rt_log("mem: mode %s (%s): %s", mode == 2 ? "verify" : mode ? "mirror" : "import", why,
           mode ? "pools used as buffers get a device-local mirror" : "every CPU-visible pool is imported host memory");
    // Import is the integrated-GPU mode. On a discrete card every draw reads the pools over PCIe (the comment above
    // this section has the measurements), which is the difference between playable and 5 fps when it is not intended.
    if (!mode && discrete)
        rt_log("mem: warning: import on a discrete GPU: the GPU reads the game's pools over PCIe on every draw, far "
               "slower than mirror (the auto default on a discrete GPU); use memory = auto unless testing");
}

static void range_add(MRange **a, int *n, int *cap, PoolRec *r, uint64_t off, uint64_t size) {
    if (*n && (*a)[*n - 1].r == r && (*a)[*n - 1].off + (*a)[*n - 1].size == off) {
        (*a)[*n - 1].size += size;
        return;
    }
    if (*n == *cap) grow((void **)a, cap, sizeof **a);
    (*a)[(*n)++] = (MRange){r, off, size};
}

static int range_cmp(const void *x, const void *y) {
    const MRange *a = x, *b = y;
    if (a->r != b->r) return (uintptr_t)a->r < (uintptr_t)b->r ? -1 : 1;
    return a->off < b->off ? -1 : a->off > b->off;
}

// DEBUG-UPLOG: TOTK_UPLOG=<pool address>: every range copied from guest memory into that pool's mirror, with the
// frame (20,000 lines at most): a pool the GPU writes should take nothing, and what it takes is older than its mirror.
static void uplog(const PoolRec *r, uint64_t off, uint64_t size, const char *how) {
    static uint64_t want = 1;
    static int n;
    if (want == 1) { const char *e = cfg_get("TOTK_UPLOG"); want = e ? strtoull(e, NULL, 0) : 0; }
    if (!want || r->lo != want || n >= 20000) return;
    n++;
    rt_log("UPLOG frame %llu %s +%#llx %llu", (unsigned long long)(__atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) + 1), how,
           (unsigned long long)(r->import_lo + off - r->lo), (unsigned long long)size);
}

// Records the ranges as one vkCmdCopyBuffer per pool, overlapping and adjacent ranges merged; to_mirror: buf -> mbuf,
// else mbuf -> buf. Returns the commands recorded; adds what they move to *bytes and *copies.
static int copy_ranges(VkCommandBuffer cb, MRange *a, int n, int to_mirror, uint64_t *bytes, uint64_t *copies) {
    qsort(a, (size_t)n, sizeof *a, range_cmp);
    int rec = 0;
    for (int i = 0; i < n;) {
        PoolRec *r = a[i].r;
        uint64_t lim = r->import_hi - r->import_lo;
        int k = 0;
        for (; i < n && a[i].r == r; i++) {
            uint64_t lo = a[i].off, hi = a[i].off + a[i].size;
            if (hi > lim) hi = lim;
            if (lo >= hi) continue;
            if (k && lo <= mregions[k - 1].srcOffset + mregions[k - 1].size) {
                uint64_t end = mregions[k - 1].srcOffset + mregions[k - 1].size;
                if (hi > end) mregions[k - 1].size += hi - end;
                continue;
            }
            if (k == cregions) grow((void **)&mregions, &cregions, sizeof *mregions);
            mregions[k++] = (VkBufferCopy){lo, lo, hi - lo};
        }
        if (!k) continue;
        if (to_mirror) vkCmdCopyBuffer(cb, r->buf, r->mbuf, (uint32_t)k, mregions);
        else vkCmdCopyBuffer(cb, r->mbuf, r->buf, (uint32_t)k, mregions);
        rec++;
        for (int j = 0; j < k; j++) *bytes += mregions[j].size;
        if (to_mirror) for (int j = 0; j < k; j++) r->up_bytes += mregions[j].size, r->hot_bytes += mregions[j].size;
        if (to_mirror) for (int j = 0; j < k; j++) uplog(r, mregions[j].srcOffset, mregions[j].size, "copy");
        *copies += (uint64_t)k;
    }
    return rec;
}

static void mbarrier(VkCommandBuffer cb, VkPipelineStageFlags2 ss, VkAccessFlags2 sa, VkPipelineStageFlags2 ds,
                     VkAccessFlags2 da) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = ss, .srcAccessMask = sa,
                           .dstStageMask = ds, .dstAccessMask = da};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
}

static void refuse(PoolRec *r, int why, const char *what) {
    r->mstate = MS_REFUSED;
    r->mwhy = (uint8_t)why;
    r->mtry = (uint32_t)__atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    uint64_t n = r->import_hi - r->import_lo;
    if (sec_on && why == MR_BUDGET) sec_count(SEC_MR_BUDGET, 1);
    if (plat_inc64(&m_refused_n[why]) <= 4) {
        char f[160];
        flag_names(r->flags, f, sizeof f);
        rt_log("mem: pool %#llx+%#llx (%s) stays imported: %s", (unsigned long long)r->lo, (unsigned long long)r->size,
               f, what);
    }
    plat_add64(&m_refused_bytes[why], (int64_t)n);
}

// Replay thread: the pool's mirror on its first buffer use.
static void mirror_create(PoolRec *r) {
    VkCtx *c = vk_ctx();
    uint64_t n = r->import_hi - r->import_lo;
    r->mstate = MS_REFUSED;
    __atomic_add_fetch(&mirror_gen, 1, __ATOMIC_RELEASE);
    if (r->kind != POOL_IMPORT || !r->buf || !n) return;
    plat_mutex_lock_shared(&pool_lock);
    int i = pidx_upper(&pool_index, r->lo) - 1;
    while (i >= 0 && pool_index.e[i].buf != r) i--;
    int alone = i >= 0 && (i == 0 || pool_index.maxhi[i - 1] <= pool_index.e[i].lo) &&
                (i + 1 == pool_index.n || pool_index.e[i + 1].lo >= pool_index.e[i].hi);
    plat_mutex_unlock_shared(&pool_lock);
    if (i < 0) return;   // finalized meanwhile
    if (!alone) return refuse(r, MR_OVERLAP, "it overlaps another pool");
    uint64_t r0 = r->import_lo >> SNAP_SHIFT, r1 = (r->import_hi - 1) >> SNAP_SHIFT;
    if (r1 >= SNAP_REGIONS) return refuse(r, MR_UNTRACKED, "outside the write-watched address space");
    // The budget test (an eighth of the budget stays free after the mirror) is for pools over 32 MiB. A smaller pool
    // always gets its mirror: without one it is bound in the game's live memory, which costs the early results point
    // (vk_mem_host_read_note). In the field, with 2.5 GiB of a 12 GiB card held by other programs, 120 pools of
    // 58 MiB in all were refused and the frame rate went from 96 to 61 fps for them; with the line at 97 % of the
    // budget 49 pools of 36 MiB still were. A refusal of a large pool is asked again 300 frames later (vk_mem_use): it
    // used to stand for the pool's life, whatever the video memory did afterwards.
    uint64_t used, budget;
    int exact;
    if (n > (32u << 20) && vk_mem_vram(&used, &budget, &exact) && used + n + budget / 8 > budget) {
        char why[128];
        snprintf(why, sizeof why, "its %.1f MiB mirror does not fit the VRAM budget (%.0f of %.0f MiB in use)",
                 n / 1048576.0, used / 1048576.0, budget / 1048576.0);
        return refuse(r, MR_BUDGET, why);
    }
    plat_mutex_lock(&gen_lock);
    mrep.sync_queries += snap_regions(r0, r1, mepoch);
    int untracked = 0;
    for (uint64_t k = r0; k <= r1; k++) untracked |= snap_untracked[k];
    r->mgen = gen_now;   // the whole-range copy below holds every write observed so far
    plat_mutex_unlock(&gen_lock);
    if (untracked) return refuse(r, MR_UNTRACKED, "its range is not write-watched");
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = n, .usage = pool_usage(c),
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
                                  .requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT};
    VkResult res = vmaCreateBuffer(c->vma, &bi, &ai, &r->mbuf, &r->malloc_, NULL);
    if (res != VK_SUCCESS) {
        r->mbuf = VK_NULL_HANDLE, r->malloc_ = NULL;
        char why[96];
        snprintf(why, sizeof why, "device-local allocation of %.1f MiB failed (%d)", n / 1048576.0, (int)res);
        return refuse(r, MR_ALLOC, why);
    }
    plat_mutex_lock(&pool_lock);
    if (nm == mcap) grow((void **)&mlist, &mcap, sizeof *mlist);
    int at = nm;
    while (at > 0 && mlist[at - 1]->import_lo > r->import_lo) at--;
    memmove(mlist + at + 1, mlist + at, (size_t)(nm - at) * sizeof *mlist);
    mlist[at] = r;
    nm++;
    plat_mutex_unlock(&pool_lock);
    r->acc_sub = msub;
    r->mstate = MS_ON;
#ifdef GAME_NS2
    __atomic_add_fetch(&gmem_mgen, 1, __ATOMIC_RELEASE);   // the global memory table points into the mirror from now on
#endif
    range_add(&pend, &npend, &cpend, r, 0, n);
    mrep.first_n++, mrep.first_bytes += n;
    plat_inc64(&m_live_n);
    plat_add64(&m_live_bytes, (int64_t)n);
}

static void mirror_forget(PoolRec *r) {
    __atomic_add_fetch(&mirror_gen, 1, __ATOMIC_RELEASE);
    for (int i = 0; i < nm; i++)
        if (mlist[i] == r) {
            memmove(mlist + i, mlist + i + 1, (size_t)(nm - i - 1) * sizeof *mlist);
            nm--;
            plat_dec64(&m_live_n);
            plat_add64(&m_live_bytes, -(int64_t)(r->import_hi - r->import_lo));
            break;
        }
}

// Finalized mirrored pools waiting for mirror_retire; vk_mem_shutdown destroys those whose callback never ran.
static PoolRec **retiring;
static int nretiring, cretiring;
static PlatMutex retire_lock = PLAT_MUTEX_INIT;

static void lz_pool_gone(PoolRec *r);
static void mirror_retiring(PoolRec *r) {
    lz_pool_gone(r);
    plat_mutex_lock(&retire_lock);
    if (nretiring == cretiring) grow((void **)&retiring, &cretiring, sizeof *retiring);
    retiring[nretiring++] = r;
    plat_mutex_unlock(&retire_lock);
}

static void mirror_retire(void *arg) {
    plat_mutex_lock(&retire_lock);
    for (int i = 0; i < nretiring; i++)
        if (retiring[i] == arg) { retiring[i] = retiring[--nretiring]; break; }
    plat_mutex_unlock(&retire_lock);
    destroy_rec(vk_ctx(), arg);
}

// Replay thread, the first access of this submission: queues the copies of the pages written since the last sync.
static void mirror_sync(PoolRec *r) {
    uint64_t t0 = qpc();
    r->acc_sub = msub;
    uint64_t lo = r->import_lo, hi = r->import_hi;
    uint64_t r0 = lo >> SNAP_SHIFT, r1 = (hi - 1) >> SNAP_SHIFT;
    int excl = 0;
    plat_mutex_lock_shared(&gen_lock);
    for (uint64_t k = r0; k <= r1; k++)
        if (snap_at[k] != mepoch) { excl = 1; break; }
    if (excl) {
        plat_mutex_unlock_shared(&gen_lock);
        plat_mutex_lock(&gen_lock);
        mrep.sync_queries += snap_regions(r0, r1, mepoch);
    }
    uint32_t since = r->mgen;
    int untracked = 0;
    for (uint64_t k = r0; k <= r1; k++) {
        untracked |= snap_untracked[k];
        if (reg_max[k] <= since) continue;
        uint64_t a = k << SNAP_SHIFT, b = a + (1ull << SNAP_SHIFT);
        if (a < lo) a = lo;
        if (b > hi) b = hi;
        const uint32_t *ch = gen_chunk[a >> GEN_CHUNK_SHIFT];   // a 2 MiB region lies in one chunk
        if (!ch) continue;
        for (uint64_t x = a; x < b; x += 1ull << GEN_PAGE_SHIFT) {
            uint32_t st = ch[(x >> GEN_PAGE_SHIFT) & (GEN_PER_CHUNK - 1)];
            // the mirror wrote these bytes, or holds the only copy of them (a cold page is closed: the CPU did not
            // write it, and what guest memory holds there is older than the mirror)
            if (st > since && (page_owned(x, st) || lz_cold(x))) { mrep.owned_pages++; continue; }
            if (st > since)
                if (!gmem_hot_page(x) && !gmem_merge_page(r, x))   // DEBUG-HOTNOSYNC
                range_add(&pend, &npend, &cpend, r, x - lo, 1ull << GEN_PAGE_SHIFT), mrep.page_bytes += 1ull << GEN_PAGE_SHIFT;
        }
    }
    {   // DEBUG: TOTK_SYNCLOG=<import_lo>: pages of that pool each submission's first access copies
        static uint64_t want = 1;
        static int nl;
        if (want == 1) { const char *e = cfg_get("TOTK_SYNCLOG"); want = e ? strtoull(e, NULL, 0) : 0; }
        static uint64_t from = 1;
        if (from == 1) { const char *e = cfg_get("TOTK_SYNCLOG_FROM"); from = e ? strtoull(e, NULL, 0) : 0; }
        if (want && r->import_lo == want && nl < 6000 &&
            __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) + 1 >= from) {
            uint64_t pages = 0, first = ~0ull, last = 0;
            for (int i = 0; i < npend; i++)
                if (pend[i].r == r) {
                    pages += pend[i].size >> GEN_PAGE_SHIFT;
                    if (pend[i].off < first) first = pend[i].off;
                    if (pend[i].off + pend[i].size > last) last = pend[i].off + pend[i].size;
                }
            // (only the submissions that copy something, once past TOTK_SYNCLOG_FROM: a pool the GPU writes should copy nothing)
            if (pages && from)
                nl++, rt_log("SYNCLOG frame %llu sub %u pages %llu range +%#llx..+%#llx",
                             (unsigned long long)(__atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) + 1), (unsigned)msub,
                             (unsigned long long)pages, (unsigned long long)first, (unsigned long long)last);
            else if (!from) {
                if (pages) rt_log("SYNCLOG   range +%#llx..+%#llx", (unsigned long long)first, (unsigned long long)last);
                nl++, rt_log("SYNCLOG frame %llu sub %u pages %llu", (unsigned long long)(__atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) + 1),
                             (unsigned)msub, (unsigned long long)pages);
            }
        }
    }
    r->mgen = gen_now;
    if (excl) plat_mutex_unlock(&gen_lock);
    else plat_mutex_unlock_shared(&gen_lock);
    if (__atomic_load_n(&r->fl_n, __ATOMIC_ACQUIRE)) {
        FlushRange fl[FLUSH_RANGES + 1];
        plat_mutex_lock(&flush_lock);
        int n = r->fl_n;
        memcpy(fl, r->fl, (size_t)n * sizeof *fl);
        uint64_t calls = r->fl_calls, sum = r->fl_sum;
        r->fl_n = 0, r->fl_calls = r->fl_sum = 0;
        plat_mutex_unlock(&flush_lock);
        mrep.flush_calls += calls, mrep.flush_sum += sum;
        for (int i = 0; i < n; i++) {
            uint64_t a = fl[i].lo > lo ? fl[i].lo : lo, b = fl[i].hi < hi ? fl[i].hi : hi;
            if (a >= b) continue;
            // Where the write watch covers the pool, a flushed page the CPU wrote is a written page already (the loop
            // above); the game's flushes of ranges it did not write only cost copies over the bus (TOTK_FLUSH_UPLOAD=1
            // copies them anyway).
            static int flush_upload = -1;
            if (flush_upload < 0) flush_upload = rt_knob("FLUSH_UPLOAD", "0") == 1;
            if (!untracked && !flush_upload) { mrep.flush_skipped += b - a; continue; }
            flush_range_add(r, a, b);
            mrep.flush_n++, mrep.flush_bytes += b - a;
            if ((rt_trace & RT_TRACE_PERF) && b - a >= 16u << 20)
                rt_log("mem: mirror frame %llu: flush range %.1f MiB at +%.1f MiB of a %.1f MiB pool (%d ranges from %llu "
                       "flushes of %.3f MiB)", (unsigned long long)mirror_frame, (b - a) / 1048576.0, (a - lo) / 1048576.0,
                       (hi - lo) / 1048576.0, n, (unsigned long long)calls, sum / 1048576.0);
        }
    }
    mrep.syncs++;
    mrep.sync_t += qpc() - t0;
}

// TOTK_TRACE=perf: buffer uses (vertex, index, uniform, storage, indirect) that stay on an imported pool's host
// memory, per pool, logged with the mirror statistics: the GPU reads and atomics there cross the bus.
#define HB_N 64
static struct { const PoolRec *r; uint64_t lo, size; uint32_t uses; uint8_t why; } hb[HB_N];
static int nhb;
static uint64_t hb_uses, hb_lost;
static PlatMutex hb_lock = PLAT_MUTEX_INIT;   // the replay notes, vk_mem_census logs
static void host_bind_note(const PoolRec *r) {   // replay thread
    plat_mutex_lock(&hb_lock);
    hb_uses++;
    int i = 0;
    while (i < nhb && hb[i].r != r) i++;
    if (i < nhb) hb[i].uses++;
    else if (nhb == HB_N) hb_lost++;
    else hb[nhb].r = r, hb[nhb].lo = r->lo, hb[nhb].size = r->size, hb[nhb].uses = 1, hb[nhb].why = r->mwhy, nhb++;
    plat_mutex_unlock(&hb_lock);
}

static void host_bind_log(uint64_t frame, double frames) {   // vk_mem_census (mirror_log)
    plat_mutex_lock(&hb_lock);
    static const char *why[] = {"budget", "untracked", "overlap", "alloc", "demoted", "hot"};
    char line[1024];
    size_t o = 0;
    for (int k = 0; k < 12 && nhb; k++) {
        int b = 0;
        for (int i = 1; i < nhb; i++) if (hb[i].uses > hb[b].uses) b = i;
        if (!hb[b].uses) break;
        if (o < sizeof line - 96)
            o += (size_t)snprintf(line + o, sizeof line - o, " %#llx+%#llx(%s)=%.1f", (unsigned long long)hb[b].lo,
                                  (unsigned long long)hb[b].size, hb[b].why <= MR_HOT ? why[hb[b].why] : "?",
                                  hb[b].uses / frames);
        hb[b].uses = 0;
    }
    rt_log("mem: host-memory buffer uses frame %llu: %.1f per frame over %d pools (+%llu uses not listed); top per frame:%s",
           (unsigned long long)frame, hb_uses / frames, nhb, (unsigned long long)hb_lost, o ? line : " none");
    nhb = 0, hb_uses = hb_lost = 0;
    plat_mutex_unlock(&hb_lock);
}

// GPU reads of the game's live memory. A pool whose mirror was refused (the video memory budget) is bound where the
// game writes it: the buffer binding stays on the import buffer (vk_mem_use). Reading the game's memory in place is
// the game's own race on the console too, but the console's GPU is done with a
// frame before the game goes on; here the results point (vk_exec.c vk_exec_results_point) lets the game go on at the
// first lighting pass, and it then rewrites next frame's blocks and pointers while that pass still reads them. In
// The Market, where video memory is short and mirrors are refused, a shader then loaded through a pointer the
// game was rewriting: a read at an address freed 12 to 45 s before, a lost device in one run of ten with one replay
// thread and in two of three once chunk threads recorded the lighting pass later (2026-11-12; with TOTK_RESULTS_FENCE=0
// four of four runs clean). vk_mem_host_read_recent says such a binding was noted in the last 120 frames: the results
// point is then left out and the game waits for the whole frame (Market: about 80 fps instead of 96 while video memory
// is short). Pools reached only through the global memory table are read in place everywhere, the forest
// included, where no run lost the device: counting them left the point out always (forest 63 fps), so they do not count.
static volatile uint64_t host_read_frame;
void vk_mem_host_read_note(void) {
    uint64_t f = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) + 1;
    if (__atomic_load_n(&host_read_frame, __ATOMIC_RELAXED) != f) __atomic_store_n(&host_read_frame, f, __ATOMIC_RELAXED);
}
int vk_mem_host_read_recent(void) {
    uint64_t f = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) + 1, h = __atomic_load_n(&host_read_frame, __ATOMIC_RELAXED);
    return h && f - h < 120;
}

// The draws of a stretch are recorded by several threads at once (vk_exec.c "Draw stretches on worker threads"): a
// mirror's creation and its first sync of the submission run under use_lock, one thread at a time; a mirror that is on
// and synced for this submission is read without it.
static PlatRMutex use_lock = PLAT_RMUTEX_INIT;

void vk_mem_use(VkMemSpan *sp) {
    PoolRec *r = sp->pool;
    if (r->mstate == MS_ON && r->acc_sub == msub) {
        sp->buffer = r->mbuf;
        return;
    }
    plat_rmutex_lock(&use_lock);
    if (r->mstate == MS_REFUSED && r->mwhy == MR_BUDGET &&
        (uint32_t)__atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) - r->mtry >= 300) {   // the budget may have room now
        plat_add64(&m_refused_n[MR_BUDGET], -1);
        plat_add64(&m_refused_bytes[MR_BUDGET], -(int64_t)(r->import_hi - r->import_lo));
        r->mstate = MS_NONE;
    }
    if (r->mstate == MS_NONE) {
        uint64_t sec0 = sec_begin();
        mirror_create(r);
        sec_end(SEC_MIR_CREATE, sec0);
    }
    if (r->mstate != MS_ON) {
        vk_mem_host_read_note();   // the GPU reads the game's memory in place
        if (rt_trace & RT_TRACE_PERF) host_bind_note(r);
        plat_rmutex_unlock(&use_lock);
        return;
    }
    if (r->acc_sub != msub) {
        uint64_t sec0 = sec_begin();
        mirror_sync(r);
        sec_end(SEC_MIR_SYNC, sec0);
    }
    sp->buffer = r->mbuf;
    plat_rmutex_unlock(&use_lock);
}

void vk_mem_pick(VkMemSpan *sp) {
    PoolRec *r = sp->pool;
    if (!r || r->mstate != MS_ON) return;
    if (r->acc_sub != msub) {
        plat_rmutex_lock(&use_lock);
        if (r->acc_sub != msub) {
            uint64_t sec0 = sec_begin();
            mirror_sync(r);
            sec_end(SEC_MIR_SYNC, sec0);
        }
        plat_rmutex_unlock(&use_lock);
    }
    sp->buffer = r->mbuf;
}

#ifdef GAME_NS2
// Replay thread, before a submission's first access through the global memory table: every mirror holds what the CPU
// wrote since its last sync, as a buffer binding of each pool would have made it (the table cannot tell which pools a
// shader will reach). Once per submission and set of mirrors.
static int gmem_resync;   // vk_mem_resync_after_wait: the next table access syncs every mirror again
void vk_mem_gmem_sync(void) {
    static uint32_t done_sub;
    static uint64_t done_gen = ~0ull;
    if (gmem_resync) gmem_resync = 0, done_gen = ~0ull;
    uint64_t g = __atomic_load_n(&gmem_mgen, __ATOMIC_ACQUIRE);
    if (!vk_mirror_on || (done_sub == msub && done_gen == g)) return;
    {   // DEBUG-GMEM-MIRROR-ALL: TOTK_GMEM_MIRROR_ALL=1: every imported pool the table reaches gets a mirror (A/B: the
        // table's loads from the import buffers read the CPU's live bytes, not the submission's)
        static int all = -1;
        if (all < 0) { const char *e = cfg_get("TOTK_GMEM_MIRROR_ALL"); all = e && *e == '1'; }
        if (all) {
            plat_mutex_lock_shared(&pool_lock);
            int nc = 0;
            PoolRec **c = pool_index.n ? malloc(sizeof *c * (size_t)pool_index.n) : NULL;
            for (int i = 0; c && i < pool_index.n; i++) {
                PoolRec *r = pool_index.e[i].buf;
                if (r->kind == POOL_IMPORT && r->buf && r->mstate == MS_NONE) c[nc++] = r;
            }
            plat_mutex_unlock_shared(&pool_lock);
            uint64_t bytes = 0;
            for (int i = 0; i < nc; i++) {
                if (c[i]->mstate != MS_NONE) continue;
                mirror_create(c[i]);
                if (c[i]->mstate == MS_ON) bytes += c[i]->import_hi - c[i]->import_lo;
            }
            if (nc) rt_log("mem: TOTK_GMEM_MIRROR_ALL: %d pools considered, %.1f MiB mirrored", nc, bytes / 1048576.0);
            free(c);
            g = __atomic_load_n(&gmem_mgen, __ATOMIC_ACQUIRE);
        }
    }
    done_sub = msub, done_gen = g;
    plat_mutex_lock_shared(&pool_lock);
    int n = 0;
    PoolRec **v = nm ? malloc(sizeof *v * (size_t)nm) : NULL;
    for (int i = 0; v && i < nm; i++)
        if (mlist[i]->mstate == MS_ON && mlist[i]->acc_sub != msub) v[n++] = mlist[i];
    plat_mutex_unlock_shared(&pool_lock);
    for (int i = 0; i < n; i++)   // a finalized pool's record outlives the submission being recorded (mirror_retire)
        if (v[i]->acc_sub != msub) mirror_sync(v[i]);
    free(v);
}

#endif

// A wait in the command stream for the CPU's signal (OP_WAIT_EVENT) has ended: what the CPU wrote before signalling
// belongs to the commands after the wait, as the console's GPU, which stopped there, reads it. Every mirror the
// submission has synced syncs again at its next access (the pages written since go into the next part's prologue).
void vk_mem_resync_after_wait(void) {
    if (!vk_mirror_on) return;
    plat_mutex_lock_shared(&pool_lock);
    for (int i = 0; i < nm; i++)
        if (mlist[i]->acc_sub == msub) mlist[i]->acc_sub = msub - 1 ? msub - 1 : ~0u;
    plat_mutex_unlock_shared(&pool_lock);
#ifdef GAME_NS2
    gmem_resync = 1;
#endif
}

// A deferred wait for the CPU's signal is met (vk_exec_xfer.c vk_exec_wait_resolve) before the part after it goes to the
// GPU. The mirrors synced since the wait (vk_mem_resync_after_wait set the others back) were synced before the signal:
// their regions are asked again what the CPU wrote (a new write-watch query within the submission's epoch), and the
// pages written since go into that part's prologue.
void vk_mem_resync_accessed(void) {
    if (!vk_mirror_on) return;
    plat_mutex_lock_shared(&pool_lock);
    int n = 0;
    PoolRec **v = nm ? malloc(sizeof *v * (size_t)nm) : NULL;
    for (int i = 0; v && i < nm; i++)
        if (mlist[i]->mstate == MS_ON && mlist[i]->acc_sub == msub) v[n++] = mlist[i];
    plat_mutex_unlock_shared(&pool_lock);
    if (n) {
        plat_mutex_lock(&gen_lock);
        for (int i = 0; i < n; i++)
            for (uint64_t k = v[i]->import_lo >> SNAP_SHIFT; k <= (v[i]->import_hi - 1) >> SNAP_SHIFT; k++) snap_at[k] = 0;
        plat_mutex_unlock(&gen_lock);
    }
    for (int i = 0; i < n; i++) mirror_sync(v[i]);
    free(v);
}

int vk_mem_mirror_probe(uint64_t addr) {
    VkMemSpan sp;
    if (!vk_mirror_on || !vk_mem_resolve(addr, 1, &sp) || !sp.pool) return 0;
    mepoch = vk_texture_epoch();
    vk_mem_use(&sp);
    return ((PoolRec *)sp.pool)->mstate == MS_ON;
}

// ---- write-back merge
// A storage buffer a shader writes whole records of does not
// go back to guest memory as a plain copy. The CPU may write fields of the same records while the GPU runs: TotK's
// particle emitters write a new particle's whole record into a free slot of a buffer that the simulation shaders update
// (the waterfall splashes and mist, 2026-11-02), and a plain copy puts the mirror's stale copy of those words back over
// them. Instead:
// - record time (gpu_wrote from vk_mem_gpu_wrote_shader): each distinct range of the submission gets room in a snapshot
//   chunk (snap_add);
// - each part's prologue, after the CPU -> mirror copies: the mirror's words of the ranges the part added are copied
//   into their snapshots (snap_copies), the state before the part's GPU work. Words of an earlier part's writes that a
//   later range overlaps are covered by the earlier range's own snapshot;
// - vk_mem_mirror_end: shaders/wb_merge.comp writes each 32-bit word that differs between the mirror and the snapshot
//   into the pool's imported buffer (wb_merge_record), one dispatch per pool and group of ranges.
// Words the GPU left unchanged keep the CPU's value; a word both changed takes the GPU's, as a plain copy would, except
// in ranges a shader adds into with float atomics (GFX_SSBO_FADD, SnapRange.fadd): there the GPU's change (mirror minus
// snapshot) is added to what guest memory holds by then. OoT's auto exposure adds the frame's luminance
// into a word of such a range (a ring of three 8 KiB pools from 0x25fb7b000) and the CPU reads the word and writes 0
// back each frame (f_7000337060); with a frame in flight that reset fell between the snapshot and the write-back, the
// GPU's value went back over it, the reads saw sums of ever more frames (209, 417, 625, ... up to 59519 and over), and
// the exposure jumped between its clamp and the right value: the forest flickered and turned dark. Taking those pools
// off the mirror (atomics live in guest memory, as on the console) fixed the sum, but the CPU's records in other
// such pools then raced the GPU (the far trees lost their leaves every eighth frame).
// TOTK_WB_MERGE=0 restores the plain copies; so does a pipeline or allocation failure.
#define SNAP_CHUNK (1u << 20)
#define MERGE_SPAN (64ull << 20)   // the bytes of one pool a merge dispatch binds at most
#define MERGE_WORDS (65535u * 64u) // the words one merge dispatch covers at most
// Larger written ranges stay plain copies (TOTK_MERGE_MAX_MB, default 4): with frames in flight a plain copy, which
// the GPU makes at the end of the submission, puts the mirror's words back over what the CPU wrote meanwhile.
static uint64_t merge_max(void) {
    static uint64_t m;
    if (!m) { const char *e = cfg_get("TOTK_MERGE_MAX_MB"); int mb = e ? atoi(e) : 0; m = (uint64_t)(mb > 0 ? mb : 4) << 20; }
    return m;
}
#define MERGE_MAX merge_max()
// Whether the CPU has written a page of [a, a + n) (in the last TOTK_MERGE_IDLE_FRAMES frames; default 0: ever), or
// the range's writes are not tracked. Only then can a plain copy at the end of the submission put the mirror's stale
// words over words the CPU wrote while the GPU ran; a range only the GPU writes goes back as a plain copy, the same bytes
// as the merge. On the forest porch the merges take ~20 MiB a frame through the shader into host memory (2.9 ms of GPU
// time) where the copy engine moves plain write-backs at ~46 GB/s, but every merged range there has pages the CPU wrote
// once: with a 600-frame window 12 MiB of them went back plainly (GPU 14.8 -> 12.9 ms) and a camera cut drew another
// frame's shadows (the CPU wrote such pages again after 600 idle frames, while the GPU ran). TOTK_MERGE_ALL=1 merges
// every range as before.
static int cpu_wrote_lately(uint64_t a, uint64_t n) {
    static int all = -1;
    static uint32_t idle;
    if (all < 0) {
        all = cfg_get("TOTK_MERGE_ALL") && atoi(cfg_get("TOTK_MERGE_ALL")) > 0;
        const char *e = cfg_get("TOTK_MERGE_IDLE_FRAMES");
        idle = e && atoi(e) > 0 ? (uint32_t)atoi(e) : 0;
    }
    if (all) return 1;
    uint32_t now = (uint32_t)__atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) + 1;
    for (uint64_t pg = a & ~((1ull << GEN_PAGE_SHIFT) - 1); pg < a + n; pg += 1ull << GEN_PAGE_SHIFT) {
        if ((pg >> SNAP_SHIFT) >= SNAP_REGIONS || snap_untracked[pg >> SNAP_SHIFT]) return 1;
        const uint32_t *c = (pg >> GEN_CHUNK_SHIFT) < GEN_CHUNKS ? cpuw_chunk[pg >> GEN_CHUNK_SHIFT] : NULL;
        uint32_t f = c ? c[(pg >> GEN_PAGE_SHIFT) & (GEN_PER_CHUNK - 1)] : 0;
        if (f && (!idle || now - f < idle)) return 1;
    }
    return 0;
}
typedef struct { PoolRec *r; uint64_t off, size, soff; int chunk, fadd; } SnapRange;
typedef struct { VkBuffer buf; VmaAllocation alloc; uint64_t size, used; int host; } SnapChunk;
typedef struct { int n; SnapChunk c[]; } SnapFree;   // a submission's chunks and range lists, pooled at its fence
static SnapRange *snl;              // replay: this submission's merged ranges; [0, snl_copied) have their snapshot copy
static int nsnl, csnl, snl_copied;
static SnapChunk *chunks;           // replay: this submission's snapshot chunks and merge range lists
static int nchunks, cchunks;
// Chunks a finished submission used (snap_free) wait here for chunk_new, so a frame with merges creates no buffer once
// the pool holds the frames in flight: a VMA allocation that needs a new memory block costs 8-11 ms on the replay
// thread. Chunk sizes are powers of two so that one fits the next request; what the pool cannot hold is destroyed.
#define SNAP_LIST_MIN (64u << 10)
#define SNAP_POOL_N 64
// A forest frame merges 20-30 MiB of snapshots; a pool smaller than that destroyed and re-created two chunks a frame,
// which is a VMA block allocation or a vkBindBufferMemory on the replay thread each time. TOTK_SNAP_POOL_MB (default 64).
static uint64_t snap_pool_max(void) {
    static uint64_t m;
    if (!m) { const char *e = cfg_get("TOTK_SNAP_POOL_MB"); int mb = e ? atoi(e) : 64; m = (uint64_t)(mb > 0 ? mb : 16) << 20; }
    return m;
}
#define SNAP_POOL_BYTES snap_pool_max()
static PlatMutex snap_pool_lock = PLAT_MUTEX_INIT;
static SnapChunk snap_pool[SNAP_POOL_N];
static int nsnap_pool;
static uint64_t snap_pool_bytes;
static int merge_on = -1;
static struct {
    int tried, ok;
    PFN_vkCmdPushDescriptorSetKHR push;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkPipeline pipe;
} WM;

static uint32_t merge_group = 64;   // invocations per merge workgroup (merge_init)
static int merge_init(VkCtx *c) {
    WM.tried = 1;
    WM.push = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(c->device, "vkCmdPushDescriptorSetKHR");
    VkDescriptorSetLayoutBinding b[4];
    for (int i = 0; i < 4; i++)
        b[i] = (VkDescriptorSetLayoutBinding){.binding = (uint32_t)i, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                              .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                           .bindingCount = 4, .pBindings = b};
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 8};
    // The workgroup size (TOTK_MERGE_GROUP, default 256; the shader is built with 64): one invocation merges one word,
    // and ~80,000 workgroups of 64 a frame on the forest porch cost about as much to launch as their work (~1.2 ms of
    // the merges' 2.5). The SPIR-V's LocalSize and its WorkgroupSize constant are set here.
    static uint32_t code[sizeof wb_merge_spv / 4];
    memcpy(code, wb_merge_spv, sizeof code);
    {
        const char *e = cfg_get("TOTK_MERGE_GROUP");
        int g = e ? atoi(e) : 256;
        merge_group = g >= 32 && g <= 1024 && !(g & (g - 1)) ? (uint32_t)g : 256;
        uint32_t n = sizeof code / 4, wgs = 0, first = 0;
        for (uint32_t i = 5; i < n && code[i] >> 16; i += code[i] >> 16) {   // the WorkgroupSize constant's x
            uint32_t op = code[i] & 0xffff;
            if (op == 71 && code[i + 2] == 11 && code[i + 3] == 25) wgs = code[i + 1];
            if (op == 44 && wgs && code[i + 2] == wgs) first = code[i + 3];
        }
        for (uint32_t i = 5; i < n && code[i] >> 16; i += code[i] >> 16) {
            uint32_t op = code[i] & 0xffff;
            if (op == 16 && code[i + 2] == 17) code[i + 3] = merge_group;   // OpExecutionMode LocalSize x
            if (op == 43 && first && code[i + 2] == first) code[i + 3] = merge_group;
        }
    }
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof code,
                                   .pCode = code};
    VkShaderModule m = VK_NULL_HANDLE;
    if (!WM.push || vkCreateDescriptorSetLayout(c->device, &dci, NULL, &WM.dsl) != VK_SUCCESS) goto fail;
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                     .pSetLayouts = &WM.dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    if (vkCreatePipelineLayout(c->device, &li, NULL, &WM.layout) != VK_SUCCESS ||
        vkCreateShaderModule(c->device, &mi, NULL, &m) != VK_SUCCESS)
        goto fail;
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                      .layout = WM.layout};
    VkResult res = vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &ci, NULL, &WM.pipe);
    vkDestroyShaderModule(c->device, m, NULL);
    if (res != VK_SUCCESS) goto fail;
    return WM.ok = 1;
fail:
    rt_log("mem: write-back merge off: its pipeline could not be created");
    return 0;
}

static int merging(void) {
    if (merge_on < 0) {
        merge_on = cfg_bool("TOTK_WB_MERGE");
        rt_log("mem: write-back merge %s (TOTK_WB_MERGE)", merge_on ? "on" : "off");
    }
    if (!merge_on) return 0;
    if (!WM.tried) merge_init(vk_ctx());
    return WM.ok;
}

// A chunk of at least size bytes in chunks[]: the smallest fitting one of the pool, else a new buffer; -1 when the
// allocation fails.
static int chunk_new(uint64_t size, int host) {
    uint64_t want = host ? SNAP_LIST_MIN : SNAP_CHUNK;
    while (want < size) want <<= 1;
    if (nchunks == cchunks) grow((void **)&chunks, &cchunks, sizeof *chunks);
    SnapChunk *ch = &chunks[nchunks];
    int best = -1;
    plat_mutex_lock(&snap_pool_lock);
    for (int i = 0; i < nsnap_pool; i++)
        if (snap_pool[i].host == host && snap_pool[i].size >= want && (best < 0 || snap_pool[i].size < snap_pool[best].size))
            best = i;
    if (best >= 0) {
        *ch = snap_pool[best];
        snap_pool[best] = snap_pool[--nsnap_pool];
        snap_pool_bytes -= ch->size;
    }
    plat_mutex_unlock(&snap_pool_lock);
    if (best < 0) {
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = want,
                                 .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT,   // late_defer copies a snapshot out
                                 .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        // A range list (host) is written by the CPU in order and read by every merge invocation's binary search
        // (wb_merge.comp): in device memory the CPU can map (resizable BAR) when there is some, else in host memory. In
        // host memory each of those reads crossed the bus: the merges took 2.5-3.3 ms of the GPU frame for ~4.5 MiB.
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
                                      .flags = host ? VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                                          VMA_ALLOCATION_CREATE_MAPPED_BIT : 0};
        if (vmaCreateBuffer(vk_ctx()->vma, &bi, &ai, &ch->buf, &ch->alloc, NULL) != VK_SUCCESS) return -1;
        ch->size = want, ch->host = host;
        {   // DEBUG-SNAPWHERE: where the snapshot and list chunks landed
            static int nl;
            VkMemoryPropertyFlags f = 0;
            vmaGetAllocationMemoryProperties(vk_ctx()->vma, ch->alloc, &f);
            if (nl < 12) nl++, rt_log("SNAPWHERE chunk %llu KiB %s: %s%s", (unsigned long long)(want >> 10), host ? "list" : "snapshot",
                                      f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT ? "device-local" : "HOST",
                                      f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT ? " host-visible" : "");
        }
        mrep.snap_allocs++;
    }
    ch->used = 0;
    return nchunks++;
}

// Record time: room in a snapshot chunk for [off, off+size) of r's mirror, once per submission. 0: plain copy instead.
static int snap_add(PoolRec *r, uint64_t off, uint64_t size, int fadd) {
    {   // DEBUG-SNAPLOG: TOTK_SNAPLOG=<frame>: the ranges merged in that frame
        static int64_t sf = -1;
        if (sf < 0) { const char *e = cfg_get("TOTK_SNAPLOG"); sf = e ? atoll(e) : 0; }
        if (sf && __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) == (uint64_t)sf)
            rt_log("SNAPLOG pool %#llx+%#llx off %#llx size %#llx fadd %d fence %llu", (unsigned long long)r->import_lo,
                   (unsigned long long)(r->import_hi - r->import_lo), (unsigned long long)off, (unsigned long long)size, fadd,
                   (unsigned long long)vk_exec_recording_fence());
    }
    for (int i = nsnl; i-- > 0;)
        if (snl[i].r == r && snl[i].off == off && snl[i].size == size) return snl[i].fadd |= fadd, 1;
    int k = -1;
    for (int i = nchunks; i-- > 0;)   // the newest snapshot chunk, if the range fits
        if (chunks[i].used != UINT64_MAX && chunks[i].size - chunks[i].used >= size) { k = i; break; }
    if (k < 0 && (k = chunk_new(size > SNAP_CHUNK ? size : SNAP_CHUNK, 0)) < 0) return 0;
    if (nsnl == csnl) grow((void **)&snl, &csnl, sizeof *snl);
    snl[nsnl++] = (SnapRange){r, off, size, chunks[k].used, k, fadd};
    chunks[k].used += (size + 15) & ~15ull;
    return 1;
}

// Prologue: the snapshot copies of the ranges added since the last prologue.
static void snap_copies(VkCommandBuffer cb) {
    for (int i = snl_copied; i < nsnl; i++) {
        VkBufferCopy cp = {snl[i].off, snl[i].soff, snl[i].size};
        vkCmdCopyBuffer(cb, snl[i].r->mbuf, chunks[snl[i].chunk].buf, 1, &cp);
        mrep.snap_bytes += snl[i].size;
    }
    snl_copied = nsnl;
}

static int snap_cmp(const void *x, const void *y) {
    const SnapRange *a = x, *b = y;
    if (a->r != b->r) return (uintptr_t)a->r < (uintptr_t)b->r ? -1 : 1;
    if (a->chunk != b->chunk) return a->chunk < b->chunk ? -1 : 1;
    return a->off < b->off ? -1 : a->off > b->off;
}

// One dispatch: ranges s[0..n) share a pool and a chunk and lie in [base, base + span) of the pool.
static void merge_dispatch(VkCommandBuffer cb, const SnapRange *s, int n, uint64_t base, uint64_t span, int list,
                           uint64_t list_off, uint32_t total) {
    VkDescriptorBufferInfo bi[4] = {{s->r->mbuf, base, span}, {chunks[s->chunk].buf, 0, chunks[s->chunk].size},
                                    {s->r->buf, base, span}, {chunks[list].buf, list_off, (uint64_t)n * 16}};
    {   // DEBUG-MERGEVRAM: TOTK_MERGE_VRAM_DST=1 (a measurement, the merge's words are lost): the merge writes a VRAM
        // buffer instead of guest memory
        static int on = -1;
        static VkBuffer vb;
        static VmaAllocation va;
        if (on < 0) {
            on = cfg_get("TOTK_MERGE_VRAM_DST") != NULL;
            VkBufferCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = MERGE_SPAN,
                                     .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
            VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
            if (on && vmaCreateBuffer(vk_ctx()->vma, &ci, &ai, &vb, &va, NULL) != VK_SUCCESS) on = 0;
        }
        if (on) bi[2] = (VkDescriptorBufferInfo){vb, 0, span};
    }
    VkWriteDescriptorSet w[4];
    for (int i = 0; i < 4; i++)
        w[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = (uint32_t)i,
                                      .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                      .pBufferInfo = &bi[i]};
    rec_push(WM.push, cb, VK_PIPELINE_BIND_POINT_COMPUTE, WM.layout, 0, 4, w);   // in call order (vk_rec.h)
    uint32_t pc[2] = {(uint32_t)n, total};
    {   // DEBUG-MERGEEMPTY: TOTK_MERGE_EMPTY=1 (a measurement, the merges do nothing): the same dispatches, no invocation works
        static int e = -1;
        if (e < 0) e = cfg_get("TOTK_MERGE_EMPTY") != NULL;
        if (e) pc[1] = 0;
    }
    vkCmdPushConstants(cb, WM.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
    vkCmdDispatch(cb, (total + merge_group - 1) / merge_group, 1, 1);
}

// vk_mem_mirror_end: the merge dispatches of this submission's ranges. Returns the commands recorded; 0 with nsnl
// left as it was when the range list cannot be allocated (the caller copies them plainly).
static int wb_merge_record(VkCommandBuffer cb) {
    VkCtx *c = vk_ctx();
    uint64_t align = c->props.limits.minStorageBufferOffsetAlignment;
    if (align < 16) align = 16;
    SnapRange *s = malloc(sizeof *s * (size_t)nsnl);
    if (!s) rt_fatal("vk: out of memory");
    memcpy(s, snl, sizeof *s * (size_t)nsnl);
    qsort(s, (size_t)nsnl, sizeof *s, snap_cmp);
    // the range lists: at most one 16-byte entry per range, each dispatch's list at an aligned offset
    int list = chunk_new((uint64_t)nsnl * 16 + (uint64_t)nsnl * align, 1);
    if (list < 0) { free(s); return 0; }
    chunks[list].used = UINT64_MAX;   // not a snapshot chunk
    VmaAllocationInfo li;
    vmaGetAllocationInfo(c->vma, chunks[list].alloc, &li);
    uint32_t *lm = li.pMappedData;
    uint64_t lo = 0;
    int rec = 0;
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, WM.pipe);
    for (int i = 0; i < nsnl;) {
        uint64_t base = s[i].off & ~(align - 1), end = s[i].off + s[i].size;
        uint32_t total = 0;
        int j = i;
        lo = (lo + align - 1) & ~(align - 1);
        uint64_t l0 = lo;
        static int per_range = -1;   // TOTK_MERGE_PER_RANGE=1: a dispatch per range (the shader's range search ends at once)
        if (per_range < 0) per_range = cfg_get("TOTK_MERGE_PER_RANGE") && atoi(cfg_get("TOTK_MERGE_PER_RANGE")) > 0;
        for (; j < nsnl && s[j].r == s[i].r && s[j].chunk == s[i].chunk; j++) {
            uint64_t e = s[j].off + s[j].size > end ? s[j].off + s[j].size : end;
            if (j > i && (per_range || e - base > MERGE_SPAN || total + s[j].size / 4 > MERGE_WORDS)) break;
            end = e;
            uint32_t *q = lm + lo / 4;
            q[0] = (uint32_t)((s[j].off - base) / 4), q[1] = (uint32_t)(s[j].soff / 4);
            q[2] = (uint32_t)(s[j].size / 4) | (s[j].fadd ? 0x80000000u : 0);   // wb_merge.comp: add the GPU's change
            q[3] = total;
            total += (uint32_t)(s[j].size / 4);
            lo += 16;
            mrep.merge_bytes += s[j].size;
        }
        merge_dispatch(cb, s + i, j - i, base, end - base, list, l0, total);
        mrep.merge_dispatches++;
        rec += 3;
        i = j;
    }
    vmaFlushAllocation(c->vma, chunks[list].alloc, 0, VK_WHOLE_SIZE);
    for (int i = 0; i < nsnl; i++) snl[i].r->wb_sub = msub;
    free(s);
    return rec;
}

static void snap_free(void *arg) {   // completion thread: the submission is done with its chunks
    SnapFree *f = arg;
    plat_mutex_lock(&snap_pool_lock);
    for (int i = 0; i < f->n; i++) {
        SnapChunk *ch = &f->c[i];
        if (nsnap_pool == SNAP_POOL_N || snap_pool_bytes + ch->size > SNAP_POOL_BYTES) continue;
        snap_pool[nsnap_pool++] = *ch;
        snap_pool_bytes += ch->size;
        ch->buf = VK_NULL_HANDLE;
    }
    plat_mutex_unlock(&snap_pool_lock);
    for (int i = 0; i < f->n; i++)
        if (f->c[i].buf) vmaDestroyBuffer(vk_ctx()->vma, f->c[i].buf, f->c[i].alloc);
    free(f);
}

// The result guard (TOTK_RESULT_GUARD, memory.c): the small ranges a shader of this submission writes are closed to
// guest threads at the end of the recording (vk_mem_mirror_end) until the submission's write-back is in guest memory.
// With a frame in flight OoT read its foliage culling's counts and reset them for the slot's next use
// (f_7002bfd340) before our write-back of the frame that made them: the counts were stale, the reset was lost, and
// the far trees lost their leaves or cast the wrong shadows for a frame now and then (every eighth frame at worst).
// On the console the GPU writes memory the CPU reads; here the read waits for the result instead of seeing an old one.
#define GUARD_MAX (64u << 10)
#define GUARD_N 4096
// DEBUG-RESULTWATCH: GPU writes into a mirror reach guest memory with the submission's write-back; the watch closes
// those guest pages at the recording and the completion of the fence opens them (an earlier CPU access is logged).
typedef struct { uint64_t addr, size; } RwRange;
static RwRange guard_q[GUARD_N];
static int nguard;
static void rw_open_cb(void *arg) { RwRange *q = arg; mem_result_release(q->addr, q->size); free(q); }
// ---- Write-back on demand (TOTK_LAZY_WB, default on for OoT)
// What shaders write into a CPU-visible pool goes back from its VRAM mirror to guest memory at the submission's end:
// in OoT's village 46 MiB of copies and 19 MiB of merges a frame, 2.2 ms of GPU time (66.9 -> 72.3 fps
// with the ranges over 64 KiB left out, TOTK_WB_SKIP_OVER_KB). The game's CPU code touches about 60 of those ~10,000
// pages a frame, the replay itself ~540 (TOTK_RESULT_WATCH_HOLD). So each whole page of a range of LAZY_MIN bytes or
// more has a state (lz_chunk, a byte per page):
//   new        closed for access (mem_lazy_close) and still written back; each further write counts up;
//   hot        an access reached it (lz_touch): open, written back as ever, for good;
//   cold       LZ_K writes with no access: it is no longer written back. Its bytes are in the mirror only, the page
//              stays closed, and its first access brings it and the cold pages around it over with a copy of its own
//              (lz_fetch, a wait of up to one frame of GPU work) and makes them hot.
// A page the CPU writes takes the fault first, so the mirror never gets a stale page: the write watch reports only
// pages that are open. The parts of a range before and after its whole pages are written back as before. A pool that
// is finalized opens its pages without their bytes (lz_pool_gone).
#define LAZY_MIN (64u << 10)
#define LZ_K 240      // writes of a page without an access before it goes cold
#define LZ_HOT 0xff
#define LZ_COLD 0xfe
#define LZ_POOLS 128
#define LZ_SLOTS 16
static uint8_t *lz_chunk[GEN_CHUNKS];
typedef struct { uint64_t lo, hi; VkBuffer mbuf, buf; } LzPool;
static LzPool lz_pool[LZ_POOLS];
static int lz_npool;
static PlatMutex lz_lock = PLAT_MUTEX_INIT;   // lz_pool and the fetch slots; a leaf lock (taken inside a fault)
static struct { VkCommandBuffer cb; VkFence fence; int busy; } lz_slot[LZ_SLOTS];
static VkCommandPool lz_cmdpool;
static uint64_t lz_touches, lz_fetches, lz_fetch_pages, lz_fetch_ns, lz_timeouts, lz_made_cold;   // racy counters
static uint64_t lz_skipped_bytes, lz_eager_bytes;                                                // replay thread

static int lazy_on(void) {
    static int on = -1;
    if (on < 0) {
#ifdef GAME_OOT
        on = rt_knob("LAZY_WB", "1");
#else
        on = rt_knob("LAZY_WB", "0");
#endif
        if (on && (shadow_on || mirror_verify || vk_sync_awb_on())) on = 0;
        if (on) rt_log("vk: write-back on demand: pages the CPU does not touch stay in the mirror (TOTK_LAZY_WB)");
    }
    return on;
}

static uint8_t *lz_byte(uint64_t page, int make) {
    uint64_t ci = page >> GEN_CHUNK_SHIFT;
    if (ci >= GEN_CHUNKS) return NULL;
    uint8_t *c = __atomic_load_n(&lz_chunk[ci], __ATOMIC_ACQUIRE);
    if (!c) {
        if (!make) return NULL;
        uint8_t *n = calloc(GEN_PER_CHUNK, 1), *exp = NULL;
        if (!n) return NULL;
        if (__atomic_compare_exchange_n(&lz_chunk[ci], &exp, n, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) c = n;
        else free(n), c = exp;
    }
    return &c[(page >> GEN_PAGE_SHIFT) & (GEN_PER_CHUNK - 1)];
}

static int lz_cold(uint64_t page_addr) {
    uint64_t ci = page_addr >> GEN_CHUNK_SHIFT;
    const uint8_t *c = ci < GEN_CHUNKS ? __atomic_load_n(&lz_chunk[ci], __ATOMIC_ACQUIRE) : NULL;
    return c && __atomic_load_n(&c[(page_addr >> GEN_PAGE_SHIFT) & (GEN_PER_CHUNK - 1)], __ATOMIC_ACQUIRE) == LZ_COLD;
}

// lz_lock held. One copy mirror -> guest memory of [lo, hi) of pool q, waited for; 0 when the wait ran out.
static int lz_copy(const LzPool *q, uint64_t lo, uint64_t hi) {
    VkCtx *c = vk_ctx();
    if (!lz_cmdpool) {
        VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = c->gfx_family};
        if (vkCreateCommandPool(c->device, &pi, NULL, &lz_cmdpool) != VK_SUCCESS) return 0;
    }
    int s = -1;
    for (int i = 0; i < LZ_SLOTS && s < 0; i++) {
        if (lz_slot[i].busy && vkGetFenceStatus(c->device, lz_slot[i].fence) == VK_SUCCESS) lz_slot[i].busy = 0;
        if (!lz_slot[i].busy) s = i;
    }
    if (s < 0) {   // every slot's copy is still out: wait for the first
        vkWaitForFences(c->device, 1, &lz_slot[0].fence, VK_TRUE, UINT64_MAX);
        lz_slot[0].busy = 0, s = 0;
    }
    if (!lz_slot[s].cb) {
        VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = lz_cmdpool,
                                          .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
        VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (vkAllocateCommandBuffers(c->device, &ai, &lz_slot[s].cb) != VK_SUCCESS ||
            vkCreateFence(c->device, &fi, NULL, &lz_slot[s].fence) != VK_SUCCESS)
            return 0;
    }
    VkCommandBuffer cb = lz_slot[s].cb;
    vkResetFences(c->device, 1, &lz_slot[s].fence);
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(cb, &bi);
    mbarrier(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
             VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferCopy rg = {lo - q->lo, lo - q->lo, hi - lo};
    vkCmdCopyBuffer(cb, q->mbuf, q->buf, 1, &rg);
    mbarrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
             VK_ACCESS_2_HOST_READ_BIT);
    vkEndCommandBuffer(cb);
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
    vk_queue_lock();   // after every part the replay has handed over
    VkResult r = vkQueueSubmit(c->gfx_queue, 1, &si, lz_slot[s].fence);
    vk_queue_unlock();
    if (r != VK_SUCCESS) return 0;
    lz_slot[s].busy = 1;
    // A part held for another queue's signal (vk_exec.c, queues interleaved) blocks the queue until the replay thread
    // submits that signal: when it is the replay thread that waits here, the copy cannot run. So the wait ends after a
    // quarter second and the access reads what guest memory holds, as every early read did before.
    if (vkWaitForFences(c->device, 1, &lz_slot[s].fence, VK_TRUE, 250000000ull) != VK_SUCCESS) return 0;
    lz_slot[s].busy = 0;
    return 1;
}

// mem_lazy_setup's hook: an access reached the closed page at `page` (any thread, inside the fault).
static void lz_touch(uint64_t page, int cold) {
    uint8_t *b = lz_byte(page, 1);
    if (b) __atomic_store_n(b, LZ_HOT, __ATOMIC_RELEASE);
    lz_touches++;
    if (!cold) return;
    uint64_t t0 = plat_now_ns();
    plat_mutex_lock(&lz_lock);
    const LzPool *q = NULL;
    for (int i = 0; i < lz_npool && !q; i++)
        if (page >= lz_pool[i].lo && page < lz_pool[i].hi) q = &lz_pool[i];
    uint64_t lo = page, hi = page + 4096;
    if (q) {
        for (int i = 0; i < 31 && lo >= q->lo + 4096; i++) {
            uint8_t *p = lz_byte(lo - 4096, 0);
            if (!p || __atomic_load_n(p, __ATOMIC_ACQUIRE) != LZ_COLD) break;
            lo -= 4096;
        }
        for (int i = 0; i < 31 && hi + 4096 <= q->hi; i++) {
            uint8_t *p = lz_byte(hi, 0);
            if (!p || __atomic_load_n(p, __ATOMIC_ACQUIRE) != LZ_COLD) break;
            hi += 4096;
        }
        for (uint64_t p = lo; p < hi; p += 4096) {
            uint8_t *s = lz_byte(p, 1);
            if (s) __atomic_store_n(s, LZ_HOT, __ATOMIC_RELEASE);
        }
        if (!lz_copy(q, lo, hi)) {
            lz_timeouts++;
            static int said;
            if (said++ < 8) rt_log("vk: write-back on demand: the copy of %#llx+%#llx did not finish in time; the access reads what "
                                   "guest memory holds", (unsigned long long)lo, (unsigned long long)(hi - lo));
        }
        lz_fetches++, lz_fetch_pages += (hi - lo) >> 12;
    }
    plat_mutex_unlock(&lz_lock);
    if (q) mem_lazy_open(lo, hi - lo);
    lz_fetch_ns += plat_now_ns() - t0;
}

// vk_pool_fini of a mirrored pool: its closed pages open as they are and its states start over.
static void lz_pool_gone(PoolRec *r) {
    if (!lazy_on()) return;
    int had = 0;
    plat_mutex_lock(&lz_lock);
    for (int i = 0; i < lz_npool; i++)
        if (lz_pool[i].lo == r->import_lo && lz_pool[i].mbuf == r->mbuf) { lz_pool[i] = lz_pool[--lz_npool]; had = 1; break; }
    plat_mutex_unlock(&lz_lock);
    for (uint64_t p = r->import_lo & ~0xfffull; p < r->import_hi; p += 4096) {
        uint8_t *b = lz_byte(p, 0);
        if (b && *b) __atomic_store_n(b, 0, __ATOMIC_RELEASE), had = 1;
    }
    if (had) mem_lazy_open(r->import_lo, r->import_hi - r->import_lo);
}

static int wb_register(PoolRec *r, uint64_t off, uint64_t size, int shader);

// gpu_wrote: a shader wrote [off, off + size) of r's mirror. 1: the range was split here (its hot and new pages
// registered for the write-back, its cold pages left out); 0: the caller registers it whole.
static int lazy_split(PoolRec *r, uint64_t off, uint64_t size, int shader, int *plain) {
    if (!lazy_on() || size < LAZY_MIN) return 0;
    uint64_t a = r->import_lo + off, e = a + size, p0 = (a + 4095) & ~4095ull, p1 = e & ~4095ull;
    if (p1 <= p0 || p1 - p0 < LAZY_MIN) return 0;
    static int setup;
    if (!setup) setup = 1, mem_lazy_setup(lz_touch);
    if (p0 > a) *plain |= wb_register(r, off, p0 - a, shader);
    uint64_t run = 0;   // start of the run of pages still written back, 0: none open
    int listed = 0;
    // The new pages are closed run by run first (memory.c mem_lazy_close_run): pre[i] is 1 where page i was closed, 2
    // where it could not be, 0 where it was not new then.
    uint64_t np = (p1 - p0) >> 12;
    uint8_t *pre = NULL;
    for (uint64_t i = 0; i < np;) {
        uint8_t *b = lz_byte(p0 + (i << 12), 1);
        if (!b || __atomic_load_n(b, __ATOMIC_ACQUIRE)) { i++; continue; }
        uint64_t j = i + 1;
        for (; j < np && j - i < 4096; j++) {
            uint8_t *c = lz_byte(p0 + (j << 12), 1);
            if (!c || __atomic_load_n(c, __ATOMIC_ACQUIRE)) break;
        }
        if (!pre && !(pre = calloc(np, 1))) break;
        uint8_t okv[4096];
        mem_lazy_close_run(p0 + (i << 12), (uint32_t)(j - i), okv);
        for (uint64_t q = i; q < j; q++) pre[q] = okv[q - i] ? 1 : 2;
        i = j;
    }
    for (uint64_t p = p0; p < p1; p += 4096) {
        uint8_t *b = lz_byte(p, 1);
        uint8_t s = b ? __atomic_load_n(b, __ATOMIC_ACQUIRE) : LZ_HOT;
        if (b && s != LZ_HOT && s != LZ_COLD) {
            uint8_t n = s;
            uint8_t was = pre ? pre[(p - p0) >> 12] : 0;
            if (!s) n = was ? (was == 1 ? 1 : LZ_HOT)   // watched from now on (or not closable: written back for good)
                            : mem_lazy_close(p, 0) ? 1 : LZ_HOT;
            else if (s + 1 >= LZ_K) {
                if (!listed) {   // the fetch needs the pool's buffers
                    plat_mutex_lock(&lz_lock);
                    int at = -1;
                    for (int i = 0; i < lz_npool && at < 0; i++)
                        if (lz_pool[i].lo == r->import_lo) at = i;
                    if (at < 0 && lz_npool < LZ_POOLS) at = lz_npool++;
                    if (at >= 0) lz_pool[at] = (LzPool){r->import_lo, r->import_hi, r->mbuf, r->buf};
                    plat_mutex_unlock(&lz_lock);
                    listed = at >= 0 ? 1 : -1;
                }
                n = listed > 0 && mem_lazy_close(p, 1) ? LZ_COLD : LZ_HOT;
                lz_made_cold += n == LZ_COLD;
            } else n = (uint8_t)(s + 1);
            // an access may have made it hot meanwhile: that stands
            if (!__atomic_compare_exchange_n(b, &s, n, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) n = s;
            s = n;
        }
        if (s == LZ_COLD) {
            if (run) *plain |= wb_register(r, run - r->import_lo, p - run, shader), lz_eager_bytes += p - run, run = 0;
            lz_skipped_bytes += 4096;
        } else if (!run) run = p;
    }
    free(pre);
    if (run) *plain |= wb_register(r, run - r->import_lo, p1 - run, shader), lz_eager_bytes += p1 - run;
    if (e > p1) *plain |= wb_register(r, p1 - r->import_lo, e - p1, shader);
    return 1;
}

static int gpu_wrote(const VkMemSpan *sp, uint64_t size, int shader) {
    PoolRec *r = sp->pool;
    {   // DEBUG-LISTLOG: GPU writes recorded into the list pool
        uint64_t vk_listlog_addr(void);
        double vk_listlog_ms(void);
        uint64_t la = vk_listlog_addr();
        if (la && r && la >= r->import_lo && la < r->import_hi)
            rt_log("LISTLOG gpuwrote fence %llu off %#llx size %#llx shader %d mirror %d at %.3f",
                   (unsigned long long)vk_exec_recording_fence(), (unsigned long long)sp->offset,
                   (unsigned long long)size, shader, r->mstate == MS_ON && sp->buffer == r->mbuf, vk_listlog_ms());
    }
    {   // DEBUG-WBLOG: TOTK_WBLOG=<addr>: GPU writes recorded over that address (frames 1400-1403)
        static uint64_t wa = 1;
        if (wa == 1) { const char *e = cfg_get("TOTK_WBLOG"); wa = e ? strtoull(e, NULL, 0) : 0; }
        uint64_t fr = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
        if (wa && r && fr >= 1400 && fr <= 1403 && wa >= r->import_lo + sp->offset && wa < r->import_lo + sp->offset + size)
            rt_log("WBLOG frame %llu fence %llu pool %#llx+%#llx off %#llx size %#llx shader %d mirror %d mergeable %d",
                   (unsigned long long)fr, (unsigned long long)vk_exec_recording_fence(), (unsigned long long)r->import_lo,
                   (unsigned long long)(r->import_hi - r->import_lo), (unsigned long long)sp->offset, (unsigned long long)size,
                   shader, r->mstate == MS_ON && sp->buffer == r->mbuf, size <= MERGE_MAX);
    }
    {   // DEBUG-WBWHO: TOTK_WBWHO=<frame>: every range a shader writes in that frame: where, how large, by which compute
        // program, and whether it was recorded after the submission's results point
        static int64_t wf = -1;
        extern uint64_t vk_dbg_cur_cs;
        int vk_sync_results_sent(void);
        if (wf < 0) { const char *e = cfg_get("TOTK_WBWHO"); wf = e ? atoll(e) : 0; }
        if (wf && r && shader && __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) == (uint64_t)wf)
        {
            extern volatile uint64_t vk_spike_sub_t0;
            rt_log("WBWHO fence %llu addr %#llx size %#llx cs %016llx after_point %d mirror %d shader %d at %.3f ms", (unsigned long long)vk_exec_recording_fence(),
                   (unsigned long long)(r->import_lo + sp->offset), (unsigned long long)size, (unsigned long long)vk_dbg_cur_cs,
                   vk_sync_results_sent(), r->mstate == MS_ON && sp->buffer == r->mbuf, shader, (plat_now_ns() - vk_spike_sub_t0) / 1e6);
        }
    }
    if (!r || r->mstate != MS_ON || sp->buffer != r->mbuf || !size) return 0;
    if (shader) r->shader_written = 1;
    if (nawbr) awb_check(r, sp->offset, size);   // async write-back: wait for one that still reads this range
    static uint64_t rw_max = 1;   // DEBUG-RESULTWATCH: TOTK_RESULT_WATCH_MAX=<bytes> (default 4096)
    if (rw_max == 1) { const char *e = cfg_get("TOTK_RESULT_WATCH_MAX"); rw_max = e ? strtoull(e, NULL, 0) : 4096; }
    static uint64_t rw_min = 1;   // DEBUG-RESULTWATCH: TOTK_RESULT_WATCH_MIN=<bytes>: only ranges of at least that size
    if (rw_min == 1) { const char *e = cfg_get("TOTK_RESULT_WATCH_MIN"); rw_min = e ? strtoull(e, NULL, 0) : 0; }
    if (mem_result_watch_on() && size <= rw_max && size >= rw_min) {
        uint64_t f = vk_exec_recording_fence(), a = r->import_lo + sp->offset;
        RwRange *q = malloc(sizeof *q);
        static int hold = -1;   // DEBUG-WATCHHOLD: TOTK_RESULT_WATCH_HOLD=1: a watched page stays closed until the CPU
                                // touches it (not opened when its result is written): which GPU results the CPU reads at all
        if (hold < 0) { const char *e = cfg_get("TOTK_RESULT_WATCH_HOLD"); hold = e && *e == '1'; }
        if (q && f && !hold) *q = (RwRange){a, size}, mem_result_watch(a, size, f), vk_completion_add_result(f, rw_open_cb, q);
        else {
            if (f && hold) mem_result_watch(a, size, f);
            free(q);
        }
    } else if (0) {
    }
    {   // DEBUG-RESFENCE: TOTK_RESFENCE=<frame>: the submissions of that frame that write small CPU-visible ranges by shader
        static int64_t rf = -1;
        if (rf < 0) { const char *e = cfg_get("TOTK_RESFENCE"); rf = e ? atoll(e) : 0; }
        if (rf && shader && size <= GUARD_MAX && __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) == (uint64_t)rf)
            rt_log("RESFENCE fence %llu pool %#llx off %#llx size %#llx", (unsigned long long)vk_exec_recording_fence(),
                   (unsigned long long)r->import_lo, (unsigned long long)sp->offset, (unsigned long long)size);
    }
    // With the results fence only what a shader writes after the submission's results point is guarded: the rest is in
    // guest memory when the game's wait for the frame returns.
    int vk_sync_results_sent(void);
    if (shader && size <= GUARD_MAX && mem_result_guard_on() && (!vk_sync_results_on() || vk_sync_results_sent())) {
        uint64_t f = vk_exec_recording_fence(), a = r->import_lo + sp->offset;
        RwRange *q = malloc(sizeof *q);
        if (q && f && nguard < GUARD_N) *q = (RwRange){a, size}, guard_q[nguard++] = *q, vk_completion_add(f, rw_open_cb, q);
        else free(q);
    }
    {   // DEBUG: TOTK_WB_SKIP_POOLS=<import_lo>[,...]: GPU writes into those pools never go back to guest memory
        static uint64_t skip[8];
        static int ns = -1;
        if (ns < 0) {
            ns = 0;
            for (const char *e = cfg_get("TOTK_WB_SKIP_POOLS"); e && *e && ns < 8;) {
                char *q;
                skip[ns++] = strtoull(e, &q, 0);
                e = *q == ',' ? q + 1 : q;
                if (q == e - 1 && !*q) break;
            }
        }
        for (int i = 0; i < ns; i++) if (skip[i] >= r->import_lo && skip[i] < r->import_hi) return 0;   // the pool holding that address
    }
    {   // DEBUG-WBSKIPBIG: TOTK_WB_SKIP_OVER_KB=<n>: ranges larger than that never go back to guest memory (a test of
        // what the write-back costs; the game's CPU side then reads stale data there)
        static int64_t over = -1;
        if (over < 0) { const char *e = cfg_get("TOTK_WB_SKIP_OVER_KB"); over = e ? atoll(e) << 10 : 0; }
        if (over && (int64_t)size > over) return 0;
    }
    // A binding to the end of its pool (no size) can span hundreds of MiB: those stay copies.
    {   // DEBUG-CPUPAGES: TOTK_CPUPAGES=<frame>: per merged range, how many of its pages the CPU ever wrote
        static int64_t cf = -1;
        if (cf < 0) { const char *e = cfg_get("TOTK_CPUPAGES"); cf = e ? atoll(e) : 0; }
        if (cf && shader && size <= MERGE_MAX && __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) == (uint64_t)cf) {
            uint64_t a = r->import_lo + sp->offset, np = 0, nc = 0;
            for (uint64_t pg = a & ~0xfffull; pg < a + size; pg += 4096, np++) {
                const uint32_t *c = (pg >> GEN_CHUNK_SHIFT) < GEN_CHUNKS ? cpuw_chunk[pg >> GEN_CHUNK_SHIFT] : NULL;
                nc += c && c[(pg >> GEN_PAGE_SHIFT) & (GEN_PER_CHUNK - 1)];
            }
            rt_log("CPUPAGES pool %#llx range +%#llx %llu KiB: %llu of %llu pages CPU-written", (unsigned long long)r->import_lo,
                   (unsigned long long)sp->offset, (unsigned long long)(size >> 10), (unsigned long long)nc, (unsigned long long)np);
        }
    }
    int plain = 0;
    if (shader && lazy_split(r, sp->offset, size, shader, &plain)) return plain;
    return wb_register(r, sp->offset, size, shader);
}

// The write-back of [off, off + size) of r's mirror at the submission's end: a merge of the words the GPU changed
// (0) or a plain copy (1).
static int wb_register(PoolRec *r, uint64_t off, uint64_t size, int shader) {
    if (shader && !(off & 3) && !(size & 3) && size <= MERGE_MAX && merging() &&
        (shader == 2 || cpu_wrote_lately(r->import_lo + off, size)) && snap_add(r, off, size, shader == 2))
        return 0;
    range_add(&wbl, &nwbl, &cwbl, r, off, size);
    return 1;
}
int vk_mem_gpu_wrote(const VkMemSpan *sp, uint64_t size) { return gpu_wrote(sp, size, 0); }
int vk_mem_gpu_wrote_shader(const VkMemSpan *sp, uint64_t size) { return gpu_wrote(sp, size, 1); }
int vk_mem_gpu_wrote_adds(const VkMemSpan *sp, uint64_t size) { return gpu_wrote(sp, size, 2); }

#ifdef GAME_NS2
// Mirror pieces the GPU wrote through the pool table. The replay cannot tell which bytes a shader stores to through a
// loaded address, and the write logs say which table entry it reached only once the submission is done, so a store
// into a mirror never reached guest memory: the CPU read stale words, and the next CPU -> mirror copy of a page or a
// flushed range of the same buffer put stale words back over the GPU's. OoT's scene culling (the compute pass before
// each frame's draws) keeps its lists and counters in small pools the CPU flushes whole every frame; with its results
// reset that way, once the camera moved the lit pass drew a few objects and the sky covered the rest, and it stayed so.
// A piece a log reports written stays hot for GMEM_HOT_AGE submissions, and every submission that binds a write log
// takes a snapshot of each hot piece and writes the words its shaders changed back into guest memory at its end (the
// write-back merge, gpu_wrote), as for a storage buffer a shader writes. The first write of a piece goes back only from
// its next submission on. Pages are not stamped: the mirror already holds what goes back. All pieces
// (TOTK_GMEM_WRITEBACK=1) ran the title movie at a third of its speed (44 MiB of network maps a frame); the default
// takes the pieces of small pools only (GMEM_WB_POOL_MAX). The crashes at the camera turn once blamed on it
// (main+0xc938d0) came with the real-time step's long steps (frame_rate.c STEP_MAX_DEFAULT).
#define GMEM_HOT_MAX 1024
#define GMEM_HOT_AGE 1800                // submissions (about 5 s at OoT's 6 a frame)
// Default (TOTK_GMEM_WRITEBACK unset, 2): only pieces of pools up to this size go back. OoT's scene culling
// writes its draw and instance lists into small pools the CPU also writes and flushes; without the write-back the
// next copy of those pages into the mirror put stale lists back, and the grass and shadows on the house floor
// flickered (the floor under the hero: 19.5 % of its pixels changing strongly frame to frame, 0.03 % with it). The title
// movie's network maps (a 29 MiB pool, 44 MiB written a frame) stay out: going back, they ran the movie at a third
// of its speed.
#define GMEM_WB_POOL_MAX (4ull << 20)
typedef struct { uint64_t lo, hi; uint32_t seen; } GmemHot;
static GmemHot ghot[GMEM_HOT_MAX];
static int nghot;
static PlatMutex ghot_lock = PLAT_MUTEX_INIT;

// A flush range of a mirrored pool goes into the next prologue's copies, except where it covers a piece the GPU wrote
// through the pool table (ghot): there the write watch alone decides (mirror_sync's page loop copies the pages the CPU
// wrote). OoT's scene culling, a compute pass before each frame's draws, writes its draw and instance
// lists through loaded addresses into small double-buffered pools that the CPU flushes whole every frame
// (nvnMemoryPoolFlushMappedRange over the pool, for the few words it wrote). The flush copied the guest bytes, which
// never got the GPU's lists, over the mirror that held them: the next frames drew stale lists, and vegetation and
// shadows flickered. TOTK_FLUSH_GMEM_CUT=0 copies the whole flush range again.
// A page the CPU wrote is copied into the mirror whole, but in a piece the GPU writes through the pool table and that
// does not go back to guest memory (a pool over GMEM_WB_POOL_MAX) the mirror holds GPU words the guest copy never got.
// OoT keeps the mesh-draw records of its foliage there: the culling compute writes their counts, the CPU
// writes other words of the same pages a submission later, and the whole-page copy put the guest's zero counts back
// over the GPU's: most frames drew no grass, some drew it, and stale words drew leaf shards. Such pages keep a host
// copy of what was last copied (pshadow); the next sync copies only the 32-bit words that differ from it, so the
// mirror keeps the GPU's words wherever the CPU did not write. The first sync of a page copies it whole.
// Off by default (TOTK_GMEM_MERGE=1): in OoT the pages the CPU and the GPU both write are in pools of 20-44 KiB, which
// go back to guest memory (GMEM_WB_POOL_MAX), so it never merged a page there.
#define PSH_N (1u << 15)                 // pages with a host copy at most (128 MiB)
static struct { uint64_t page; uint8_t *bytes; } pshadow[PSH_N];
static int psh_full;

static int gmem_merge_page(PoolRec *r, uint64_t x) {   // replay thread; 1: handled, 0: copy the page whole
    static int on = -1;
    if (on < 0) {   // with every piece going back (TOTK_GMEM_WRITEBACK=1) the guest copy has the GPU's words: no merge
        const char *wb = cfg_get("TOTK_GMEM_WRITEBACK");
        on = wb && *wb == '1' ? 0 : rt_knob("GMEM_MERGE", "0");
    }
    static uint64_t whole = 1;   // DEBUG: TOTK_MERGE_POOL=<import_lo>: every page of that pool merges by words
    if (whole == 1) { const char *e = cfg_get("TOTK_MERGE_POOL"); whole = e ? strtoull(e, NULL, 0) : 0; }
    if (!on || (!__atomic_load_n(&nghot, __ATOMIC_ACQUIRE) && !(whole && r->import_lo == whole))) return 0;
    int hit = whole && r->import_lo == whole;
    plat_mutex_lock(&ghot_lock);
    for (int i = 0; i < nghot && !hit; i++) hit = x >= ghot[i].lo && x < ghot[i].hi;
    plat_mutex_unlock(&ghot_lock);
    {   // DEBUG-MERGELOG: the CPU-written pages inside GPU-written pieces, with their pool
        static int nl = -1;
        if (nl < 0) { const char *e = cfg_get("TOTK_MERGELOG"); nl = e && *e == '1' ? 0 : 1000; }
        if (hit && nl < 300) nl++, rt_log("MERGELOG page %#llx pool %#llx..%#llx (%.1f MiB)", (unsigned long long)x,
                                          (unsigned long long)r->import_lo, (unsigned long long)r->import_hi,
                                          (r->import_hi - r->import_lo) / 1048576.0);
    }
    static int small = -1;   // DEBUG-MERGESMALL: TOTK_GMEM_MERGE_SMALL=1: pages of small pools merge by words too
    if (small < 0) small = cfg_get("TOTK_GMEM_MERGE_SMALL") != NULL;
    if (!hit || (!small && r->import_hi - r->import_lo <= GMEM_WB_POOL_MAX)) return 0;
    uint64_t pg = x >> GEN_PAGE_SHIFT, ps = 1ull << GEN_PAGE_SHIFT;
    uint32_t h = (uint32_t)((pg * 0x9E3779B97F4A7C15ull) >> 49) & (PSH_N - 1);
    for (uint32_t k = 0; k < 64; k++, h = (h + 1) & (PSH_N - 1)) {
        if (pshadow[h].page == pg + 1) {
            uint32_t *sh = (uint32_t *)pshadow[h].bytes;
            const uint32_t *g = (const uint32_t *)(uintptr_t)x;
            for (uint32_t w = 0; w < ps / 4;) {
                if (g[w] == sh[w]) { w++; continue; }
                uint32_t e = w;
                while (e < ps / 4 && g[e] != sh[e]) sh[e] = g[e], e++;
                range_add(&pend, &npend, &cpend, r, x + (uint64_t)w * 4 - r->import_lo, (uint64_t)(e - w) * 4);
                mrep.page_bytes += (uint64_t)(e - w) * 4, mrep.merged_words += e - w;
                w = e;
            }
            mrep.merged_pages++;
            return 1;
        }
        if (!pshadow[h].page) {
            uint8_t *c = malloc(ps);
            if (!c) return 0;
            memcpy(c, (const void *)(uintptr_t)x, ps);
            pshadow[h].page = pg + 1, pshadow[h].bytes = c;
            return 0;   // the first copy goes whole
        }
    }
    if (!psh_full++) rt_log("mem: page merge: the table of %u host copies is full; further pages are copied whole", PSH_N);
    return 0;
}

// DEBUG-HOTNOSYNC: TOTK_GMEM_HOT_NOSYNC=1: CPU-written pages inside pieces the GPU writes through the table are not
// copied into the mirror (A/B: whether the CPU's page copies overwrite what the GPU wrote there)
static int gmem_hot_page(uint64_t a) {
    static int on = -1;
    if (on < 0) { const char *e = cfg_get("TOTK_GMEM_HOT_NOSYNC"); on = e && *e == '1'; }
    if (!on || !__atomic_load_n(&nghot, __ATOMIC_ACQUIRE)) return 0;
    int hit = 0;
    plat_mutex_lock(&ghot_lock);
    for (int i = 0; i < nghot && !hit; i++) hit = a >= ghot[i].lo && a < ghot[i].hi;
    plat_mutex_unlock(&ghot_lock);
    return hit;
}

static void flush_range_add_(PoolRec *r, uint64_t a, uint64_t b);
// A flush names a range the CPU may have written; pages whose last stamp is a GPU write the mirror holds (page_owned)
// and that the CPU has not written since are left out: guest memory there is the write-back, which with frames in
// flight can be older than what later GPU work already left in the mirror (foliage records flickered).
// TOTK_FLUSH_OWNED_CUT=0 copies flush ranges whole. gen_lock held (mirror_sync).
static void flush_range_add(PoolRec *r, uint64_t a, uint64_t b) {
    static int on = -1;
    if (on < 0) on = rt_knob("FLUSH_OWNED_CUT", "1");
    if (!on) return flush_range_add_(r, a, b);
    uint64_t run = a, x = a & ~0xfffull;
    for (; x < b; x += 1ull << GEN_PAGE_SHIFT) {
        const uint32_t *ch = gen_chunk[x >> GEN_CHUNK_SHIFT];
        uint32_t st = ch ? ch[(x >> GEN_PAGE_SHIFT) & (GEN_PER_CHUNK - 1)] : 0;
        if (!(st && page_owned(x, st)) && !lz_cold(x)) continue;
        uint64_t p0 = x > a ? x : a, p1 = x + (1ull << GEN_PAGE_SHIFT) < b ? x + (1ull << GEN_PAGE_SHIFT) : b;
        if (p0 > run) flush_range_add_(r, run, p0);
        run = p1;
        mrep.owned_pages++;
    }
    if (run < b) flush_range_add_(r, run, b);
}

static void flush_range_add_(PoolRec *r, uint64_t a, uint64_t b) {
    static int cut = -1;
    if (cut < 0) cut = rt_knob("FLUSH_GMEM_CUT", "1");
    {   // DEBUG: TOTK_MERGE_POOL: that pool's flush ranges are left to the page loop (word merge)
        static uint64_t whole = 1;
        if (whole == 1) { const char *e = cfg_get("TOTK_MERGE_POOL"); whole = e ? strtoull(e, NULL, 0) : 0; }
        if (whole && r->import_lo == whole) return;
    }
    uint64_t lo = r->import_lo;
    if (cut && __atomic_load_n(&nghot, __ATOMIC_ACQUIRE)) {
        uint64_t cuts[64][2];
        int nc = 0;
        plat_mutex_lock(&ghot_lock);
        for (int i = 0; i < nghot && nc < 64; i++)
            if (ghot[i].lo < b && ghot[i].hi > a) cuts[nc][0] = ghot[i].lo, cuts[nc][1] = ghot[i].hi, nc++;
        plat_mutex_unlock(&ghot_lock);
        if (nc) {
            for (int i = 1; i < nc; i++)   // by start
                for (int j = i; j > 0 && cuts[j - 1][0] > cuts[j][0]; j--) {
                    uint64_t t0 = cuts[j][0], t1 = cuts[j][1];
                    cuts[j][0] = cuts[j - 1][0], cuts[j][1] = cuts[j - 1][1], cuts[j - 1][0] = t0, cuts[j - 1][1] = t1;
                }
            uint64_t at = a;
            for (int i = 0; i < nc && at < b; i++) {
                if (cuts[i][0] > at) range_add(&pend, &npend, &cpend, r, at - lo, (cuts[i][0] < b ? cuts[i][0] : b) - at);
                if (cuts[i][1] > at) at = cuts[i][1];
            }
            if (at < b) range_add(&pend, &npend, &cpend, r, at - lo, b - at);
            mrep.flush_cut++;
            return;
        }
    }
    range_add(&pend, &npend, &cpend, r, a - lo, b - a);
}

// Small pools that both the CPU and the GPU write through the table give up their mirror: OoT's mesh-draw
// records for its foliage (pools of 20-44 KiB, double-buffered per frame) take words from the CPU and from the culling
// compute, and in the mirror, whose pages the CPU's writes reach whole at the next submission and the GPU's go back
// at the end of their own, the task and mesh shaders read records half one frame's and half another's: the grass drew
// in a few frames, leaf shards in others. Read through the import buffer, as on the console, each side sees the other's
// words as they are written; at 44 KiB the bus traffic is nothing. A log naming a mirror piece of such a pool queues it
// (completion thread); the next submission takes the pool off the mirrored list (vk_mem_mirror_begin): bindings and
// the pool table use the import buffer from then on, which holds the GPU's words since the write-back
// (GMEM_WB_POOL_MAX) ran at the end of each submission. The mirror's buffer stays allocated (a few KiB each).
// TOTK_GMEM_DEMOTE=0 keeps the mirrors.
#define DEMOTE_MAX_DEFAULT (1ull << 20)
static uint64_t demote_max(void) {   // TOTK_GMEM_DEMOTE_MAX=<KiB> (A/B)
    static uint64_t m;
    if (!m) { const char *e = cfg_get("TOTK_GMEM_DEMOTE_MAX"); m = e && *e ? strtoull(e, NULL, 0) << 10 : DEMOTE_MAX_DEFAULT; }
    return m;
}
static uint64_t demote_q[256];
static int ndemote;
static PlatMutex demote_lock = PLAT_MUTEX_INIT;

static void gmem_demote_pending(void) {   // replay thread
    if (!__atomic_load_n(&ndemote, __ATOMIC_ACQUIRE)) return;
    uint64_t q[256];
    plat_mutex_lock(&demote_lock);
    int n = ndemote;
    memcpy(q, demote_q, sizeof q[0] * (size_t)n);
    ndemote = 0;
    plat_mutex_unlock(&demote_lock);
    int done = 0;
    plat_mutex_lock(&pool_lock);
    for (int i = 0; i < n; i++) {
        const pidx_ent *e = pidx_find(&pool_index, q[i], 1);
        PoolRec *r = e ? e->buf : NULL;
        if (!r || r->kind != POOL_IMPORT || r->mstate != MS_ON || r->import_hi - r->import_lo > demote_max()) continue;
        mirror_forget(r);
        r->mstate = MS_REFUSED;
        r->mwhy = MR_DEMOTED;
        done++;
        static int said;
        if (said++ < 32)
            rt_log("mem: pool %#llx+%#llx (%.0f KiB) gives up its mirror: the GPU writes it through the pool table and the CPU "
                   "writes it too", (unsigned long long)r->lo, (unsigned long long)r->size, (r->import_hi - r->import_lo) / 1024.0);
    }
    plat_mutex_unlock(&pool_lock);
    if (done) __atomic_add_fetch(&gmem_mgen, 1, __ATOMIC_RELEASE);   // the pool table points at the import buffers
}

void vk_mem_gmem_written(uint64_t lo, uint64_t hi) {   // completion thread
    {
        static int dm = -1;
        // Off by default since the mirrors stopped taking back what the GPU wrote (vk_gpu_written_mirror): read live from
        // the import buffer, with a frame in flight these records raced the CPU's next frame (the far trees lost their
        // leaves every eighth frame, 2026-11-07); in the mirror they come out right. TOTK_GMEM_DEMOTE=1: the old rule.
        if (dm < 0) dm = rt_knob("GMEM_DEMOTE", "0");
        if (dm) {
            plat_mutex_lock(&demote_lock);
            int k = 0;
            while (k < ndemote && demote_q[k] != lo) k++;
            if (k == ndemote && ndemote < 256) demote_q[ndemote++] = lo;
            plat_mutex_unlock(&demote_lock);
        }
    }
    vk_usnap_gpuw(lo, hi - lo);   // DEBUG-USNAP-GPUW
    uint32_t now = __atomic_load_n(&msub, __ATOMIC_RELAXED);
    int added = 0;
    plat_mutex_lock(&ghot_lock);
    int i = 0;
    while (i < nghot && (ghot[i].lo != lo || ghot[i].hi != hi)) i++;
    if (i == nghot && nghot < GMEM_HOT_MAX) ghot[nghot++] = (GmemHot){lo, hi, now}, added = 1;
    else if (i < nghot) ghot[i].seen = now;
    plat_mutex_unlock(&ghot_lock);
    if (added) {
        static int said;
        if (said++ < ((rt_trace & RT_TRACE_GFX) ? 256 : 8))
            rt_log("mem: global memory writes into the mirror of %#llx..%#llx go back to guest memory from now on",
                   (unsigned long long)lo, (unsigned long long)hi);
    }
}

void vk_mem_gmem_writeback(void) {   // replay thread, the submission being recorded binds a write log
    static uint32_t done_sub = ~0u;
    static int on = -1;
    if (on < 0) {   // 0 off, 1 every piece, 2 (default) pieces of pools up to GMEM_WB_POOL_MAX
        const char *e = cfg_get("TOTK_GMEM_WRITEBACK");
        on = e && *e ? atoi(e) : 2;
        rt_log("mem: global memory writes into mirrors go back to guest memory: %s",
               on == 1 ? "every piece (TOTK_GMEM_WRITEBACK=1)" : on ? "pieces of pools up to 4 MiB" : "off (TOTK_GMEM_WRITEBACK=0)");
    }
    static int64_t from = -1;   // DEBUG-GMEM-FROM: TOTK_GMEM_WRITEBACK_FROM=<frame>: only from that frame on (skips the title movie)
    if (from < 0) { const char *e = cfg_get("TOTK_GMEM_WRITEBACK_FROM"); from = e && *e ? atoll(e) : 0; }
    if (from && __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) < (uint64_t)from) return;
    if (!on || !vk_mirror_on || done_sub == msub || !__atomic_load_n(&nghot, __ATOMIC_ACQUIRE)) return;
    done_sub = msub;
    GmemHot v[GMEM_HOT_MAX];
    int n = 0;
    plat_mutex_lock(&ghot_lock);
    for (int i = 0; i < nghot;)
        if (msub - ghot[i].seen > GMEM_HOT_AGE) ghot[i] = ghot[--nghot];   // not written for a while
        else v[n++] = ghot[i++];
    plat_mutex_unlock(&ghot_lock);
    for (int i = 0; i < n; i++)
        for (uint64_t a = v[i].lo, b; a < v[i].hi; a = b) {   // ranges the merge takes (at most MERGE_MAX each)
            b = v[i].hi - a > MERGE_MAX ? a + MERGE_MAX : v[i].hi;
            VkMemSpan sp;
            if (!vk_mem_resolve(a, b - a, &sp) || !sp.pool) continue;
            const PoolRec *r = sp.pool;
            if (on == 2 && r->import_hi - r->import_lo > GMEM_WB_POOL_MAX) break;   // a large pool: the whole piece stays
            vk_mem_pick(&sp);
            gpu_wrote(&sp, b - a, 1);
        }
}
#else
static void flush_range_add_(PoolRec *r, uint64_t a, uint64_t b) { range_add(&pend, &npend, &cpend, r, a - r->import_lo, b - a); }
static void flush_range_add(PoolRec *r, uint64_t a, uint64_t b) { flush_range_add_(r, a, b); }
static int gmem_hot_page(uint64_t a) { (void)a; return 0; }
static int gmem_merge_page(PoolRec *r, uint64_t x) { (void)r, (void)x; return 0; }
static void gmem_demote_pending(void) {}
#endif

// Adds [a, b) to l, n sorted ranges that do not touch; returns the new count. Above cap ranges (l holds cap + 1) the two
// nearest merge, so a full list copies the fewest bytes that were not flushed; cap 1 keeps one span over all flushes.
static int flush_add(FlushRange *l, int n, uint64_t a, uint64_t b, int cap) {
    int i = 0;
    while (i < n && l[i].hi < a) i++;
    int j = i;
    for (; j < n && l[j].lo <= b; j++) {
        if (l[j].lo < a) a = l[j].lo;
        if (l[j].hi > b) b = l[j].hi;
    }
    memmove(l + i + 1, l + j, (size_t)(n - j) * sizeof *l);
    l[i] = (FlushRange){a, b};
    n += 1 - (j - i);
    if (n > cap) {
        int k = 0;
        for (int x = 1; x + 1 < n; x++)
            if (l[x + 1].lo - l[x].hi < l[k + 1].lo - l[k].hi) k = x;
        l[k].hi = l[k + 1].hi;
        memmove(l + k + 1, l + k + 2, (size_t)(n - k - 2) * sizeof *l);
        n--;
    }
    return n;
}

int vk_mem_flush_add_probe(void) {
    FlushRange l[5];
    int n = 0;
    n = flush_add(l, n, 0x8000, 0x9000, 4);
    n = flush_add(l, n, 0x1000, 0x2000, 4);
    n = flush_add(l, n, 0x4000, 0x5000, 4);
    if (n != 3 || l[0].lo != 0x1000 || l[1].lo != 0x4000 || l[2].hi != 0x9000) return 1;   // kept apart, sorted
    n = flush_add(l, n, 0x2000, 0x3000, 4);
    if (n != 3 || l[0].hi != 0x3000) return 2;                                             // touching ranges join
    n = flush_add(l, n, 0x800, 0x8800, 4);
    if (n != 1 || l[0].lo != 0x800 || l[0].hi != 0x9000) return 3;                         // one covers several
    n = 0;
    n = flush_add(l, n, 0x10000, 0x11000, 2);
    n = flush_add(l, n, 0x40000, 0x41000, 2);
    n = flush_add(l, n, 0x12000, 0x13000, 2);                                              // over the cap: nearest merge
    if (n != 2 || l[0].lo != 0x10000 || l[0].hi != 0x13000 || l[1].lo != 0x40000) return 4;
    n = 0;
    n = flush_add(l, n, 0x10000, 0x11000, 1);
    n = flush_add(l, n, 0x40000, 0x41000, 1);
    if (n != 1 || l[0].lo != 0x10000 || l[0].hi != 0x41000) return 5;                      // TOTK_FLUSH_SPAN=1
    return 0;
}

uint64_t vk_mem_mirror_uploaded(void) {
    plat_mutex_lock(&mirror_stat_lock);
    uint64_t n = macc.upload_bytes;
    plat_mutex_unlock(&mirror_stat_lock);
    return n;
}

void nvn_pool_flushed(const pool_t *p, uint64_t off, uint64_t size) {
    if (!vk_mirror_on || !size || !p->mem) return;
    plat_inc64(&m_flush_calls);
    plat_add64(&m_flush_bytes, (int64_t)size);
    plat_mutex_lock_shared(&pool_lock);
    const pidx_ent *e = pidx_find(&pool_index, (uintptr_t)p->mem, 0);
    PoolRec *r = e && e->key == p ? e->buf : NULL;
    if (r && r->mstate == MS_ON) {
        uint64_t a = ((uintptr_t)p->mem + off) & ~0xfffull, b = ((uintptr_t)p->mem + off + size + 0xfff) & ~0xfffull;
        plat_mutex_lock(&flush_lock);
        __atomic_store_n(&r->fl_n, flush_add(r->fl, r->fl_n, a, b, flush_cap), __ATOMIC_RELEASE);
        r->fl_calls++, r->fl_sum += size;
        plat_mutex_unlock(&flush_lock);
    }
    plat_mutex_unlock_shared(&pool_lock);
}

// gen_lock exclusive: freezes up to FREEZE_PER_SUB candidate regions still idle. The query after the
// protection collects the writes made before it took effect.
static void freeze_pass(void) {
    for (int done = 0; nfcand && done < FREEZE_PER_SUB;) {
        uint64_t j = fcand[--nfcand], a = j << REG_SHIFT;
        if (ww_ep - reg_last[j] < (uint32_t)freeze_idle || mem_frozen(a)) continue;
        done++;
        if (!mem_freeze(a)) { n_frz_refused++; if (sec_on) sec_count(SEC_FZ_REFUSED, 1); continue; }
        if (sec_on) sec_count(SEC_FZ_OK, 1);
        uint64_t sec1 = sec_begin();
        query_stamp(a, a + (1ull << REG_SHIFT), ++gen_now);
        sec_end(SEC_FZ_QUERY, sec1);
    }
}

void vk_mem_mirror_begin(void) {
    uint64_t sec0 = sec_begin();
    awb_prune();   // async write-back: drop the ranges of the write-backs done
    sec_end(SEC_MB_AWB, sec0);
    if (!vk_mirror_on) {   // import mode: the texture checks' snapshots still skip frozen regions
        if (!freeze_idle) return;
        plat_mutex_lock(&gen_lock);
        ww_ep++;
        freeze_pass();
        plat_mutex_unlock(&gen_lock);
        return;
    }
    uint64_t t0 = qpc(), sec1 = sec_begin();
    gmem_demote_pending();
    hot_demote_pending();
    sec_end(SEC_MB_DEMOTE, sec1);
    nsnl = snl_copied = nchunks = 0;   // the previous submission handed its chunks to snap_free
    uint32_t prev = msub;
    msub = msub + 1 ? msub + 1 : 1;
    __atomic_add_fetch(&mirror_gen, 1, __ATOMIC_RELEASE);
    mepoch = vk_texture_epoch();
    uint64_t q = 0, run0 = 0, run1 = 0;
    int have = 0;
    sec1 = sec_begin();
    plat_mutex_lock_shared(&pool_lock);
    plat_mutex_lock(&gen_lock);
    sec_end(SEC_MB_LOCKS, sec1);
    ww_ep++;
    uint64_t b0 = ww_bytes;
    sec1 = sec_begin();
    for (int i = 0; i < nm; i++) {
        const PoolRec *r = mlist[i];
        if (r->acc_sub != prev) continue;
        uint64_t a = r->import_lo >> SNAP_SHIFT, b = (r->import_hi - 1) >> SNAP_SHIFT;
        if (have && a <= run1 + 1 + (uint64_t)mirror_gap) {
            if (b > run1) run1 = b;
            continue;
        }
        if (have) q += snap_regions(run0, run1, mepoch);
        run0 = a, run1 = b, have = 1;
    }
    if (have) q += snap_regions(run0, run1, mepoch);
    sec_end(SEC_MB_SNAP, sec1);
    mrep.presnap_bytes += ww_bytes - b0;
    if (freeze_idle) {
        sec1 = sec_begin();
        freeze_pass();
        sec_end(SEC_MB_FREEZE, sec1);
    }
    plat_mutex_unlock(&gen_lock);
    plat_mutex_unlock_shared(&pool_lock);
    mrep.subs++;
    mrep.presnap_queries += q;
    mrep.presnap_t += qpc() - t0;
    sec_end(SEC_MB_TOTAL, sec0);
}

int vk_mem_mirror_pending(void) { return npend || snl_copied < nsnl; }

// The write-back this submission has collected so far: bytes of plain copies and of merges.
void vk_mem_wb_pending(uint64_t *plain, uint64_t *merge) {
    uint64_t p = 0, m = 0;
    for (int i = 0; i < nwbl; i++) p += wbl[i].size;
    for (int i = 0; i < nsnl; i++) m += snl[i].size;
    *plain = p, *merge = m;
}

// ---- Submit-time snapshots of the CPU -> mirror copies (TOTK_MIRROR_SNAP=0: off)
// NVN starts the GPU within microseconds of nvnQueueSubmitCommands, so the console's GPU reads a pool's bytes as the
// CPU left them at the submit. A prologue's copies used to read guest memory when the GPU ran them, 15-50 ms later, by
// which time the game's other threads had written the next frame into some of those pages (per-frame rings, constant
// blocks): the mirror held a mix of two frames, and the lighting was off (haze, too bright; with copy guards, which
// hold such writes until the copy ran, the picture matched the console, docs/oot-notes.md). Now the prologue copies the
// bytes into a host ring when it is recorded, right before the part's submit, several threads at a time, and the GPU
// copies them from there into the mirror. Ring space is reclaimed when the prologue timeline (vk_sync.c pro_sem)
// passes the prologue that used it; a prologue larger than half the ring copies from guest memory as before.
// The ring is host-visible memory the driver keeps resident whole, written or not (every page of it is in the
// working set from its allocation on: the 512 MiB block in the process's memory map, 2026-11-14), while a frame
// takes one to five megabytes of it and the largest burst seen, a save's first frames, 246. So its size follows the
// machine: SNAP_RING_BIG with 20 GiB of memory and more, SNAP_RING_SMALL below, where a prologue over 64 MiB then
// copies from guest memory as before the ring.
#define SNAP_RING_BIG (512ull << 20)
#define SNAP_RING_SMALL (128ull << 20)
static uint64_t snap_ring = SNAP_RING_BIG;
#define SNAP_RING snap_ring
#define SNAP_PIECE (512u << 10)      // bytes a worker claims at a time
#define SNAP_THREADS 6               // workers, the caller not counted
#define SNAP_MIN_MT (2u << 20)       // below this the caller copies alone
struct SnapJob { uint8_t *dst; const uint8_t *src; uint64_t n; };
static struct {
    int tried, on;
    VkBuffer buf;
    VmaAllocation alloc;
    uint8_t *map;
    uint64_t head, tail;             // byte positions, monotonic: in use [tail, head)
    struct { uint64_t end, value; } *ent;   // per prologue: its end position and the timeline value that frees it
    int nent, cent, untagged;        // untagged: entries at the back waiting for vk_mem_prologue_submitted
    uint64_t (*done)(void);
    int (*wait)(uint64_t);
    // the parallel copy
    PlatMutex lock;
    PlatCond work, fin;
    int threads, active;
    uint64_t gen;
    struct SnapJob *job;
    uint64_t *job_at;                // first byte (over all jobs) of each job, for the piece lookup
    int njob, cjob;
    volatile int32_t next;           // next piece to claim
    int32_t pieces;
    // statistics (TOTK_TRACE=perf)
    uint64_t bytes, copies, fallbacks, waits, ns;
} msn = {.lock = PLAT_MUTEX_INIT, .work = PLAT_COND_INIT, .fin = PLAT_COND_INIT};

void vk_mem_prologue_timeline(uint64_t (*done)(void), int (*wait)(uint64_t)) { msn.done = done, msn.wait = wait; }

static void snap_piece(int32_t k) {
    uint64_t at = (uint64_t)k * SNAP_PIECE, end = at + SNAP_PIECE;
    int lo = 0, hi = msn.njob - 1;   // the job holding byte `at`: the last with job_at <= at
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (msn.job_at[mid] <= at) lo = mid;
        else hi = mid - 1;
    }
    for (int j = lo; j < msn.njob && msn.job_at[j] < end; j++) {
        uint64_t a = msn.job_at[j], b = a + msn.job[j].n;
        uint64_t x = a > at ? a : at, y = b < end ? b : end;
        if (x < y) memcpy(msn.job[j].dst + (x - a), msn.job[j].src + (x - a), (size_t)(y - x));
    }
}

static void snap_claim(void) {
    for (;;) {
        int32_t k = plat_xadd32(&msn.next, 1);
        if (k >= msn.pieces) return;
        snap_piece(k);
    }
}

static void snap_worker(void *arg) {
    (void)arg;
    uint64_t seen = 0;
    plat_mutex_lock(&msn.lock);
    for (;;) {
        while (msn.gen == seen) plat_cond_wait(&msn.work, &msn.lock);
        seen = msn.gen;
        plat_mutex_unlock(&msn.lock);
        snap_claim();
        plat_mutex_lock(&msn.lock);
        if (!--msn.active) plat_cond_broadcast(&msn.fin);
    }
}

static int snap_init(void) {
    if (msn.tried) return msn.on;
    msn.tried = 1;
    // Default: on only with frames in flight (TOTK_PRESENT_SYNC other than c / off): with the present waiting for its
    // own frame the GPU runs right after the submit anyway, and the copies would cost ~1.3 ms of CPU a frame for nothing.
    // c<j> with j > 0 overlaps too (the frame's last j submissions run during the next frame's CPU work). Pools a shader
    // writes stay late (prologue below, snap_late).
    const char *ps = cfg_get("TOTK_PRESENT_SYNC");
#ifdef GAME_OOT
    if (!ps || !*ps) ps = "c";    // nvn_api_obj.c: OoT's default
#endif
    int overlap = (ps && *ps && strcmp(ps, "off") && (*ps != 'c' || atoi(ps + 1) > 0)) || vk_sync_results_on();
    if (!rt_knob("MIRROR_SNAP", overlap ? "1" : "0") || !msn.done || !msn.wait) return 0;
    {   // TOTK_SNAP_RING_MB overrides the size (64 to 2048)
        const char *e = cfg_get("TOTK_SNAP_RING_MB");
        float ru = 0, ram = 0, rp = 0;
        plat_mem_info(&ru, &ram, &rp);
        if (e && atoi(e) >= 64 && atoi(e) <= 2048) snap_ring = (uint64_t)atoi(e) << 20;
        else if (ram > 0 && ram < 20480) snap_ring = SNAP_RING_SMALL;
    }
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = SNAP_RING,
                             .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                           VMA_ALLOCATION_CREATE_MAPPED_BIT};
    VmaAllocationInfo info;
    if (vmaCreateBuffer(vk_ctx()->vma, &bi, &ai, &msn.buf, &msn.alloc, &info) != VK_SUCCESS || !info.pMappedData) {
        rt_log("mem: no host ring for submit-time mirror copies; the GPU reads guest memory when it runs them");
        return 0;
    }
    msn.map = info.pMappedData;
    int cpus = (int)plat_cpu_usable() - 2;
    int n = cpus < 1 ? 0 : cpus > SNAP_THREADS ? SNAP_THREADS : cpus;
    for (int i = 0; i < n; i++)
        if (plat_thread_spawn(snap_worker, NULL, "mirror snap", 0)) msn.threads++;
    rt_log("mem: mirror copies take the guest bytes at the submit (host ring %llu MiB, %d copy threads)",
           (unsigned long long)(SNAP_RING >> 20), msn.threads);
    msn.on = 1;
    return 1;
}

// Frees ring entries the GPU is done with, then finds room for need bytes (no wrap inside them), waiting for the
// oldest prologues when the ring is full. UINT64_MAX: no room without waiting on this submission's own copies.
static uint64_t snap_alloc(uint64_t need) {
    for (int waited = 0;; waited = 1) {
        uint64_t done = msn.done();
        int k = 0;
        while (k < msn.nent - msn.untagged && msn.ent[k].value <= done) msn.tail = msn.ent[k++].end;
        if (k) memmove(msn.ent, msn.ent + k, sizeof *msn.ent * (size_t)(msn.nent - k)), msn.nent -= k;
        if (!msn.nent) msn.tail = msn.head;
        uint64_t pos = msn.head, off = pos % SNAP_RING;
        if (off + need > SNAP_RING) pos += SNAP_RING - off;
        if (pos + need - msn.tail <= SNAP_RING) return pos;
        if (msn.nent - msn.untagged <= 0) return UINT64_MAX;
        if (!waited) msn.waits++;
        msn.wait(msn.ent[0].value);
    }
}

// Records the ranges (sorted and merged as copy_ranges does) as copies from the ring, after copying the guest bytes
// into it. -1: they do not fit (the caller copies from guest memory), else the commands recorded.
static int copy_ranges_snap(VkCommandBuffer cb, MRange *a, int n, uint64_t *bytes, uint64_t *copies) {
    qsort(a, (size_t)n, sizeof *a, range_cmp);
    uint64_t total = 0;
    for (int i = 0; i < n;) {
        PoolRec *r = a[i].r;
        uint64_t lim = r->import_hi - r->import_lo, end = 0;
        int any = 0;
        for (; i < n && a[i].r == r; i++) {
            uint64_t lo = a[i].off, hi = a[i].off + a[i].size;
            if (hi > lim) hi = lim;
            if (lo >= hi) continue;
            if (any && lo <= end) {
                if (hi > end) total += hi - end, end = hi;
                continue;
            }
            total += hi - lo, end = hi, any = 1;
        }
    }
    if (!total) return 0;
    if (total > SNAP_RING / 2) return -1;
    uint64_t pos = snap_alloc(total);
    if (pos == UINT64_MAX) return -1;
    uint64_t t0 = qpc();
    uint64_t base = pos % SNAP_RING, at = 0;
    msn.njob = 0;
    int rec = 0;
    for (int i = 0; i < n;) {
        PoolRec *r = a[i].r;
        uint64_t lim = r->import_hi - r->import_lo;
        int k = 0;
        for (; i < n && a[i].r == r; i++) {
            uint64_t lo = a[i].off, hi = a[i].off + a[i].size;
            if (hi > lim) hi = lim;
            if (lo >= hi) continue;
            if (k && lo <= mregions[k - 1].dstOffset + mregions[k - 1].size) {
                uint64_t e = mregions[k - 1].dstOffset + mregions[k - 1].size;
                if (hi > e) mregions[k - 1].size += hi - e;
                continue;
            }
            if (k == cregions) grow((void **)&mregions, &cregions, sizeof *mregions);
            mregions[k++] = (VkBufferCopy){0, lo, hi - lo};
        }
        if (!k) continue;
        for (int j = 0; j < k; j++) {   // ring places in order, one job per region
            mregions[j].srcOffset = base + at;
            if (msn.njob == msn.cjob) {
                int c = msn.cjob;
                grow((void **)&msn.job, &msn.cjob, sizeof *msn.job);
                msn.job_at = realloc(msn.job_at, sizeof *msn.job_at * (size_t)msn.cjob);
                if (!msn.job_at) rt_fatal("mem: out of memory (mirror snapshots, %d jobs)", c);
            }
            msn.job_at[msn.njob] = at;
            msn.job[msn.njob++] = (struct SnapJob){msn.map + base + at,
                                                 (const uint8_t *)(uintptr_t)(r->import_lo + mregions[j].dstOffset),
                                                 mregions[j].size};
            at += mregions[j].size;
            *bytes += mregions[j].size;
            r->up_bytes += mregions[j].size, r->hot_bytes += mregions[j].size;
            uplog(r, mregions[j].dstOffset, mregions[j].size, "snap");
        }
        vkCmdCopyBuffer(cb, msn.buf, r->mbuf, (uint32_t)k, mregions);
        rec++;
        *copies += (uint64_t)k;
    }
    // the guest bytes, now: in pieces over the workers, the caller among them
    msn.pieces = (int32_t)((at + SNAP_PIECE - 1) / SNAP_PIECE);
    msn.next = 0;
    if (msn.threads && at >= SNAP_MIN_MT) {
        plat_mutex_lock(&msn.lock);
        msn.active = msn.threads;
        msn.gen++;
        plat_cond_broadcast(&msn.work);
        plat_mutex_unlock(&msn.lock);
        snap_claim();
        plat_mutex_lock(&msn.lock);
        while (msn.active) plat_cond_wait(&msn.fin, &msn.lock);
        plat_mutex_unlock(&msn.lock);
    } else {
        snap_claim();
    }
    msn.head = pos + at;
    if (msn.nent == msn.cent) grow((void **)&msn.ent, &msn.cent, sizeof *msn.ent);
    msn.ent[msn.nent].end = msn.head, msn.ent[msn.nent].value = UINT64_MAX, msn.nent++, msn.untagged++;
    msn.bytes += at, msn.copies++;
    msn.ns += qpc() - t0;
    return rec;
}

// TOTK_TRACE=perf, with the mirror statistics: what the submit-time copies cost.
static void snap_log(uint64_t frame, double frames) {
    if (!msn.on || frames <= 0) return;
    rt_log("mem: mirror snapshots frame %llu: %.1f MiB in %.1f prologues per frame, %.3f ms of copying per frame, "
           "%llu ring waits, %llu prologues from guest memory", (unsigned long long)frame, msn.bytes / 1048576.0 / frames,
           msn.copies / frames, qpc_ms(msn.ns) / frames, (unsigned long long)msn.waits, (unsigned long long)msn.fallbacks);
    msn.bytes = msn.copies = msn.ns = msn.waits = msn.fallbacks = 0;
}

// Copy guards (memory.c): the guest ranges the last prologue's CPU -> mirror copies read, guarded once it is submitted
// (vk_mem_prologue_submitted) until the GPU ran it. TOTK_COPY_GUARD=0: off.
static struct { uint64_t lo, n; } *cgq;
static int ncgq, ccgq;
static int copy_guard_on(void) {
    static int on = -1;
    if (on < 0) on = rt_knob("COPY_GUARD", "0");   // off: protecting ~90 MiB a frame cost ~70 ms of CPU
    return on;
}

void vk_mem_prologue_submitted(uint64_t value) {
    for (int i = msn.nent - msn.untagged; i < msn.nent; i++) msn.ent[i].value = value;   // the ring space it copied from
    msn.untagged = 0;
    for (int i = 0; i < ncgq; i++) mem_copy_guard(cgq[i].lo, cgq[i].n, value);
    ncgq = 0;
    mem_copy_guard_sweep();
}

void vk_mem_mirror_prologue(VkCommandBuffer cb) {
    if (!npend && snl_copied == nsnl) return;
    for (int i = 0; nawbr && i < npend; i++) awb_check(pend[i].r, pend[i].off, pend[i].size);   // async write-back
    if (copy_guard_on())
        for (int i = 0; i < npend; i++) {
            if (ncgq == ccgq) grow((void **)&cgq, &ccgq, sizeof *cgq);
            cgq[ncgq].lo = pend[i].r->import_lo + pend[i].off, cgq[ncgq].n = pend[i].size, ncgq++;
        }
    mbarrier(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
             VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
    {   // Pools a shader writes (results, counters, argument records) are copied from guest memory when the GPU runs
        // the copy, as the console's GPU reads them; the others as of the submit (the ring above). OoT
        // writes its foliage culling's initial records after the submit that uses them and relies on the GPU running
        // it later: as of the submit the far trees lost their leaves every eighth frame, while CPU-only data (the sun
        // shadow setup among it) has to be as of the submit, or about one frame in 300 drew another frame's shadows.
        // TOTK_SNAP_LATE=0: every pool as of the submit. TOTK_SNAP_SKIP_POOL=<import_lo> (DEBUG): that pool late too.
        static uint64_t skipp = 1;
        static int late_on = -1;
        if (skipp == 1) { const char *e = cfg_get("TOTK_SNAP_SKIP_POOL"); skipp = e ? strtoull(e, NULL, 0) : 0; }
        if (late_on < 0) late_on = rt_knob("SNAP_LATE", "1");
        if ((skipp || late_on) && npend && snap_init()) {
            MRange *late = malloc(sizeof *late * (size_t)npend);
            int nl = 0, k = 0;
            for (int i = 0; late && i < npend; i++)
                if (pend[i].r->import_lo == skipp || (late_on && pend[i].r->shader_written)) late[nl++] = pend[i];
                else pend[k++] = pend[i];
            if (late) npend = k;
            if (nl) copy_ranges(cb, late, nl, 1, &mrep.upload_bytes, &mrep.upload_copies);
            free(late);
        }
    }
    if (!(npend && !copy_guard_on() && snap_init() &&
          copy_ranges_snap(cb, pend, npend, &mrep.upload_bytes, &mrep.upload_copies) >= 0)) {
        if (npend && msn.on) msn.fallbacks++;
        copy_ranges(cb, pend, npend, 1, &mrep.upload_bytes, &mrep.upload_copies);
    }
    if (snl_copied < nsnl) {   // the snapshots read what the uploads above wrote
        if (npend) mbarrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                            VK_ACCESS_2_TRANSFER_READ_BIT);
        snap_copies(cb);
    }
    mbarrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
             VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    npend = 0;
    mrep.prologues++;
}

// ---- verify mode
typedef struct { PoolRec *r; uint64_t off, len, dst; uint32_t mgen; } VSample;
typedef struct { uint32_t sub; int n; VkBuffer buf; VmaAllocation alloc; const uint8_t *map; VSample s[VERIFY_SAMPLES]; } VJob;
static int ver_i;
static uint64_t ver_off;

// The CPU wrote the page since the mirror's sync that mgen records: stamped later, or its write-watch bit is set.
static int page_dirty(uint64_t a, uint32_t mgen) {
    uint32_t *s = gen_slot(a, 0);
    if (s && __atomic_load_n(s, __ATOMIC_RELAXED) > mgen) return 1;
    uint64_t p[1];
    return mem_written_peek(a, 1ull << GEN_PAGE_SHIFT, p, 1) != 0;   // UINT64_MAX (not watched) counts as written
}

static void verify_cb(void *arg) {   // completion thread
    VJob *j = arg;
    static volatile int32_t logged;
    uint64_t pages = 0, skipped = 0, bad = 0;
    for (int i = 0; i < j->n; i++) {
        const VSample *s = &j->s[i];
        PoolRec *r = s->r;
        uint32_t wb = __atomic_load_n(&r->wb_sub, __ATOMIC_RELAXED);
        if (wb != j->sub && wb - j->sub < 0x80000000u) {   // a later submission wrote back into it
            skipped += s->len >> GEN_PAGE_SHIFT;
            continue;
        }
        for (uint64_t o = 0; o < s->len; o += 1ull << GEN_PAGE_SHIFT) {
            uint64_t a = r->import_lo + s->off + o;
            const uint8_t *m = j->map + s->dst + o, *g = (const uint8_t *)(uintptr_t)a;
            pages++;
            if (!memcmp(m, g, 1ull << GEN_PAGE_SHIFT)) continue;
            if (page_dirty(a, s->mgen)) { skipped++; continue; }
            bad++;
            if (plat_inc32(&logged) <= VERIFY_LOG) {
                int k = 0, e = (1 << GEN_PAGE_SHIFT) - 1;
                while (k < e && m[k] == g[k]) k++;
                while (e > k && m[e] == g[e]) e--;
                char f[160];
                flag_names(r->flags, f, sizeof f);
                rt_log("mem: verify mismatch: pool %#llx+%#llx (%s) offset %#llx: bytes %#x..%#x of the page differ, "
                       "mirror %02x guest %02x at the first (submission %u)", (unsigned long long)r->lo,
                       (unsigned long long)r->size, f, (unsigned long long)(a - r->lo), k, e, m[k], g[k], j->sub);
            }
        }
    }
    plat_add64(&v_pages, (int64_t)pages);
    plat_add64(&v_skipped, (int64_t)skipped);
    plat_add64(&v_mismatch, (int64_t)bad);
    vmaDestroyBuffer(vk_ctx()->vma, j->buf, j->alloc);
    free(j);
}

static int verify_record(VkCommandBuffer cb, uint64_t fence) {
    VJob *j = calloc(1, sizeof *j);
    if (!j) rt_fatal("vk: out of memory");
    uint64_t left = VERIFY_BYTES, dst = 0;
    plat_mutex_lock_shared(&pool_lock);
    for (int tries = 0; nm && left && j->n < VERIFY_SAMPLES && tries <= nm; tries++) {
        if (ver_i >= nm) ver_i = 0, ver_off = 0;
        PoolRec *r = mlist[ver_i];
        uint64_t size = r->import_hi - r->import_lo;
        if (ver_off >= size) { ver_i++, ver_off = 0; continue; }
        uint64_t len = size - ver_off < left ? size - ver_off : left;
        j->s[j->n++] = (VSample){r, ver_off, len, dst, r->mgen};
        dst += len, left -= len, ver_off += len;
        if (ver_off >= size) ver_i++, ver_off = 0;
    }
    plat_mutex_unlock_shared(&pool_lock);
    if (!j->n) { free(j); return 0; }
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = dst,
                             .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
    VmaAllocationInfo info;
    if (vmaCreateBuffer(vk_ctx()->vma, &bi, &ai, &j->buf, &j->alloc, &info) != VK_SUCCESS) { free(j); return 0; }
    j->map = info.pMappedData;
    j->sub = msub;
    mbarrier(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
             VK_ACCESS_2_TRANSFER_READ_BIT);
    for (int i = 0; i < j->n; i++) {
        VkBufferCopy c = {j->s[i].off, j->s[i].dst, j->s[i].len};
        vkCmdCopyBuffer(cb, j->s[i].r->mbuf, j->buf, 1, &c);
    }
    mbarrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
             VK_ACCESS_2_HOST_READ_BIT);
    vk_completion_add(fence, verify_cb, j);
    return j->n + 2;
}



// Async write-back (vk_sync.c): the mirror ranges pending write-backs read, with the value of their submission. A part
// that writes into one (an upload in its prologue, a GPU write) waits for that write-back (awb_check). Only ranges of
// TOTK_ASYNC_WB_MIN_KB (default 256) or more go there: small ones, like the status words every submission fills, are
// written back at once, so the next submissions need not wait.
typedef struct { PoolRec *r; uint64_t lo, hi, fence; } AwbRange;
static AwbRange *awbr;
static int cawbr;
static uint64_t awb_done_seen;   // vk_sync_awb_done as of the last prune
static void awb_note(PoolRec *r, uint64_t off, uint64_t size, uint64_t fence) {
    if (nawbr == cawbr) grow((void **)&awbr, &cawbr, sizeof *awbr);
    awbr[nawbr++] = (AwbRange){r, off, off + size, fence};
}
static uint64_t awb_waits;
static void awb_check(PoolRec *r, uint64_t off, uint64_t size) {
    for (int i = 0; i < nawbr; i++)
        if (awbr[i].r == r && off < awbr[i].hi && awbr[i].lo < off + size && awbr[i].fence > awb_done_seen)
            vk_sync_awb_need(awbr[i].fence), awb_waits++;
}
static void awb_prune(void) {
    if (!nawbr) return;
    awb_done_seen = vk_sync_awb_done();
    int k = 0;
    for (int i = 0; i < nawbr; i++)
        if (awbr[i].fence > awb_done_seen) awbr[k++] = awbr[i];
    nawbr = k;
}
static uint64_t awb_min(void) {
    static uint64_t m;
    if (!m) { const char *e = cfg_get("TOTK_ASYNC_WB_MIN_KB"); m = (uint64_t)(e && atoi(e) > 0 ? atoi(e) : 256) << 10; }
    return m;
}
// The merges and copies of the current snl / wbl into cb (lead: the barrier after the submission's work; on the compute
// queue the semaphore wait orders it). Returns the commands recorded.
static int wb_emit(VkCommandBuffer cb, int lead, uint64_t fence) {
    int rec = 0;
    if (nsnl) {
        if (lead)   // the merge reads the mirror the submission's shaders wrote and writes the imported buffers
            mbarrier(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
        VkPassMark *mm = vk_pass_mark_begin(cb, VK_MARK_MERGE, fence);
        int m = wb_merge_record(cb);
        vk_pass_mark_end(cb, mm);
        if (!m) {   // no range list: copy them plainly
            for (int i = 0; i < nsnl; i++) range_add(&wbl, &nwbl, &cwbl, snl[i].r, snl[i].off, snl[i].size);
        }
        rec += m + 1;
        mbarrier(cb, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_HOST_BIT | VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                 VK_ACCESS_2_HOST_READ_BIT | VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    }
    if (nwbl) {
        if (lead)
            mbarrier(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                     VK_ACCESS_2_TRANSFER_READ_BIT);
        for (int i = 0; i < nwbl; i++) wbl[i].r->wb_sub = msub;
        mrep.wb_ranges += (uint64_t)nwbl;
        VkPassMark *cm = vk_pass_mark_begin(cb, VK_MARK_WBCOPY, fence);
        rec += copy_ranges(cb, wbl, nwbl, 0, &mrep.wb_bytes, &mrep.wb_copies) + 2;
        vk_pass_mark_end(cb, cm);
        mbarrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_HOST_BIT | VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                 VK_ACCESS_2_HOST_READ_BIT | VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
        nwbl = 0;
    }
    return rec;
}

// ---- late sums (with the results fence, vk_sync.c "Results before the frame's end")
// What a shader adds with float atomics after a submission's results point (OoT's auto exposure: the frame's
// luminance summed into a word the CPU reads and resets once a frame, see "write-back merge") cannot go back at the
// submission's end: the game's wait for the frame has returned by then, its next frame runs, and the sum lands before or
// after that frame's read as the GPU happens to finish. The read then sees the luminance of no frame, of one or of two,
// and the exposure pulses. Such ranges are instead copied out, the mirror's words and their snapshot, into a
// host-visible ring at the submission's end, and a game thread adds the difference into guest memory when its wait for
// a LATER value returns (vk_mem_late_apply from nvnSyncWait and the present): every submission before that value has
// finished altogether by then, so the moment depends on the game's own order of waits and not on the GPU's timing, and
// each frame's read gets exactly one frame's sum, one frame late. Only float sums go this way: a difference is right
// whatever guest memory and the mirror hold meanwhile, where a plain word that stayed back would be overwritten in
// the mirror by the next upload of its page. TOTK_LATE_SUMS=0 leaves them to the merge at the submission's end.
#define LATE_RING (4u << 20)
#define LATE_MAX (64u << 10)
#define LATE_N 256
typedef struct { uint64_t addr, size, fence; uint32_t post, pre; } LateSum;   // post, pre: ring offsets
static struct {
    VkBuffer buf;
    VmaAllocation alloc;
    uint8_t *map;
    uint32_t head;
    LateSum q[LATE_N];
    int n, failed;
    uint64_t deferred, applied, words, stores;   // since the last log; stores: words taken as stored (late_shadow)
} LS;
static PlatMutex late_lock = PLAT_MUTEX_INIT;

static int late_on(void) {
    static int on = -1;
    if (on < 0) on = rt_knob("LATE_SUMS", "1");
    return on;
}

static int late_init(void) {
    if (LS.buf) return 1;
    if (LS.failed) return 0;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = LATE_RING,
                             .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
    VmaAllocationInfo info;
    if (vmaCreateBuffer(vk_ctx()->vma, &bi, &ai, &LS.buf, &LS.alloc, &info) != VK_SUCCESS || !info.pMappedData) {
        LS.failed = 1, LS.buf = VK_NULL_HANDLE;
        return 0;
    }
    LS.map = info.pMappedData;
    return 1;
}

// late_lock held. Room for n bytes in the ring that no pending entry reads; UINT32_MAX when there is none.
static uint32_t late_room(uint32_t n) {
    uint32_t at = LS.head + n <= LATE_RING ? LS.head : 0;
    for (int i = 0; i < LS.n; i++) {
        const LateSum *e = &LS.q[i];
        uint32_t sz = (uint32_t)e->size;
        if ((e->post < at + n && at < e->post + sz) || (e->pre < at + n && at < e->pre + sz)) return UINT32_MAX;
    }
    LS.head = at + n;
    return at;
}

// vk_mem_mirror_end, before the merge: the float-sum ranges written after the results point leave snl for the ring.
static int late_defer(VkCommandBuffer cb, uint64_t fence) {
    if (!nsnl || mirror_verify || !vk_sync_results_on() || !vk_sync_results_sent() || !late_on()) return 0;
    int any = 0;
    for (int i = 0; i < nsnl; i++) any |= snl[i].fadd && snl[i].size <= LATE_MAX && !(snl[i].size & 3);
    if (!any || !late_init()) return 0;
    int rec = 0, k = 0;
    plat_mutex_lock(&late_lock);
    for (int i = 0; i < nsnl; i++) {
        SnapRange *s = &snl[i];
        uint32_t at = UINT32_MAX;
        if (s->fadd && s->size <= LATE_MAX && !(s->size & 3) && LS.n < LATE_N) at = late_room((uint32_t)s->size * 2);
        if (at == UINT32_MAX) {
            snl[k++] = *s;
            continue;
        }
        if (!rec)   // the copies read the mirror the submission's shaders wrote
            mbarrier(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                     VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferCopy post = {.srcOffset = s->off, .dstOffset = at, .size = s->size};
        VkBufferCopy pre = {.srcOffset = s->soff, .dstOffset = at + s->size, .size = s->size};
        vkCmdCopyBuffer(cb, s->r->mbuf, LS.buf, 1, &post);
        vkCmdCopyBuffer(cb, chunks[s->chunk].buf, LS.buf, 1, &pre);
        LS.q[LS.n++] = (LateSum){s->r->import_lo + s->off, s->size, fence, at, at + (uint32_t)s->size};
        LS.deferred++;
        rec += 2;
        for (int g = 0; g < nguard; g++)   // not a result the game has to wait for any more
            if (guard_q[g].addr < s->r->import_lo + s->off + s->size && s->r->import_lo + s->off < guard_q[g].addr + guard_q[g].size)
                guard_q[g].size = 0;
    }
    plat_mutex_unlock(&late_lock);
    nsnl = k;
    if (rec)
        mbarrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
                 VK_ACCESS_2_HOST_READ_BIT);
    return rec ? rec + 2 : 0;
}

// What vk_mem_late_apply wrote into each word of the ranges it serves (late_lock). The range a shader adds into with
// float atomics also holds words it simply stores: beside the luminance sums of OoT's auto exposure sit
// about 1,800 words of results the GPU writes every frame and the game never resets. For such a word the rule of a
// sum (guest += mirror - snapshot whenever guest differs from the snapshot) is wrong, and it differed every frame:
// this function's own write dirties the page, the next submission uploads it into the mirror a frame before the
// newest value has gone back, and the snapshot is then a frame older than guest memory. Each word became
// g(n+1) = g(n) - g(n-1) + v(n+1), which swings around v with a period of six frames and never decays: 0.34 was read
// as -2.2 to 2.9, and the scene came out a third darker for one frame every 40 to 150 (the performance video at 1:22,
// 2026-11-12). A word that still holds what was written here last was not written by the game since, so it is
// taken as stored: the GPU's word goes back as it is. A sum the game resets differs from it and is added as before.
// TOTK_LATE_STORES=0: every word as a sum.
#define LATE_SHADOWS 16
typedef struct { uint64_t addr, size, used; uint32_t *last; uint8_t *have; } LateShadow;
static LateShadow late_sh[LATE_SHADOWS];
static uint64_t late_sh_clock;
static int late_stores = -1;
static LateShadow *late_shadow(uint64_t addr, uint64_t size) {
    if (late_stores < 0) late_stores = rt_knob("LATE_STORES", "1");
    if (!late_stores) return NULL;
    LateShadow *lru = &late_sh[0];
    for (int i = 0; i < LATE_SHADOWS; i++) {
        LateShadow *x = &late_sh[i];
        if (x->addr == addr && x->size == size && x->last) { x->used = ++late_sh_clock; return x; }
        if (x->used < lru->used) lru = x;
    }
    free(lru->last), free(lru->have);
    *lru = (LateShadow){addr, size, ++late_sh_clock, malloc(size), calloc(size / 4, 1)};
    if (!lru->last || !lru->have) {
        free(lru->last), free(lru->have);
        *lru = (LateShadow){0};
        return NULL;
    }
    return lru;
}

// A game thread, its wait for `before` done: the sums of every submission before that value (wb_merge.comp's rule for
// a float sum, word by word; late_shadow for the words that are no sums).
void vk_mem_late_apply(uint64_t before) {
    if (!__atomic_load_n(&LS.n, __ATOMIC_RELAXED)) return;
    uint64_t done = vk_sync_fence_done();
    plat_mutex_lock(&late_lock);
    int k = 0, inval = 0;
    for (int i = 0; i < LS.n; i++) {
        LateSum *e = &LS.q[i];
        if (e->fence >= before || e->fence > done) {
            LS.q[k++] = *e;
            continue;
        }
        if (!inval) inval = 1, vmaInvalidateAllocation(vk_ctx()->vma, LS.alloc, 0, VK_WHOLE_SIZE);
        const uint32_t *v = (const uint32_t *)(LS.map + e->post), *p = (const uint32_t *)(LS.map + e->pre);
        uint32_t *g = (uint32_t *)(uintptr_t)e->addr;
        LateShadow *sh = late_shadow(e->addr, e->size);
        for (uint64_t w = 0; w < e->size / 4; w++) {
            if (v[w] == p[w]) continue;
            uint32_t cur = g[w], o = v[w];
            // The word still holds what this function wrote last: the game has not written it since, so it is no sum
            // being read and reset, and the GPU's word stands as it is (late_shadow).
            int ours = sh && late_stores && sh->have[w] && cur == sh->last[w];
            if (cur != p[w] && !ours) {
                float fc, fv, fp;
                memcpy(&fc, &cur, 4), memcpy(&fv, &v[w], 4), memcpy(&fp, &p[w], 4);
                fc += fv - fp;
                memcpy(&o, &fc, 4);
            } else if (cur != p[w]) {
                LS.stores++;
            }
            g[w] = o;
            if (sh) sh->last[w] = o, sh->have[w] = 1;
            LS.words++;
        }
        LS.applied++;
    }
    __atomic_store_n(&LS.n, k, __ATOMIC_RELAXED);
    plat_mutex_unlock(&late_lock);
}

// The write-back collected so far, in the middle of a submission (vk_exec.c wb_early, at the start of a part: every
// range's snapshot copy is recorded by then, in the prologues of the parts before). The ranges leave the lists; one a
// shader writes again later in the submission joins them anew, with a new snapshot, and goes back at the end as before.
// Returns the commands recorded, 0 when there was nothing to do.
int vk_mem_mirror_flush(VkCommandBuffer cb, uint64_t fence) {
    if (!vk_mirror_on || mirror_verify || vk_sync_awb_on() || (!nsnl && !nwbl) || snl_copied < nsnl) return 0;
    int rec = wb_emit(cb, 1, fence);
    nsnl = snl_copied = 0;   // the chunks stay until the fence (vk_mem_mirror_end hands them all to snap_free)
    mrep.early_flushes++;
    return rec;
}

int vk_mem_mirror_end(VkCommandBuffer cb, uint64_t fence) {
    if (!vk_mirror_on) {
        for (int i = 0; i < nguard; i++) mem_result_guard(guard_q[i].addr, guard_q[i].size, fence);
        nguard = 0;
        return 0;
    }
    int rec = late_defer(cb, fence);   // before the guard: what it takes is no result of this submission any more
    for (int i = 0; i < nguard; i++) mem_result_guard(guard_q[i].addr, guard_q[i].size, fence);   // the result guard
    nguard = 0;
    // async write-back: the big ranges go to the compute queue, the rest stays here
    static SnapRange *bs;
    static MRange *bw;
    static int cbs, cbw;
    int nbs = 0, nbw = 0;
    if ((nsnl || nwbl) && !mirror_verify && vk_sync_awb_on()) {
        int k = 0;
        for (int i = 0; i < nsnl; i++)
            if (snl[i].size >= awb_min()) {
                if (nbs == cbs) grow((void **)&bs, &cbs, sizeof *bs);
                bs[nbs++] = snl[i];
            } else snl[k++] = snl[i];
        nsnl = k;
        k = 0;
        for (int i = 0; i < nwbl; i++)
            if (wbl[i].size >= awb_min()) {
                if (nbw == cbw) grow((void **)&bw, &cbw, sizeof *bw);
                bw[nbw++] = wbl[i];
            } else wbl[k++] = wbl[i];
        nwbl = k;
    }
    rec += wb_emit(cb, 1, fence);
    if (nbs || nbw) {
        VkCommandBuffer acb = vk_sync_awb_begin(fence);
        SnapRange *ss = snl;
        int sn = nsnl;
        snl = bs, nsnl = nbs;
        for (int i = 0; i < nbw; i++) range_add(&wbl, &nwbl, &cwbl, bw[i].r, bw[i].off, bw[i].size);
        for (int i = 0; i < nbs; i++) awb_note(bs[i].r, bs[i].off, bs[i].size, fence);
        for (int i = 0; i < nbw; i++) awb_note(bw[i].r, bw[i].off, bw[i].size, fence);
        wb_emit(acb, 0, fence);
        bs = snl, nbs = 0;   // wb_merge_record may have grown it
        snl = ss, nsnl = sn;
    }
    if (nchunks) {   // the last part's prologue still records snapshot copies into them; back to the pool at the fence
        SnapFree *f = malloc(sizeof *f + sizeof(SnapChunk) * (size_t)nchunks);
        if (!f) rt_fatal("vk: out of memory");
        f->n = nchunks;
        memcpy(f->c, chunks, sizeof(SnapChunk) * (size_t)nchunks);
        vk_completion_add(fence, snap_free, f);
    }
    if (mirror_verify) rec += verify_record(cb, fence);
    plat_mutex_lock(&mirror_stat_lock);
    uint64_t *d = (uint64_t *)&macc, *s = (uint64_t *)&mrep;
    for (size_t i = 0; i < sizeof macc / sizeof(uint64_t); i++) d[i] += s[i];
    plat_mutex_unlock(&mirror_stat_lock);
    memset(&mrep, 0, sizeof mrep);
    return rec;
}

// One summary line: with TOTK_TRACE=perf every 60 frames, else at frames 600 and 1800.
static void mirror_log(uint64_t frame) {
    if (lazy_on() && (rt_trace & RT_TRACE_PERF)) {   // racy reads of counters other threads add to
        static uint64_t last_f, last_skip, last_eager, last_touch, last_fetch, last_ns;
        double fr = frame > last_f ? (double)(frame - last_f) : 1.0;
        rt_log("perf: frame %llu: write-back on demand: per frame %.1f MiB left in the mirror, %.1f MiB of large ranges written "
               "back, %.1f pages first touched, %.2f fetches (%.3f ms); in all: %llu pages gone cold, %llu fetches of %llu "
               "pages, %llu timed out", (unsigned long long)frame, (double)(lz_skipped_bytes - last_skip) / 1048576.0 / fr,
               (double)(lz_eager_bytes - last_eager) / 1048576.0 / fr, (double)(lz_touches - last_touch) / fr,
               (double)(lz_fetches - last_fetch) / fr, (double)(lz_fetch_ns - last_ns) / 1e6 / fr,
               (unsigned long long)lz_made_cold, (unsigned long long)lz_fetches, (unsigned long long)lz_fetch_pages,
               (unsigned long long)lz_timeouts);
        last_f = frame, last_skip = lz_skipped_bytes, last_eager = lz_eager_bytes, last_touch = lz_touches;
        last_fetch = lz_fetches, last_ns = lz_fetch_ns;
    }
    static uint64_t last, ww_last;
    plat_mutex_lock(&gen_lock);
    uint64_t wc = ww_calls, wb = ww_bytes, wt = ww_ticks, dc = ww_dirty_calls, dt = ww_dirty_ticks, wp = ww_pages;
    ww_pages = 0;
    uint64_t nr = n_frz_refused;
    ww_calls = ww_bytes = ww_ticks = ww_dirty_calls = ww_dirty_ticks = n_frz_refused = 0;
    plat_mutex_unlock(&gen_lock);
    double wf = frame > ww_last ? (double)(frame - ww_last) : 1.0;
    ww_last = frame;
    if (rt_trace & RT_TRACE_PERF) snap_log(frame, wf);   // the snapshot counters are the replay's; read without a lock
    uint64_t fz, ff, ft, fo;
    mem_freeze_stats(&fz, &ff, &ft, &fo);
    if (rt_trace & RT_TRACE_PERF)
        rt_log("mem: write-watch frame %llu: per frame, every consumer: %.1f queries over %.1f MiB, %.3f ms (%.1f found "
               "%.0f written pages, %.3f ms); frozen %.0f MiB; since the last line: freezes %llu, thaws by a write %llu, by a file read "
               "or mapping change %llu, refused %llu",
               (unsigned long long)frame, wc / wf, wb / wf / 1048576.0, qpc_ms(wt) / wf, dc / wf, wp / wf, qpc_ms(dt) / wf,
               fz * 2.0, (unsigned long long)ff, (unsigned long long)ft, (unsigned long long)fo, (unsigned long long)nr);
    if (!vk_mirror_on) return;
    plat_mutex_lock(&mirror_stat_lock);
    MirrorAcc a = macc;
    memset(&macc, 0, sizeof macc);
    plat_mutex_unlock(&mirror_stat_lock);
    double f = frame > last ? (double)(frame - last) : 1.0, M = 1048576.0;
    last = frame;
    uint64_t fc = (uint64_t)plat_xchg64(&m_flush_calls, 0), fb = (uint64_t)plat_xchg64(&m_flush_bytes, 0);
    char ver[160] = "";
    if (mirror_verify)
        snprintf(ver, sizeof ver, "; verify: pages compared %lld, skipped %lld (written since), mismatches %lld",
                 (long long)plat_xchg64(&v_pages, 0), (long long)plat_xchg64(&v_skipped, 0),
                 (long long)plat_xchg64(&v_mismatch, 0));
    rt_log("mem: mirror frame %llu: pools %lld, %.1f MiB device-local; stay imported: budget %lld (%.1f MiB), untracked "
           "%lld, overlapping %lld, failed %lld; per frame: submits %.2f, syncs %.1f, upload %.3f MiB in %.1f ranges, "
           "first use %.2f pools %.3f MiB, write-back %.3f MiB in %.1f ranges, merge %.3f MiB in %.1f dispatches "
           "(snapshots %.3f MiB, new buffers %.2f), flush calls %.1f (%.3f MiB), queries "
           "%.1f pre-snap (%.1f MiB) + %.1f sync, prologues %.2f, tracking cpu %.3f ms (pre-snap %.3f, syncs %.3f)%s",
           (unsigned long long)frame, (long long)m_live_n, m_live_bytes / M, (long long)m_refused_n[MR_BUDGET],
           m_refused_bytes[MR_BUDGET] / M, (long long)m_refused_n[MR_UNTRACKED], (long long)m_refused_n[MR_OVERLAP],
           (long long)m_refused_n[MR_ALLOC], a.subs / f, a.syncs / f, a.upload_bytes / f / M, a.upload_copies / f,
           a.first_n / f, a.first_bytes / f / M, a.wb_bytes / f / M, a.wb_copies / f, a.merge_bytes / f / M,
           a.merge_dispatches / f, a.snap_bytes / f / M, a.snap_allocs / f, fc / f, fb / f / M,
           a.presnap_queries / f, a.presnap_bytes / f / M, a.sync_queries / f, a.prologues / f, qpc_ms(a.presnap_t + a.sync_t) / f,
           qpc_ms(a.presnap_t) / f, qpc_ms(a.sync_t) / f, ver);
    host_bind_log(frame, f);
    {
        uint64_t gr, gf, gw, gns;
        mem_copy_guard_stats(&gr, &gf, &gw, &gns);
        if (gr) rt_log("mem: copy guards frame %llu: %.1f ranges, %.2f write faults, %.2f waits (%.3f ms) per frame",
                       (unsigned long long)frame, gr / f, gf / f, gw / f, gns / 1e6 / f);
    }
    if (rt_trace & RT_TRACE_PERF) {   // the pools behind the CPU -> mirror upload, largest first
        PoolRec *top[10] = {0};
        plat_mutex_lock_shared(&pool_lock);
        for (int i = 0; i < nm; i++) {
            PoolRec *r = mlist[i];
            if (!r->up_bytes) continue;
            for (int j = 0; j < 10; j++)
                if (!top[j] || r->up_bytes > top[j]->up_bytes) {
                    memmove(&top[j + 1], &top[j], sizeof top[0] * (size_t)(9 - j));
                    top[j] = r;
                    break;
                }
        }
        char line[1400];
        size_t o = 0;
        for (int j = 0; j < 10 && top[j] && o < sizeof line - 128; j++) {
            char fl[96];
            flag_names(top[j]->flags, fl, sizeof fl);
            double sz = (double)(top[j]->import_hi - top[j]->import_lo);
            o += (size_t)snprintf(line + o, sizeof line - o, " %#llx %.1fMiB [%s] %.2fMiB/frame (%.0f%%)",
                                  (unsigned long long)top[j]->lo, sz / M, fl, top[j]->up_bytes / f / M,
                                  sz ? 100.0 * (double)top[j]->up_bytes / f / sz : 0.0);
        }
        for (int i = 0; i < nm; i++) mlist[i]->up_bytes = 0;
        plat_mutex_unlock_shared(&pool_lock);
        rt_log("mem: mirror frame %llu: uploads by pool:%s", (unsigned long long)frame, o ? line : " none");
    }
    rt_log("mem: mirror frame %llu: upload sources per frame, overlaps counted twice: written pages %.3f MiB; flush ranges "
           "%.2f, %.3f MiB (%.2f cut around the GPU's pieces), from %.1f flushes of %.3f MiB; merged pages %.1f (%.0f words); "
           "pages the mirror wrote, left out %.3f MiB",
           (unsigned long long)frame, a.page_bytes / f / M, a.flush_n / f, a.flush_bytes / f / M, a.flush_cut / f,
           a.flush_calls / f, a.flush_sum / f / M, a.merged_pages / f, a.merged_words / f, a.owned_pages * 4096.0 / f / M);
}

// With TOTK_TRACE=perf every 600 frames: what holds device-local memory, to see what grows over a long session.
// VMA's blocks and allocations on the device-local heaps, texture entries with their own memory, the VRAM mirrors and
// the objects waiting for their last submission to retire.
static void vram_log(uint64_t frame) {
    VkCtx *c = vk_ctx();
    if (!c) return;
    VmaBudget vb[VK_MAX_MEMORY_HEAPS];
    vmaGetHeapBudgets(c->vma, vb);
    uint64_t blocks = 0, block_bytes = 0, allocs = 0, alloc_bytes = 0;
    for (uint32_t h = 0; h < c->mem_props.memoryHeapCount && h < VK_MAX_MEMORY_HEAPS; h++)
        if (c->mem_props.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            blocks += vb[h].statistics.blockCount, block_bytes += vb[h].statistics.blockBytes,
                allocs += vb[h].statistics.allocationCount, alloc_bytes += vb[h].statistics.allocationBytes;
    VkTexCensus t;
    vk_texture_census(&t);
    const double M = 1048576.0;
    uint64_t used = 0, budget = 0;
    int exact = 0;
    vk_mem_vram(&used, &budget, &exact);   // the driver's count for this process: also what VMA does not hold
    rt_log("mem: vram frame %llu: driver %.0f of %.0f MiB%s; vma %llu blocks %.0f MiB, %llu allocations %.0f MiB; textures "
           "live %llu (own memory %llu, %.0f MiB), released %llu (own memory %llu, %.0f MiB); mirrors %lld %.0f MiB; "
           "releases pending %d",
           (unsigned long long)frame, used / M, budget / M, exact ? "" : " (estimated)", (unsigned long long)blocks,
           block_bytes / M, (unsigned long long)allocs,
           alloc_bytes / M, (unsigned long long)t.live, (unsigned long long)t.own_live, t.bytes_live / M,
           (unsigned long long)t.released, (unsigned long long)t.own_released, t.bytes_released / M, (long long)m_live_n,
           m_live_bytes / M, dead_pending());
    // DEBUG-VMADUMP: TOTK_VMA_DUMP=<path prefix> writes VMA's detailed map (every allocation with its size, kind and
    // usage flags) to <prefix><frame>.json at each of these lines, to tell what the allocations beside textures are.
    const char *dump = cfg_get("TOTK_VMA_DUMP");
    if (dump && *dump) {
        char *s = NULL, path[PLAT_PATH_MAX * 2];
        vmaBuildStatsString(c->vma, &s, VK_TRUE);
        snprintf(path, sizeof path, "%s%llu.json", dump, (unsigned long long)frame);
        FILE *f = s ? fopen(path, "wb") : NULL;
        if (f) fwrite(s, 1, strlen(s), f), fclose(f);
        if (s) vmaFreeStatsString(c->vma, s);
    }
}

// Device class: INTEGRATED -> uma; a discrete GPU with a DEVICE_LOCAL heap of 1 GiB or more beside a
// heap that is not device-local (host memory) -> discrete; otherwise uma (import everywhere). Sets the mirror mode.
void vk_mem_class_log(const VkPhysicalDeviceProperties *p, const VkPhysicalDeviceMemoryProperties *m) {
    int big_local = -1, host_heap = -1;
    for (uint32_t h = 0; h < m->memoryHeapCount; h++) {
        if (m->memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
            if (m->memoryHeaps[h].size >= (1ull << 30) && big_local < 0) big_local = (int)h;
        } else if (host_heap < 0) host_heap = (int)h;
    }
    static const char *const types[] = {"other", "integrated", "discrete", "virtual", "cpu"};
    int t = (int)p->deviceType;
    const char *tn = t >= 0 && t <= 4 ? types[t] : "unknown";
    const char *cls, *why;
    if (p->deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) cls = "uma", why = "integrated gpu";
    else if (p->deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) cls = "uma", why = "not a discrete gpu";
    else if (big_local < 0) cls = "uma", why = "no device-local heap of 1 GiB or more";
    else if (host_heap < 0) cls = "uma", why = "every heap is device-local";
    else cls = "discrete", why = "device-local heap separate from host memory";
    int discrete = cls[0] == 'd';
    char line[2048];
    size_t k = (size_t)snprintf(line, sizeof line, "mem: class %s (%s; device type %d %s, %s)", cls, why, t, tn, p->deviceName);
    for (uint32_t h = 0; h < m->memoryHeapCount; h++)
        k += (size_t)snprintf(line + k, k < sizeof line ? sizeof line - k : 0, "; heap %u %.0f MiB flags %#x%s%s", h,
                              m->memoryHeaps[h].size / 1048576.0, m->memoryHeaps[h].flags,
                              m->memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT ? " DEVICE_LOCAL" : "",
                              m->memoryHeaps[h].flags & VK_MEMORY_HEAP_MULTI_INSTANCE_BIT ? " MULTI_INSTANCE" : "");
    rt_log("%s", line);
    k = (size_t)snprintf(line, sizeof line, "mem: types");
    for (uint32_t i = 0; i < m->memoryTypeCount; i++) {
        VkMemoryPropertyFlags f = m->memoryTypes[i].propertyFlags;
        k += (size_t)snprintf(line + k, k < sizeof line ? sizeof line - k : 0, "; %u heap %u flags %#x%s%s%s%s%s", i,
                              m->memoryTypes[i].heapIndex, f, f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT ? " DEVICE_LOCAL" : "",
                              f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT ? " HOST_VISIBLE" : "",
                              f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT ? " HOST_COHERENT" : "",
                              f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT ? " HOST_CACHED" : "",
                              f & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT ? " LAZY" : "");
    }
    rt_log("%s", line);
    mirror_mode(discrete);
}

// ---------------------------------------------------------------- upload batches and deferred destruction
typedef struct { VkBuffer buf; VmaAllocation alloc; } Staging;
typedef struct {
    VkCommandBuffer cb;
    VkFence fence;
    uint64_t seq;
    Staging *stg;
    int nstg, cap;
    int ub;   // TOTK_SECTIONS: the batch's timestamp pair + 1, 0 none
} Batch;

enum { DEAD_IMAGE = 1, DEAD_IMAGE_VMA, DEAD_VIEW, DEAD_BUFFER_VMA, DEAD_HOST, DEAD_CALL };   // from DEAD_HOST on: no Vulkan object
typedef struct { int kind; uint64_t handle; VmaAllocation alloc; uint64_t seq, tl; } Dead;

static PlatRMutex cmd_lock;
static PlatOnce cmd_once = PLAT_ONCE_INIT;
static VkCommandPool cmd_pool;
static Batch open_b;
static int open_active;
static Batch *inflight;
static int ninflight, cap_inflight;
static Batch *spare;                  // completed batches whose command buffer and fence are reused
static int nspare, cap_spare;
static uint64_t next_seq, done_seq;   // batches handed out / completed (all batches up to done_seq)
static Dead *dead;
static int ndead, cap_dead;
static uint64_t tl_noted;             // newest timeline value vk_sync.c reported submitted

// TOTK_SECTIONS: GPU time of upload batches, a timestamp pair per batch read when the batch is retired (SEC_GPU_UPLOAD).
#define UB_PAIRS 256
static VkQueryPool ub_pool;
static int ub_tried;
static uint32_t ub_next;
static double ub_period;

static void ub_open(VkCtx *c) {   // cmd_lock; open_b has just begun
    if (!ub_tried) {
        ub_tried = 1;
        VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP,
                                    .queryCount = 2 * UB_PAIRS};
        if (c->props.limits.timestampPeriod > 0 && vkCreateQueryPool(c->device, &qi, NULL, &ub_pool) == VK_SUCCESS)
            ub_period = c->props.limits.timestampPeriod;
        else ub_pool = VK_NULL_HANDLE;
    }
    if (!ub_pool) return;
    uint32_t p = ub_next++ % UB_PAIRS;
    vkCmdResetQueryPool(open_b.cb, ub_pool, 2 * p, 2);
    vkCmdWriteTimestamp(open_b.cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, ub_pool, 2 * p);
    open_b.ub = (int)p + 1;
}

static void ub_close(void) {
    if (open_b.ub) vkCmdWriteTimestamp(open_b.cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, ub_pool, 2 * (uint32_t)(open_b.ub - 1) + 1);
}

static void ub_read(VkCtx *c, Batch *b) {   // the batch's fence has signalled
    uint64_t v[2];
    if (vkGetQueryPoolResults(c->device, ub_pool, 2 * (uint32_t)(b->ub - 1), 2, sizeof v, v, sizeof v[0], VK_QUERY_RESULT_64_BIT) == VK_SUCCESS &&
        v[1] >= v[0])
        sec_add_ns(SEC_GPU_UPLOAD, (uint64_t)((double)(v[1] - v[0]) * ub_period));
    b->ub = 0;
}

static void cmd_init(void *p) {
    (void)p;
    return;
}

static void grow(void **arr, int *cap, size_t elem) {
    *cap = *cap ? *cap * 2 : 16;
    void *n = realloc(*arr, (size_t)*cap * elem);
    if (!n) rt_fatal("vk: out of memory (upload bookkeeping)");
    *arr = n;
}

// ---- the reaper
// Destroying a Vulkan object costs the driver 30-80 us, and a frame lets go of dozens (the game makes its transient
// render targets anew every frame). That ran under cmd_lock on whichever thread collected: the completion thread before
// it reported a fence done, or the replay thread inside vk_upload_flush, and a submission was seen waiting 2 ms for the
// lock. Objects whose users completed now go on a list, and a thread of its own destroys them with no lock of the
// upload path held. vk_mem_reap_drain waits until the list is empty (shutdown, and before the device is asked to be idle
// for good).
static PlatMutex reap_lock = PLAT_MUTEX_INIT;
static PlatCond reap_cv = PLAT_COND_INIT;
static Dead *ripe;
static int nripe, cap_ripe, reap_busy, reap_started;

// DEBUG-LIVECHECK (TOTK_LIVE_CHECK=1): the images and views that wait for their destruction, by handle, with the
// timeline value they wait for (defer puts them in, reap_one takes them out before the handle can belong to a new
// object). A recording that is handed one whose wait ends before the recording's own submission would have the GPU
// read it after it is gone: the device fault with an invalid read, found here on the CPU, at the moment and with the
// path that did it, whether or not the GPU would have faulted. vk_live_check_use is called where a recording takes an
// image or a view: a draw's texture binding, the view cache, a render target, the texture pool's slots.
#define LC_N (1u << 16)
static struct { uint64_t handle, tl; } *lc_tab;   // open addressing; handle 0 free, 1 a taken-out entry
// The destroyed ones, by handle (direct-mapped, a later one replaces): the value each waited for and when it went.
// vk_live_check_born takes a handle out when a new image or view gets it (the texture module's and the view cache's
// creations call it); until then a recording that takes the handle takes a destroyed object.
#define LG_N (1u << 17)
static struct { uint64_t handle, tl, t_ns; } *lg_tab;
static PlatMutex lc_lock = PLAT_MUTEX_INIT;
static int lc_on = -1;
static volatile int32_t lc_reports;
static uint64_t lc_uses, lc_same;   // checks made; uses of an object that goes with the recording's own submission

int vk_live_check_on(void) {
    if (lc_on < 0) {
        int on = getenv("TOTK_LIVE_CHECK") != NULL;
        if (on && (!(lc_tab = calloc(LC_N, sizeof *lc_tab)) || !(lg_tab = calloc(LG_N, sizeof *lg_tab)))) on = 0;
        if (on) rt_log("vk: live check on (TOTK_LIVE_CHECK): a recording that takes an image or view already waiting for "
                       "its destruction is reported");
        lc_on = on;
    }
    return lc_on;
}

static void lc_put(uint64_t handle, uint64_t tl) {
    plat_mutex_lock(&lc_lock);
    uint32_t i = (uint32_t)((handle * 0x9E3779B97F4A7C15ull) >> 48), free_at = LC_N;
    for (uint32_t k = 0; k < LC_N; k++, i = (i + 1) & (LC_N - 1)) {
        if (lc_tab[i].handle == handle) { free_at = i; break; }
        if (lc_tab[i].handle == 1 && free_at == LC_N) free_at = i;
        if (!lc_tab[i].handle) { if (free_at == LC_N) free_at = i; break; }
    }
    if (free_at < LC_N) lc_tab[free_at].handle = handle, lc_tab[free_at].tl = tl;
    plat_mutex_unlock(&lc_lock);
}

static void lc_del(uint64_t handle) {   // it is destroyed now
    plat_mutex_lock(&lc_lock);
    uint32_t i = (uint32_t)((handle * 0x9E3779B97F4A7C15ull) >> 48);
    uint64_t tl = 0;
    for (uint32_t k = 0; k < LC_N && lc_tab[i].handle; k++, i = (i + 1) & (LC_N - 1))
        if (lc_tab[i].handle == handle) { tl = lc_tab[i].tl, lc_tab[i].handle = 1; break; }
    uint32_t g = (uint32_t)((handle * 0x9E3779B97F4A7C15ull) >> 47);
    lg_tab[g].handle = handle, lg_tab[g].tl = tl, lg_tab[g].t_ns = plat_now_ns();
    plat_mutex_unlock(&lc_lock);
}

void vk_live_check_born(uint64_t handle) {   // a new image or view has this handle
    if (lc_on <= 0 || !handle) return;
    plat_mutex_lock(&lc_lock);
    uint32_t g = (uint32_t)((handle * 0x9E3779B97F4A7C15ull) >> 47);
    if (lg_tab[g].handle == handle) lg_tab[g].handle = 0;
    plat_mutex_unlock(&lc_lock);
}

// A recording takes the image or view `handle` (what: the path; id: a slot or texture handle for the log). 1 when it
// waits for a destruction that comes before the submission being recorded.
int vk_live_check_use(uint64_t handle, const char *what, uint64_t id) {
    if (!lc_on || !handle) return 0;
    uint64_t tl = 0;
    int found = 0;
    plat_mutex_lock(&lc_lock);
    lc_uses++;
    uint32_t i = (uint32_t)((handle * 0x9E3779B97F4A7C15ull) >> 48);
    for (uint32_t k = 0; k < LC_N && lc_tab[i].handle; k++, i = (i + 1) & (LC_N - 1))
        if (lc_tab[i].handle == handle) { tl = lc_tab[i].tl, found = 1; break; }
    uint64_t f = vk_exec_recording_fence();
    if (found && f && tl >= f) lc_same++, found = 0;   // it lives until this submission is done
    uint32_t g = (uint32_t)((handle * 0x9E3779B97F4A7C15ull) >> 47);
    uint64_t dead_tl = 0, dead_ns = 0;
    int dead = !found && lg_tab[g].handle == handle;
    if (dead) dead_tl = lg_tab[g].tl, dead_ns = lg_tab[g].t_ns;
    plat_mutex_unlock(&lc_lock);
    if (dead) {
        if (plat_inc32(&lc_reports) <= 24)
            rt_log("LIVE CHECK: %s takes %#llx (id %#llx), which was DESTROYED %.1f ms ago (it had waited for value %llu); the "
                   "submission being recorded is %llu, the newest submitted %llu",
                   what, (unsigned long long)handle, (unsigned long long)id, (plat_now_ns() - dead_ns) / 1e6,
                   (unsigned long long)dead_tl, (unsigned long long)f, (unsigned long long)vk_sync_issued());
        return 1;
    }
    if (!found) return 0;
    if (plat_inc32(&lc_reports) <= 24)
        rt_log("LIVE CHECK: %s takes %#llx (id %#llx), which waits for its destruction at value %llu; the submission being "
               "recorded is %llu, the newest submitted %llu, the GPU is past %llu",
               what, (unsigned long long)handle, (unsigned long long)id, (unsigned long long)tl, (unsigned long long)f,
               (unsigned long long)vk_sync_issued(), (unsigned long long)vk_sync_fence_done());
    return 1;
}

void vk_live_check_log(void) {
    if (lc_on > 0)
        rt_log("vk: live check: %llu uses checked, %llu of objects that go with the same submission, %d reports",
               (unsigned long long)lc_uses, (unsigned long long)lc_same, (int)lc_reports);
}

// DEBUG-LIVECHECK: an image or view is about to be destroyed because the timeline counter reached the value it waited
// for. The GPU's breadcrumbs must agree: the newest command that finished belongs to a later submission, or is that
// submission's last. If they do not, the counter ran ahead of the GPU's work and the object goes while a submission
// that may read it is still executing.
static void lc_reap_check(VkCtx *c, const Dead *d) {
    int vk_crumbs_progress(uint32_t *sub, int *ended, uint32_t *raw);
    static uint64_t n_checked, n_unknown;
    static volatile int32_t n_ahead;
    uint32_t sub = 0, raw = 0;
    int ended = 0, work = 0;
    if (!d->tl) return;
    // the newest submission up to the value that sent command buffers at all: an empty one leaves no crumb
    uint64_t wv = d->tl;
    for (int k = 0; k < 512 && wv; k++, wv--)
        if ((work = vk_sync_sub_work(wv)) != 0) break;
    if (work <= 0 || !wv) { n_unknown++; return; }
    uint32_t want = (uint32_t)(wv & 0x0fffffff);
    if (!vk_crumbs_progress(&sub, &ended, &raw)) { n_unknown++; return; }
    n_checked++;
    if (sub > want || (sub == want && ended)) return;
    if (sub == want && !(work & 1)) {   // its last batch was empty: no end crumb to wait for
        static volatile int32_t n_empty;
        if (plat_inc32(&n_empty) <= 8)
            rt_log("LIVE CHECK (empty last batch): %#llx is destroyed for value %llu while the GPU's newest finished command "
                   "is inside submission %u, whose last batch had no command buffer (crumb %#x)",
                   (unsigned long long)d->handle, (unsigned long long)d->tl, sub, raw);
        return;
    }
    uint64_t now = 0;
    if (c->timeline) vkGetSemaphoreCounterValue(c->device, c->timeline, &now);
    if (plat_inc32(&n_ahead) <= 24)
        rt_log("LIVE CHECK: %s %#llx is destroyed for value %llu (the timeline counter is at %llu, the newest submitted "
               "%llu; the newest submission up to the value that sent commands is %llu), but by its breadcrumbs the GPU has "
               "finished nothing past %s submission %u (crumb %#x); %llu destructions agreed so far, %llu had nothing to "
               "compare", d->kind == DEAD_VIEW ? "view" : "image", (unsigned long long)d->handle, (unsigned long long)d->tl,
               (unsigned long long)now, (unsigned long long)vk_sync_issued(), (unsigned long long)wv,
               ended ? "the end of" : "a command inside", sub, raw, (unsigned long long)n_checked, (unsigned long long)n_unknown);
}

static void reap_one(VkCtx *c, const Dead *d) {
    if (lc_on > 0 && d->kind <= DEAD_VIEW) lc_del(d->handle), lc_reap_check(c, d);
    switch (d->kind) {
    case DEAD_IMAGE: vkDestroyImage(c->device, (VkImage)(uintptr_t)d->handle, NULL); break;
    case DEAD_IMAGE_VMA: vmaDestroyImage(c->vma, (VkImage)(uintptr_t)d->handle, d->alloc); break;
    case DEAD_VIEW: vkDestroyImageView(c->device, (VkImageView)(uintptr_t)d->handle, NULL); break;
    case DEAD_BUFFER_VMA: vmaDestroyBuffer(c->vma, (VkBuffer)(uintptr_t)d->handle, d->alloc); break;
    case DEAD_HOST: free((void *)(uintptr_t)d->handle); break;
    case DEAD_CALL: ((void (*)(void *))(uintptr_t)d->handle)((void *)d->alloc); break;   // vk_defer_call
    }
}

static void reap_main(void *arg) {
    (void)arg;
    Dead *mine = NULL;
    int cap = 0;
    plat_mutex_lock(&reap_lock);
    for (;;) {
        while (!nripe) plat_cond_wait(&reap_cv, &reap_lock);
        Dead *t = ripe;
        int n = nripe, tc = cap_ripe;
        ripe = mine, cap_ripe = cap, nripe = 0;
        mine = t, cap = tc;
        reap_busy = 1;
        plat_mutex_unlock(&reap_lock);
        VkCtx *c = vk_ctx();
        for (int i = 0; i < n && c; i++) reap_one(c, &mine[i]);
        plat_mutex_lock(&reap_lock);
        reap_busy = 0;
        plat_cond_broadcast(&reap_cv);
    }
}

static void reap_push(VkCtx *c, const Dead *d) {
    static int direct = -1;   // TOTK_REAPER=0: destroyed where collected, as before
    if (direct < 0) direct = !rt_knob("REAPER", "1");
    if (direct) { reap_one(c, d); return; }
    plat_mutex_lock(&reap_lock);
    if (!reap_started) reap_started = plat_thread_spawn(reap_main, NULL, "vk reaper", 0) ? 1 : -1;
    if (reap_started < 0) {
        plat_mutex_unlock(&reap_lock);
        reap_one(c, d);
        return;
    }
    if (nripe == cap_ripe) {
        cap_ripe = cap_ripe ? cap_ripe * 2 : 256;
        Dead *n = realloc(ripe, (size_t)cap_ripe * sizeof *ripe);
        if (!n) rt_fatal("vk: out of memory (reaper)");
        ripe = n;
    }
    ripe[nripe++] = *d;
    plat_cond_broadcast(&reap_cv);
    plat_mutex_unlock(&reap_lock);
}

void vk_mem_reap_drain(void) {
    plat_mutex_lock(&reap_lock);
    while (nripe || reap_busy) plat_cond_wait(&reap_cv, &reap_lock);
    plat_mutex_unlock(&reap_lock);
}

static void free_staging(VkCtx *c, Batch *b) {
    for (int i = 0; i < b->nstg; i++)
        reap_push(c, &(Dead){DEAD_BUFFER_VMA, (uint64_t)(uintptr_t)b->stg[i].buf, b->stg[i].alloc, 0, 0});
    b->nstg = 0;
}

// Batches complete in submission order (one queue): retire from the front while their fences are signalled.
static void retire(VkCtx *c, int wait_all) {
    int k = 0;
    for (; k < ninflight; k++) {
        Batch *b = &inflight[k];
        if (wait_all) vkWaitForFences(c->device, 1, &b->fence, VK_TRUE, UINT64_MAX);
        else if (vkGetFenceStatus(c->device, b->fence) != VK_SUCCESS) break;
        if (b->ub) ub_read(c, b);
        free_staging(c, b);
        done_seq = b->seq;
        if (nspare == cap_spare) grow((void **)&spare, &cap_spare, sizeof *spare);
        spare[nspare++] = *b;
    }
    memmove(inflight, inflight + k, (size_t)(ninflight - k) * sizeof *inflight);
    ninflight -= k;
}

static uint64_t lost_seen_ns;   // when the timeline counter first read as a lost device's (the fault report's clock)
uint64_t vk_mem_lost_seen_ns(void) { return lost_seen_ns; }

static void collect_locked(VkCtx *c) {
    uint64_t ft_t = ftrace_on ? plat_now_ns() : 0;
    int ft_n0 = ndead;
    retire(c, 0);
    uint64_t ft_r = ft_t ? plat_now_ns() - ft_t : 0;
    uint64_t tl = 0;
    VkResult cr = c->timeline ? vkGetSemaphoreCounterValue(c->device, c->timeline, &tl) : VK_SUCCESS;
    // A lost device answers with an error, and NVIDIA's driver with the counter at its largest value (every waiter is
    // let go). Taken at its word, that destroyed every deferred object at once, at the moment of the fault: harmless
    // for a GPU that is gone, but the fault report then showed each of them unbound "just before" the read, which
    // sent the search for the device fault of 2026-11-14 after use-after-destroy for hours. Nothing is destroyed on
    // such an answer; what waits stays as it was when the GPU stopped.
    if (cr != VK_SUCCESS || tl == UINT64_MAX) {
        static volatile int32_t said;
        if (!lost_seen_ns) lost_seen_ns = plat_now_ns();
        if (plat_inc32(&said) == 1)
            rt_log("vk: the timeline counter reads %llu (result %d): the device is lost; %d deferred objects stay as they are",
                   (unsigned long long)tl, (int)cr, ndead);
        tl = 0;
    }
    if (lc_on > 0 && tl) lc_reap_check(c, &(Dead){DEAD_IMAGE, 0, NULL, 0, tl});   // DEBUG-LIVECHECK: the counter itself, each time it is read
    int k = 0;
    for (int i = 0; i < ndead; i++) {
        Dead *d = &dead[i];
        if (d->seq > done_seq || d->tl > tl) { dead[k++] = *d; continue; }
        if (d->kind < DEAD_HOST)   // (not a Vulkan object: nothing for the recorder's handle check)
            rec_check_handle(d->kind == DEAD_VIEW ? REC_VIEW : d->kind == DEAD_BUFFER_VMA ? REC_BUFFER : REC_IMAGE, d->handle, 1,
                             d->tl, tl);
        reap_push(c, d);   // destroyed by the reaper, outside cmd_lock
    }
    ndead = k;
    if (ft_t && plat_now_ns() - ft_t > 30000) FT(FT_NOTE, 15, plat_now_ns() - ft_t, (uint64_t)(ft_n0 - k) << 32 | (ft_r / 1000));
}

VkCommandBuffer vk_upload_begin(void) {
    plat_once(&cmd_once, cmd_init, NULL);
    plat_rmutex_lock(&cmd_lock);
    VkCtx *c = vk_ctx();
    if (!c) rt_fatal("vk: upload before vk_init");
    if (open_active) return open_b.cb;
    if (!cmd_pool) {
        VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                      .queueFamilyIndex = c->gfx_family};
        if (vkCreateCommandPool(c->device, &pi, NULL, &cmd_pool) != VK_SUCCESS) rt_fatal("vk: upload command pool");
    }
    retire(c, 0);
    memset(&open_b, 0, sizeof open_b);
    if (nspare) {
        Batch s = spare[--nspare];
        open_b.cb = s.cb, open_b.fence = s.fence, open_b.stg = s.stg, open_b.cap = s.cap;
        vkResetCommandBuffer(open_b.cb, 0);
        vkResetFences(c->device, 1, &open_b.fence);
    } else {
        VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cmd_pool,
                                          .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
        VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (vkAllocateCommandBuffers(c->device, &ai, &open_b.cb) != VK_SUCCESS ||
            vkCreateFence(c->device, &fi, NULL, &open_b.fence) != VK_SUCCESS)
            rt_fatal("vk: upload command buffer");
    }
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(open_b.cb, &bi);
    open_b.seq = ++next_seq;
    vk_crumb(open_b.cb, VK_CRUMB_UPLOAD, open_b.seq);
    if (__builtin_expect(sec_on, 0)) ub_open(c);
    open_active = 1;
    return open_b.cb;
}

void vk_upload_end(void) { plat_rmutex_unlock(&cmd_lock); }

// Staging memory comes from a pool of its own that keeps STG_MIN_BLOCKS blocks from its first use (at boot). In the
// default pools VMA creates a block under the lock of its memory type (VmaBlockVector::Allocate), so a block made by
// the headroom thread (vk_headroom.c) or a burst of uploads held every staging allocation of the render thread: one
// took 47-50 ms, for a few bytes, in the first frame after a save load. Larger buffers than a block go to the default
// pool.
#define STG_BLOCK (64ull << 20)
#define STG_MIN_BLOCKS 2
static VmaPool stg_pool;
static int stg_pool_tried;

void *vk_staging(VkDeviceSize size, VkBuffer *buf, VmaAllocation *alloc) {
    uint64_t sec0 = sec_begin();
    VkCtx *c = vk_ctx();
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size ? size : 16,
                             .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo aci = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                   .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
    if (!__atomic_load_n(&stg_pool_tried, __ATOMIC_ACQUIRE)) {
        plat_once(&cmd_once, cmd_init, NULL);
        plat_rmutex_lock(&cmd_lock);
        if (!stg_pool_tried) {
            uint32_t type;
            VmaPoolCreateInfo pi = {.blockSize = STG_BLOCK, .minBlockCount = STG_MIN_BLOCKS};
            if (vmaFindMemoryTypeIndexForBufferInfo(c->vma, &bi, &aci, &type) == VK_SUCCESS) {
                pi.memoryTypeIndex = type;
                if (vmaCreatePool(c->vma, &pi, &stg_pool) != VK_SUCCESS) stg_pool = NULL;
            }
            rt_log("vk: staging pool %s (%d blocks of %llu MiB kept)", stg_pool ? "created" : "unavailable: default pool",
                   STG_MIN_BLOCKS, (unsigned long long)(STG_BLOCK >> 20));
            __atomic_store_n(&stg_pool_tried, 1, __ATOMIC_RELEASE);
        }
        plat_rmutex_unlock(&cmd_lock);
    }
    if (stg_pool && bi.size <= STG_BLOCK / 2) aci.pool = stg_pool;
    VmaAllocationInfo info;
    VkBuffer b;
    VmaAllocation a;
    VkResult r = vmaCreateBuffer(c->vma, &bi, &aci, &b, &a, &info);
    if (r != VK_SUCCESS) rt_fatal("vk: staging buffer of %llu bytes failed (%d)", (unsigned long long)size, (int)r);
    *buf = b;
    if (alloc) *alloc = a;   // the caller destroys it (vmaDestroyBuffer) after vk_upload_flush(1)
    else {
        if (open_b.nstg == open_b.cap) grow((void **)&open_b.stg, &open_b.cap, sizeof *open_b.stg);
        open_b.stg[open_b.nstg++] = (Staging){b, a};
    }
    sec_end(SEC_STAGING, sec0);
    return info.pMappedData;
}

uint64_t vk_upload_flush(int wait) {
    uint64_t sec0 = sec_begin();
    plat_once(&cmd_once, cmd_init, NULL);
    uint64_t ft_t = ftrace_on ? plat_now_ns() : 0;
    plat_rmutex_lock(&cmd_lock);
    if (ft_t && plat_now_ns() - ft_t > 50000) FT(FT_NOTE, 14, plat_now_ns() - ft_t, 0);
    VkCtx *c = vk_ctx();
    uint64_t seq = next_seq;
    if (c && open_active) {
        for (int i = 0; i < open_b.nstg; i++) vmaFlushAllocation(c->vma, open_b.stg[i].alloc, 0, VK_WHOLE_SIZE);
        ub_close();
        vk_crumb(open_b.cb, VK_CRUMB_UPLOAD_END, open_b.seq);
        vkEndCommandBuffer(open_b.cb);
        VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &open_b.cb};
        vk_queue_lock();
        VkResult r = vkQueueSubmit(c->gfx_queue, 1, &si, open_b.fence);
        vk_queue_unlock();
        if (r != VK_SUCCESS) {
            vk_device_fault_report(r);
            rt_fatal("vk: upload submit failed (%d)", (int)r);
        }
        if (ninflight == cap_inflight) grow((void **)&inflight, &cap_inflight, sizeof *inflight);
        inflight[ninflight++] = open_b;
        open_active = 0;
    }
    if (c && wait) {
        while (ninflight && done_seq < seq) {
            vkWaitForFences(c->device, 1, &inflight[0].fence, VK_TRUE, UINT64_MAX);
            retire(c, 0);
        }
    }
    if (c) collect_locked(c);
    plat_rmutex_unlock(&cmd_lock);
    sec_end(SEC_UP_FLUSH, sec0);
    return seq;
}

static void defer(int kind, uint64_t handle, VmaAllocation alloc) {
    plat_once(&cmd_once, cmd_init, NULL);
    plat_rmutex_lock(&cmd_lock);
    if (ndead == cap_dead) grow((void **)&dead, &cap_dead, sizeof *dead);
    // The open batch may record into the object: it counts as the newest user. The GPU must also be past every
    // submission that names it. tl_noted covers the submitted ones; an object the replay lets go of while it records
    // a submission (vk_exec_recording_fence) may be named by that one too, not yet submitted: vk_texture.c
    // slot_resolve hands a draw a released entry, and entry_of destroys released entries at that address later in
    // the same submission. Stamped with the last submitted value, the object went as soon as the GPU reached it,
    // before the submission that names it ran, and with the recorder thread (vk_rec.c) behind, before the recorder
    // made the call that names it. LAS Recomp and Savage Wind Recomp stamp every deferral with the submission being
    // recorded (runtime/nvn/vulkan/textures.c defer: vk_pending_seq). TOTK_DEFER_STAMP=0 brings back the old stamp.
    static int stamp = -1;
    if (stamp < 0) stamp = rt_knob("DEFER_STAMP", "1") != 0;
    uint64_t tl = tl_noted, rf = stamp ? vk_exec_recording_fence() : 0;
    // vk_exec_recording_fence knows only the recording thread's own submission. A texture a guest thread releases (the
    // synchronous replay: vk_texture_release, entry_of, sweep_released on the game's loader threads) can be named by
    // the submission another thread records meanwhile: that one is issued + 1 (vk_sync_submit sets issued after it).
    if (stamp && !rf) rf = vk_sync_issued() + 1;
    if (rf > tl) tl = rf;
    dead[ndead++] = (Dead){kind, handle, alloc, open_active ? open_b.seq : next_seq, tl};
    if (vk_live_check_on() && kind <= DEAD_VIEW) lc_put(handle, tl);   // DEBUG-LIVECHECK
    if (kind < DEAD_HOST)
        rec_check_handle(kind == DEAD_VIEW ? REC_VIEW : kind == DEAD_BUFFER_VMA ? REC_BUFFER : REC_IMAGE, handle, 0, tl,
                         vk_sync_fence_done());
    plat_rmutex_unlock(&cmd_lock);
}

static int dead_pending(void) {
    plat_once(&cmd_once, cmd_init, NULL);
    plat_rmutex_lock(&cmd_lock);
    int n = ndead;
    plat_rmutex_unlock(&cmd_lock);
    return n;
}

void vk_defer_image(VkImage img, VmaAllocation alloc) {
    if (img) defer(alloc ? DEAD_IMAGE_VMA : DEAD_IMAGE, (uint64_t)(uintptr_t)img, alloc);
}
void vk_defer_view(VkImageView view) { if (view) defer(DEAD_VIEW, (uint64_t)(uintptr_t)view, NULL); }
void vk_defer_free(void *p) { if (p) defer(DEAD_HOST, (uint64_t)(uintptr_t)p, NULL); }
// fn(arg) on the reaper thread (or where the deferrals are collected, with TOTK_REAPER=0) once the GPU is past the
// open upload batch and every submission made or being recorded now: for reading what such a batch copied back.
void vk_defer_call(void (*fn)(void *), void *arg) { defer(DEAD_CALL, (uint64_t)(uintptr_t)fn, (VmaAllocation)arg); }
void vk_defer_buffer(VkBuffer buf, VmaAllocation alloc) {
    if (buf) defer(DEAD_BUFFER_VMA, (uint64_t)(uintptr_t)buf, alloc);
}

void vk_mem_note_submit(uint64_t timeline_value) {
    plat_once(&cmd_once, cmd_init, NULL);
    plat_rmutex_lock(&cmd_lock);
    if (timeline_value > tl_noted) tl_noted = timeline_value;
    plat_rmutex_unlock(&cmd_lock);
}

void vk_mem_collect(void) {
    VkCtx *c = vk_ctx();
    if (!c) return;
    plat_once(&cmd_once, cmd_init, NULL);
    plat_rmutex_lock(&cmd_lock);
    collect_locked(c);
    plat_rmutex_unlock(&cmd_lock);
}

void vk_mem_stats(VkMemStats *s) {
    memset(s, 0, sizeof *s);
    plat_mutex_lock_shared(&pool_lock);
    for (int i = 0; i < pool_index.n; i++) {
        const PoolRec *r = pool_index.e[i].buf;
        if (r->kind == POOL_IMPORT) s->imported_pools++;
        else if (r->kind == POOL_PHYS) s->physical_pools++;
        else s->virtual_pools++;
    }
    plat_mutex_unlock_shared(&pool_lock);
    s->import_failures = (uint64_t)n_import_failures;
    s->upload_batches = next_seq;
}

void vk_mem_shutdown(void) {
    VkCtx *c = vk_ctx();
    if (!c) return;
    vk_upload_flush(1);
    vk_queue_lock();
    vkDeviceWaitIdle(c->device);
    vk_queue_unlock();
    vk_texture_shutdown();
    plat_rmutex_lock(&cmd_lock);
    retire(c, 1);
    uint64_t keep = tl_noted;
    tl_noted = 0;
    for (int i = 0; i < ndead; i++) dead[i].seq = 0, dead[i].tl = 0;   // the device is idle
    collect_locked(c);
    vk_mem_reap_drain();
    tl_noted = keep;
    for (int i = 0; i < nspare; i++) {
        vkDestroyFence(c->device, spare[i].fence, NULL);
        free(spare[i].stg);
    }
    nspare = 0;
    if (cmd_pool) vkDestroyCommandPool(c->device, cmd_pool, NULL);
    cmd_pool = VK_NULL_HANDLE;
    if (stg_pool) vmaDestroyPool(c->vma, stg_pool);   // after retire: no staging buffer is left in it
    stg_pool = NULL;
    stg_pool_tried = 0;
    plat_rmutex_unlock(&cmd_lock);
    plat_mutex_lock(&pool_lock);
    for (int i = 0; i < pool_index.n; i++) destroy_rec(c, pool_index.e[i].buf);
    pool_index.n = 0;
    nm = npend = nwbl = nsnl = snl_copied = 0;   // mirrors went with their pools
    plat_mutex_lock(&snap_pool_lock);
    for (int i = 0; i < nsnap_pool; i++) vmaDestroyBuffer(c->vma, snap_pool[i].buf, snap_pool[i].alloc);
    nsnap_pool = 0, snap_pool_bytes = 0;
    plat_mutex_unlock(&snap_pool_lock);
    plat_mutex_lock(&shadow_lock);
    nstouch = nspend = 0;   // shadows too
    s_live_n = s_live_bytes = 0;
    plat_mutex_unlock(&shadow_lock);
    for (int i = 0; i < nretiring; i++) destroy_rec(c, retiring[i]);   // finalized ones whose callback never ran
    nretiring = 0;
    __atomic_add_fetch(&pool_gen, 1, __ATOMIC_RELEASE);
    nphys = 0;
    plat_mutex_unlock(&pool_lock);
    plat_mutex_lock(&gen_lock);
    for (uint64_t i = 0; i < GEN_CHUNKS; i++) free(gen_chunk[i]), gen_chunk[i] = NULL;
    plat_mutex_unlock(&gen_lock);
}

// Hot pools: a mirrored pool of 1 MiB or more that the CPU -> mirror copies take whole (90 % or more) for HOT_FRAMES
// frames in a row is one of the engine's per-frame rings. It gives up its mirror: the GPU reads guest memory directly,
// as the console's GPU reads the engine's rings in unified memory. The copies ran when the GPU reached them, so with
// frames overlapping they took the next frame's bytes (white frames), and they moved ~90 MiB a frame over the bus on
// the porch. TOTK_HOT_DEMOTE=0 keeps the mirrors.
#define HOT_FRAMES 30
static PoolRec *hot_q[64];
static int nhot_q;

static void hot_census(void) {   // vk_mem_census, once a frame (the replay's thread)
    static int on = -1;
    if (on < 0) on = rt_knob("HOT_DEMOTE", "0");   // off: no help with the white frames, and the GPU reads slower
    if (!on) return;
    plat_mutex_lock_shared(&pool_lock);
    for (int i = 0; i < nm; i++) {
        PoolRec *r = mlist[i];
        uint64_t size = r->import_hi - r->import_lo;
        if (size >= (1ull << 20) && r->hot_bytes >= size / 10 * 9) r->hot_frames++;
        else r->hot_frames = 0;
        r->hot_bytes = 0;
        if (r->hot_frames == HOT_FRAMES && nhot_q < 64) hot_q[nhot_q++] = r;
    }
    plat_mutex_unlock_shared(&pool_lock);
}

static void hot_demote_pending(void) {   // vk_mem_mirror_begin (replay thread)
    if (!nhot_q) return;
    plat_mutex_lock(&pool_lock);
    for (int i = 0; i < nhot_q; i++) {
        PoolRec *r = hot_q[i];
        int j = 0;
        while (j < nm && mlist[j] != r) j++;   // still mirrored (not finalized or forgotten meanwhile)
        if (j == nm || r->mstate != MS_ON) continue;
        mirror_forget(r);
        r->mstate = MS_REFUSED, r->mwhy = MR_HOT;
        rt_log("mem: pool %#llx+%#llx (%.1f MiB) gives up its mirror: copied whole for %d frames in a row (a per-frame "
               "ring); the GPU reads it in guest memory", (unsigned long long)r->lo, (unsigned long long)r->size,
               (r->import_hi - r->import_lo) / 1048576.0, HOT_FRAMES);
    }
    nhot_q = 0;
    plat_mutex_unlock(&pool_lock);
    __atomic_add_fetch(&gmem_mgen, 1, __ATOMIC_RELEASE);   // the pool table points at the import buffers
    __atomic_add_fetch(&mirror_gen, 1, __ATOMIC_RELEASE);
}
