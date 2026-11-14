// The texture pool as one descriptor set (GAME_NS2; gfx.h GFX_POOL_SET).
//
// OoT's lit materials read their baked lighting (RBake_* lightmaps: BC5, BC6H and BC7 arrays) through
// texture handles they load from a table in a storage buffer, so no constant buffer names the texture: the translator
// indexes an array over the whole texture pool by the handle's texture id and a sampler array by its sampler id. With
// those arrays bound to the draw's own texture slots, every shadowed surface read a stand-in or another texture and
// came out black (the great tree, the village floor under the canopy). This set holds every registered texture at its
// id and every registered sampler at its id; gfx_shader.c moves such arrays here (patch_pool_arrays).
//
// Bindings (gfx.h): GFX_POOL_SAMP samplers, GFX_POOL_2D and GFX_POOL_2D_ARRAY sampled images (a 2D image's 2D-array
// view has one layer; an array image's 2D view is its first layer). The set is update-after-bind and partially bound:
// entries start as the replay's stand-ins and are written when a slot's texture is resolved. Each submission (once per
// vk_texture_epoch, on the replay thread) resolves the slots registered again since (the registration serial in
// vk_texture_slot_token), those whose image went (vk_tpool_image_gone, from destroy_tex) and SWEEP more in turn (a slot
// that follows another texture at its storage, storage the CPU rewrote, a slot whose pool was not live yet), through
// vk_texture_pool_slot_locked, which creates and uploads images as a draw binding would. The whole token changes too
// often to follow (its address generation is shared by every texture hashed to the same bucket: 6000 slots a frame
// resolved again in the forest). Making a texture resident costs an upload (and an ASTC decode), so each submission
// resolves slots for at most TOTK_TPOOL_MS milliseconds (default 2); a lightmap appears a few frames after its area
// loads.
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "gfx.h"
#include "host.h"
#include "nvn_api.h"
#include "tex.h"
#include "vk.h"
#include "vk_exec.h"
#include "platform/platform.h"

static struct {
    int state;                          // 0 not set up, 1 usable, -1 not available
    VkDescriptorSetLayout layout, empty;
    VkDescriptorPool pool;
    VkDescriptorSet set;
    VkImageView dummy2d, dummy2da;
    VkSampler dummys;
    uint32_t *reg;                      // per texture id: the registration serial its entries were written for (0: none)
    VkImage *img;                       // per texture id: the image its views belong to (VK_NULL_HANDLE: stand-ins)
    uint8_t *dirty;                     // per texture id: resolve again (its image went)
    VkImageView *v2d, *v2da;            // per texture id: the views written
    VkSampler samp[GFX_POOL_SAMPLERS];  // per sampler id: the sampler written
    uint32_t samp_gen, epoch, scan, sweep;   // scan: where the resolve budget stopped; sweep: the next slot resolved in turn
    int ntex;                           // texture ids the set covers
    uint64_t budget_ns;
    uint64_t written, resolved;
    VkImage *gone;                      // images destroy_tex dropped since the last update (texture lock)
    uint32_t ngone, cgone;
    int gone_all;                       // the list overflowed: every slot with an image resolves again
    int dirty_any;                      // vk_tpool_slot_dirty since the last complete look
    uint64_t upd_fence;                 // DEBUG-LIVECHECK: the submission the last update was made in
} T;

#define SWEEP 128   // slots resolved in turn per submission

static PlatMutex init_lock = PLAT_MUTEX_INIT;

static int setup(void) {
    VkCtx *c = vk_ctx();
    if (!c || !c->device) return 0;   // asked before the device exists: not decided yet
    if (!rt_knob("TPOOL", "1")) {
        rt_log("vk_texpool: off (TOTK_TPOOL=0): pool-indexed texture arrays keep the draw's slots");
        return -1;
    }
    VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f12};
    vkGetPhysicalDeviceFeatures2(c->phys, &f2);   // vk_device.c enables every feature the device reports
    VkPhysicalDeviceVulkan12Properties p12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
    VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &p12};
    vkGetPhysicalDeviceProperties2(c->phys, &p2);
    if (!f12.descriptorBindingSampledImageUpdateAfterBind || !f12.descriptorBindingPartiallyBound ||
        !f12.descriptorBindingUpdateUnusedWhilePending || !f12.runtimeDescriptorArray) {
        rt_log("vk_texpool: the device lacks update-after-bind sampled images or partially bound arrays: pool-indexed "
               "texture arrays keep the draw's slots");
        return -1;
    }
    uint32_t ntex = GFX_POOL_TEXTURES;
    if (p12.maxPerStageDescriptorUpdateAfterBindSampledImages < 2 * ntex ||
        p12.maxDescriptorSetUpdateAfterBindSampledImages < 2 * ntex ||
        p12.maxPerStageDescriptorUpdateAfterBindSamplers < GFX_POOL_SAMPLERS ||
        p12.maxDescriptorSetUpdateAfterBindSamplers < GFX_POOL_SAMPLERS) {
        rt_log("vk_texpool: the device's update-after-bind limits (%u sampled images, %u samplers per stage) are below "
               "the pool's: pool-indexed texture arrays keep the draw's slots",
               p12.maxPerStageDescriptorUpdateAfterBindSampledImages, p12.maxPerStageDescriptorUpdateAfterBindSamplers);
        return -1;
    }
    VkDescriptorBindingFlags bf = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                                  VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
    VkDescriptorBindingFlags flags[GFX_POOL_BINDINGS] = {bf, bf, bf};
    VkDescriptorSetLayoutBinding b[GFX_POOL_BINDINGS] = {
        {GFX_POOL_SAMP, VK_DESCRIPTOR_TYPE_SAMPLER, GFX_POOL_SAMPLERS, VK_SHADER_STAGE_ALL, NULL},
        {GFX_POOL_2D, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, ntex, VK_SHADER_STAGE_ALL, NULL},
        {GFX_POOL_2D_ARRAY, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, ntex, VK_SHADER_STAGE_ALL, NULL}};
    VkDescriptorSetLayoutBindingFlagsCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
                                                      .bindingCount = GFX_POOL_BINDINGS, .pBindingFlags = flags};
    VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .pNext = &fi,
                                          .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
                                          .bindingCount = GFX_POOL_BINDINGS, .pBindings = b};
    VkDescriptorSetLayoutCreateInfo ei = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_SAMPLER, GFX_POOL_SAMPLERS},
                                     {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2 * ntex}};
    VkDescriptorPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                     .flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT, .maxSets = 1,
                                     .poolSizeCount = 2, .pPoolSizes = sizes};
    if (vkCreateDescriptorSetLayout(c->device, &li, NULL, &T.layout) != VK_SUCCESS ||
        vkCreateDescriptorSetLayout(c->device, &ei, NULL, &T.empty) != VK_SUCCESS ||
        vkCreateDescriptorPool(c->device, &pi, NULL, &T.pool) != VK_SUCCESS) {
        rt_log("vk_texpool: the pool set could not be created: pool-indexed texture arrays keep the draw's slots");
        return -1;
    }
    VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = T.pool,
                                      .descriptorSetCount = 1, .pSetLayouts = &T.layout};
    if (vkAllocateDescriptorSets(c->device, &ai, &T.set) != VK_SUCCESS) {
        rt_log("vk_texpool: the pool set could not be allocated: pool-indexed texture arrays keep the draw's slots");
        return -1;
    }
    T.ntex = (int)ntex;
    T.reg = calloc(ntex, sizeof *T.reg), T.img = calloc(ntex, sizeof *T.img), T.dirty = calloc(ntex, 1);
    T.v2d = calloc(ntex, sizeof *T.v2d), T.v2da = calloc(ntex, sizeof *T.v2da);
    if (!T.reg || !T.img || !T.dirty || !T.v2d || !T.v2da) rt_fatal("vk_texpool: out of memory");
    const char *e = cfg_get("TOTK_TPOOL_MS");
    int ms = e ? atoi(e) : 0;
    T.budget_ns = (uint64_t)(ms > 0 ? ms : 2) * 1000000ull;
    rt_log("vk_texpool: the texture pool's set holds %d texture ids and %d sampler ids (set %d)", T.ntex,
           GFX_POOL_SAMPLERS, GFX_POOL_SET);
    return 1;
}

static int ready(void) {
    int s = __atomic_load_n(&T.state, __ATOMIC_ACQUIRE);
    if (s) return s > 0;
    plat_mutex_lock(&init_lock);
    if (!T.state) {
        int r = setup();
        if (r) __atomic_store_n(&T.state, r, __ATOMIC_RELEASE);
    }
    plat_mutex_unlock(&init_lock);
    return T.state > 0;
}

int vk_tpool_available(void) { return ready(); }
VkDescriptorSetLayout vk_tpool_layout(void) { return ready() ? T.layout : VK_NULL_HANDLE; }
VkDescriptorSetLayout vk_tpool_empty_layout(void) { return ready() ? T.empty : VK_NULL_HANDLE; }

// Writes of one update, kept per binding and flushed in batches. Consecutive elements of a binding go out as one
// write: OoT registers about a hundred slots again every frame, mostly neighbours (its render targets of
// the frame), and each element as a write struct of its own was some 160 structs a frame for the driver to walk.
#define WB 512
static struct { VkDescriptorImageInfo info[WB]; uint32_t at[WB]; int n; } PB[3];   // samplers, 2D views, 2D-array views
static const uint32_t pb_binding[3] = {GFX_POOL_SAMP, GFX_POOL_2D, GFX_POOL_2D_ARRAY};

static void flush(void) {
    static VkWriteDescriptorSet w[3 * WB];
    uint64_t st = sec_begin();
    uint32_t nw = 0, nd = 0;
    for (int b = 0; b < 3; b++) {
        for (int i = 0; i < PB[b].n;) {
            int j = i + 1;
            while (j < PB[b].n && PB[b].at[j] == PB[b].at[j - 1] + 1) j++;
            w[nw++] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = T.set,
                                             .dstBinding = pb_binding[b], .dstArrayElement = PB[b].at[i],
                                             .descriptorCount = (uint32_t)(j - i),
                                             .descriptorType = b ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLER,
                                             .pImageInfo = &PB[b].info[i]};
            i = j;
        }
        nd += (uint32_t)PB[b].n;
        PB[b].n = 0;
    }
    if (nw) vkUpdateDescriptorSets(vk_ctx()->device, nw, w, 0, NULL);
    if (st) sec_end(SEC_TP_PUT, st), sec_count(SEC_TP_DESC, nd);
    T.written += nd;
}

static void put(uint32_t binding, uint32_t at, VkImageView view, VkSampler samp) {
    int b = binding == GFX_POOL_SAMP ? 0 : binding == GFX_POOL_2D ? 1 : 2;
    if (PB[b].n == WB) flush();
    PB[b].info[PB[b].n] = (VkDescriptorImageInfo){samp, view, VK_IMAGE_LAYOUT_GENERAL};
    PB[b].at[PB[b].n++] = at;
}

// Every entry as a stand-in (first update).
static void fill_dummies(void) {
    vk_exec_pool_dummies(&T.dummy2d, &T.dummy2da, &T.dummys);
    VkDescriptorImageInfo *ii = malloc(sizeof *ii * (size_t)T.ntex);
    if (!ii) rt_fatal("vk_texpool: out of memory");
    VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = T.set,
                              .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = ii};
    for (int i = 0; i < T.ntex; i++) ii[i] = (VkDescriptorImageInfo){VK_NULL_HANDLE, T.dummy2d, VK_IMAGE_LAYOUT_GENERAL};
    w.dstBinding = GFX_POOL_2D, w.descriptorCount = (uint32_t)T.ntex;
    vkUpdateDescriptorSets(vk_ctx()->device, 1, &w, 0, NULL);
    for (int i = 0; i < T.ntex; i++) ii[i].imageView = T.dummy2da, T.v2d[i] = T.dummy2d, T.v2da[i] = T.dummy2da;
    w.dstBinding = GFX_POOL_2D_ARRAY;
    vkUpdateDescriptorSets(vk_ctx()->device, 1, &w, 0, NULL);
    for (int i = 0; i < GFX_POOL_SAMPLERS; i++) ii[i] = (VkDescriptorImageInfo){T.dummys, VK_NULL_HANDLE, 0}, T.samp[i] = T.dummys;
    w.dstBinding = GFX_POOL_SAMP, w.descriptorCount = GFX_POOL_SAMPLERS, w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    vkUpdateDescriptorSets(vk_ctx()->device, 1, &w, 0, NULL);
    free(ii);
    T.samp_gen = vk_sampler_gen() - 1;   // the samplers are read at the first update
}

void vk_tpool_image_gone(VkImage image) {
    if (__atomic_load_n(&T.state, __ATOMIC_ACQUIRE) <= 0 || T.gone_all) return;
    if (T.ngone == T.cgone) {
        uint32_t c = T.cgone ? T.cgone * 2 : 64;
        VkImage *g = c <= 65536 ? realloc(T.gone, sizeof *g * c) : NULL;
        if (!g) {
            T.gone_all = 1;
            return;
        }
        T.gone = g, T.cgone = c;
    }
    T.gone[T.ngone++] = image;
}

void vk_tpool_slot_dirty(int id) {
    if (__atomic_load_n(&T.state, __ATOMIC_ACQUIRE) <= 0 || id < 0 || id >= T.ntex) return;
    T.dirty[id] = 1;
    T.dirty_any = 1;
}

// Marks the slots whose image went. Texture lock held.
static void take_gone(int n) {
    if (!T.ngone && !T.gone_all) return;
    if (T.gone_all) {
        for (int id = 0; id < n; id++)
            if (T.img[id]) T.dirty[id] = 1;
    } else {
        uint32_t cap = 64;
        while (cap < T.ngone * 2) cap *= 2;
        VkImage *set = calloc(cap, sizeof *set);   // open addressing; images are never VK_NULL_HANDLE
        if (!set) {
            for (int id = 0; id < n; id++)
                if (T.img[id]) T.dirty[id] = 1;
        } else {
            for (uint32_t i = 0; i < T.ngone; i++) {
                uint32_t h = (uint32_t)(((uint64_t)T.gone[i] * 0x9E3779B97F4A7C15ull) >> 40) & (cap - 1);
                while (set[h] && set[h] != T.gone[i]) h = (h + 1) & (cap - 1);
                set[h] = T.gone[i];
            }
            for (int id = 0; id < n; id++) {
                VkImage im = T.img[id];
                if (!im) continue;
                uint32_t h = (uint32_t)(((uint64_t)im * 0x9E3779B97F4A7C15ull) >> 40) & (cap - 1);
                while (set[h] && set[h] != im) h = (h + 1) & (cap - 1);
                if (set[h]) T.dirty[id] = 1;
            }
            free(set);
        }
    }
    T.ngone = 0, T.gone_all = 0;
}

static int pump_ms(void) {       // TOTK_TPOOL_PUMP_MS, 0 off
    static int ms = -1;
    if (ms < 0) { const char *e = cfg_get("TOTK_TPOOL_PUMP_MS"); ms = e && *e ? atoi(e) : 8; }
    return ms;
}
static int pumping;              // update() runs from vk_tpool_pump: its own budget
static int update_stopped;       // the last update() left slots for a later one
static uint64_t last_bind_frame; // the frame of the last pool bind

// One slot: its image and views now, or the stand-in.
static void resolve_slot(int id, uint32_t reg) {
    VkImageView a = T.dummy2d, b = T.dummy2da;
    VkImage im = VK_NULL_HANDLE;
    if (reg) {
        view_t v;
        int hv;
        uint64_t sec1 = sec_begin();
        const VkTexImage *img = vk_texture_pool_slot_locked(id, &v, &hv);
        sec_end(SEC_TP_RESOLVE, sec1);
        sec_end(T.img[id] ? SEC_TP_OLD : SEC_TP_NEW, sec1);
        if (sec1) sec_count(SEC_TP_SLOTS, 1);
        // TOTK_TPOOL_HIDE=<classes> (a test): the pool shows the stand-in for textures of these classes
        // (vk_texture_pool_class), to see on screen which classes shaders read through the pool. What they do not read
        // may be evicted while registered (vk_texture.c evictable).
        static int hide = -1;
        if (hide < 0) { const char *e = cfg_get("TOTK_TPOOL_HIDE"); hide = e && *e ? atoi(e) : 0; }
        // the class bits 1, 2 and 4 name a class to hide; with 8 in the mask, of those classes only the textures a draw
        // has bound (8 alone: every such texture)
        int cls = img && hide ? vk_texture_pool_class(img) : 0;
        if (img && hide && (hide & 8 ? (cls & 8) && (!(hide & 7) || (cls & hide & 7)) : cls & hide)) {
            im = img->image;
            img = NULL;
        }
        if (img) {
            if (sec1 && img->layers > 1 && (img->format == VK_FORMAT_BC5_UNORM_BLOCK || img->format == VK_FORMAT_BC5_SNORM_BLOCK ||
                                            img->format == VK_FORMAT_BC6H_UFLOAT_BLOCK || img->format == VK_FORMAT_BC6H_SFLOAT_BLOCK ||
                                            img->format == VK_FORMAT_BC7_UNORM_BLOCK || img->format == VK_FORMAT_BC7_SRGB_BLOCK))
                sec_count(SEC_TP_ARRAY, 1);   // the classes the texture arrays read
            sec1 = sec_begin();
            VkImageView x = vk_exec_pool_view(img, &v, hv, 3), y = vk_exec_pool_view(img, &v, hv, 0x103);
            sec_end(SEC_TP_VIEWS, sec1);
            if (x) a = x;
            if (y) b = y;
            im = img->image;
        }
        T.resolved++;
    }
    if (__builtin_expect(vk_live_check_on(), 0)) {   // DEBUG-LIVECHECK: what the slot is given
        if (a != T.dummy2d) vk_live_check_use((uint64_t)(uintptr_t)a, "the texture pool's slot (resolved, 2D view)", (uint64_t)id);
        if (b != T.dummy2da) vk_live_check_use((uint64_t)(uintptr_t)b, "the texture pool's slot (resolved, array view)", (uint64_t)id);
    }
    if (a != T.v2d[id]) T.v2d[id] = a, put(GFX_POOL_2D, (uint32_t)id, a, VK_NULL_HANDLE);
    if (b != T.v2da[id]) T.v2da[id] = b, put(GFX_POOL_2D_ARRAY, (uint32_t)id, b, VK_NULL_HANDLE);
    T.reg[id] = reg, T.img[id] = im, T.dirty[id] = 0;
}

static uint64_t knob_ms(const char *name, int def) {
    const char *e = cfg_get(name);
    int ms = e && *e ? atoi(e) : def;
    return (uint64_t)(ms > 0 ? ms : 2) * 1000000ull;
}

// The slots to resolve, within the time budget; a slot left over is next.
static void update(void) {
    uint64_t sec0 = sec_begin();
    T.upd_fence = vk_exec_recording_fence();
    static int filled;
    if (!filled) fill_dummies(), filled = 1;
    vk_texture_lock();
    uint32_t sg = vk_sampler_gen();
    if (sg != T.samp_gen) {
        T.samp_gen = sg;
        for (int i = 0; i < GFX_POOL_SAMPLERS; i++) {
            VkSampler s = vk_sampler_by_id_locked(i);
            if (!s) s = T.dummys;
            if (s != T.samp[i]) T.samp[i] = s, put(GFX_POOL_SAMP, (uint32_t)i, VK_NULL_HANDLE, s);
        }
    }
    int n = vk_texture_slot_count();
    if (n > T.ntex) n = T.ntex;
    // Whether every slot is looked at: only when a slot was registered since the last complete look (the registry's
    // generation, read before the scans: a registration during them is seen next time), an image went, or the last
    // update left slots over. Otherwise only the sweep's slots are: the two scans read the registration of every
    // slot, 28,000 in OoT, 0.4 to 0.5 ms of every frame's replay with nothing to find.
    static uint32_t scan_gen;
    static int scan_n;
    uint32_t reg_gen = vk_texture_slots_gen();
    int full = reg_gen != scan_gen || n != scan_n || T.ngone || T.gone_all || update_stopped || T.dirty_any;
    take_gone(n);
    uint64_t t0 = plat_now_ns();
    int checked = 0, stopped = 0;
    // A scene load registers thousands of slots at once; resolved 2 ms at a time, the lightmaps came in over the first seconds of
    // play and half the world was dark. While the previous update left a large backlog (TOTK_TPOOL_BURST_MIN slots, default 1024)
    // the budget is TOTK_TPOOL_BURST_MS (default 50), so they finish behind the game's fade from black. A smaller backlog
    // is the world streaming in while the player moves: the Market registers a few hundred slots within half a
    // second of running, about a tenth of them new textures to create and upload, and under the long budget those were
    // frames of 25 to 50 ms, ten in a row (2026-11-12). They get TOTK_TPOOL_MID_MS (default 4) per update instead. Slots
    // that showed an image before (a texture the game's compaction moved, a slot registered again) may be on screen and
    // would show the stand-in while they wait, so those come first and keep the long budget: they mostly find their
    // image by its content (vk_texture.c adopt_released) and cost no upload.
    static int backlog;
    static uint64_t burst_ns, mid_ns;
    static int burst_min;
    if (!burst_ns) {
        burst_ns = knob_ms("TOTK_TPOOL_BURST_MS", 50), mid_ns = knob_ms("TOTK_TPOOL_MID_MS", 4);
        const char *e = cfg_get("TOTK_TPOOL_BURST_MIN");
        burst_min = e && *e ? atoi(e) : 1024;
    }
    uint64_t pump_ns = (uint64_t)pump_ms() * 1000000ull;
    uint64_t budget_old = pumping ? pump_ns : burst_ns;
    uint64_t budget = pumping ? pump_ns : backlog >= burst_min ? burst_ns : backlog >= 16 ? mid_ns : T.budget_ns;
    uint32_t sweep_lo = n ? T.sweep % (uint32_t)n : 0, sweep_n = n < SWEEP ? (uint32_t)n : SWEEP;
    const uint64_t *regs = vk_texture_slot_regs();   // n > 0: a slot was registered, so the words exist
    for (int id = 0; id < n && full; id++) {   // the slots that showed an image
        uint32_t reg = (uint32_t)(regs[id] >> 16);
        if (!T.img[id] || (reg == T.reg[id] && !T.dirty[id])) continue;
        if ((++checked & 15) == 0 && plat_now_ns() - t0 > budget_old) {
            stopped = 1;
            break;
        }
        resolve_slot(id, reg);
    }
    uint64_t t1 = plat_now_ns();
    for (int k = 0, id = !n ? 0 : full ? (int)(T.scan % (uint32_t)n) : (int)sweep_lo, todo = full ? n : (int)sweep_n; k < todo && !stopped;
         k++, id = id + 1 == n ? 0 : id + 1) {
        uint32_t reg = (uint32_t)(regs[id] >> 16);   // the slot's registration serial; 0: never registered
        int in_sweep = (uint32_t)((uint32_t)id - sweep_lo) % (uint32_t)n < sweep_n;
        if (reg == T.reg[id] && !T.dirty[id] && !(in_sweep && reg)) continue;
        if ((++checked & 15) == 0 && plat_now_ns() - t1 > budget) {
            T.scan = (uint32_t)id;   // the rest at the next submission
            stopped = 1;
            break;
        }
        resolve_slot(id, reg);
    }
    if (full && !stopped) scan_gen = reg_gen, scan_n = n, T.dirty_any = 0;
    backlog = 0;
    if (stopped)
        for (int id = 0; id < n; id++) backlog += (uint32_t)(regs[id] >> 16) != T.reg[id] || T.dirty[id];
    update_stopped = stopped;
    if (stopped) {
        // A slot whose image went and whose turn did not come within the budget must not keep its views: they are
        // destroyed with the image once the GPU is past this submission, and the next one samples the slot. It shows
        // the stand-in until it is resolved (dirty stays set). The Market, where the game's texture
        // compaction makes and destroys about 1.5 textures a frame and 100 slots wait at any time, lost the device this
        // way in 3 of 21 two-minute runs: an occlusion query not available after its fence, then an invalid read at an
        // address in no pool buffer (VK_EXT_device_fault), with frame generation and without (2026-11-12).
        static uint64_t parked_slots;
        uint64_t before = parked_slots;
        for (int id = 0; id < n; id++) {
            if (!T.dirty[id] || !T.img[id]) continue;
            if (T.v2d[id] != T.dummy2d) T.v2d[id] = T.dummy2d, put(GFX_POOL_2D, (uint32_t)id, T.dummy2d, VK_NULL_HANDLE);
            if (T.v2da[id] != T.dummy2da) T.v2da[id] = T.dummy2da, put(GFX_POOL_2D_ARRAY, (uint32_t)id, T.dummy2da, VK_NULL_HANDLE);
            T.img[id] = VK_NULL_HANDLE;
            parked_slots++;
        }
        if (before / 4096 != parked_slots / 4096 || (!before && parked_slots))
            rt_log("vk_texpool: %llu slots so far took the stand-in while they waited for their turn after their image went",
                   (unsigned long long)parked_slots);
    }
    if (__builtin_expect(vk_live_check_on(), 0)) {   // DEBUG-LIVECHECK: what every slot shows after the update
        void vk_texture_slot_log(int id);
        for (int id = 0; id < n; id++) {
            int bad = 0;
            if (T.v2d[id] != T.dummy2d) bad |= vk_live_check_use((uint64_t)(uintptr_t)T.v2d[id], "a texture pool slot after the update (2D view)", (uint64_t)id);
            if (T.v2da[id] != T.dummy2da) bad |= vk_live_check_use((uint64_t)(uintptr_t)T.v2da[id], "a texture pool slot after the update (array view)", (uint64_t)id);
            if (T.img[id]) bad |= vk_live_check_use((uint64_t)(uintptr_t)T.img[id], "a texture pool slot after the update (its image)", (uint64_t)id);
            if (bad) {
                rt_log("LIVE CHECK: pool slot %d: dirty %d, written for serial %u, serial now %u; update %s, %u images gone and "
                       "not taken in", id, (int)T.dirty[id], T.reg[id], (uint32_t)(regs[id] >> 16), stopped ? "stopped" : "complete",
                       T.ngone);
                vk_texture_slot_log(id);
            }
        }
    }
    if (sec0 && stopped) sec_peak(SEC_TP_BACKLOG, (uint64_t)backlog);   // slots still to resolve after this update
    T.sweep = sweep_lo + sweep_n;
    flush();
    vk_texture_unlock();
    sec_end(SEC_TP_UPDATE, sec0);
}

// TOTK_SECTIONS, the 60-frame line: slots registered and those the pool has not resolved (racy reads; a diagnostic).
void vk_tpool_backlog(int *registered, int *unresolved) {
    int n = vk_texture_slot_count(), r = 0, u = 0;
    if (!T.reg) n = 0;
    else if (n > T.ntex) n = T.ntex;
    for (int id = 0; id < n; id++) {
        uint32_t reg = (uint32_t)(vk_texture_slot_token(id) >> 32);
        if (!reg) continue;
        r++;
        u += reg != T.reg[id] || T.dirty[id];
    }
    *registered = r, *unresolved = u;
}

// Submissions while no draw binds the pool (the title and the loading screen, long before the first scene frame) resolve
// the registered slots a piece at a time. The first scene frames then find them done, where they used to spend hundreds
// of milliseconds creating and uploading in plain view. The budget is what keeps the loading screen smooth; resolving
// them unbudgeted from the title on ran at 7 fps. TOTK_TPOOL_PUMP_MS: the budget per submission, 0 off.
void vk_tpool_pump(void) {
    static uint32_t done_gen;
    if (pump_ms() <= 0 || !ready()) return;
    uint64_t f = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    if (last_bind_frame && f < last_bind_frame + 30) return;   // a scene is on screen: its binds drive the updates
    uint32_t ep = vk_texture_epoch(), g = vk_texture_slots_gen();
    if (ep == T.epoch || (g == done_gen && !update_stopped)) return;
    T.epoch = ep;
    pumping = 1;
    update();
    pumping = 0;
    if (!update_stopped) done_gen = g;
}

// The submission's update of the set, which its first bind makes: done here ahead of binds that other threads record
// (vk_exec.c "Draw stretches on worker threads": the chunks' threads must find the set up to date and only bind it).
void vk_tpool_prepare(void) {
    if (!ready()) return;
    uint32_t ep = vk_texture_epoch();
    if (ep != T.epoch) {
        T.epoch = ep;
        update();
    }
}

// DEBUG-LIVECHECK: the pool is bound; the update before a submission's first bind must have been made in this
// submission (vk_sync.c vk_sync_submit has the case where it was not). Reported once a submission, with the slots
// whose views wait for a destruction that comes before this submission is done.
static void live_check_bind(void) {
    static volatile uint64_t said;
    uint64_t f = vk_exec_recording_fence();
    if (!f || f == T.upd_fence || __atomic_exchange_n(&said, f, __ATOMIC_RELAXED) == f) return;
    static volatile int32_t n_no, n_dead_subs;
    int n = vk_texture_slot_count(), dead = 0;
    if (n > T.ntex) n = T.ntex;
    vk_texture_lock();
    for (int id = 0; id < n; id++) {
        if (T.v2d[id] != T.dummy2d) dead += vk_live_check_use((uint64_t)(uintptr_t)T.v2d[id], "a texture pool slot bound without an update (2D view)", (uint64_t)id);
        if (T.v2da[id] != T.dummy2da) dead += vk_live_check_use((uint64_t)(uintptr_t)T.v2da[id], "a texture pool slot bound without an update (array view)", (uint64_t)id);
    }
    vk_texture_unlock();
    int k = plat_inc32(&n_no);
    if (dead) plat_inc32(&n_dead_subs);
    if (k <= 8 || dead || !(k & 255))
        rt_log("LIVE CHECK: the texture pool is bound in submission %llu, its last update was made in %llu: %d views in it wait "
               "for a destruction before this submission is done (%d such submissions so far, %d of them with such views; %u "
               "images gone and not taken in)", (unsigned long long)f, (unsigned long long)T.upd_fence, dead, k,
               (int)n_dead_subs, T.ngone);
}

void vk_tpool_bind(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout layout) {
    if (!ready()) return;
    last_bind_frame = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) + 1;
    uint32_t ep = vk_texture_epoch();
    // A thread that records a chunk of a draw stretch only binds. The epoch can move while a submission is being
    // recorded (another thread's submit call bumps it before it waits for the submit lock: vk_backend.c submit_sync),
    // and the update it asks for would then run here, inside a chunk: the replay thread makes it at its next bind or
    // stretch (vk_tpool_prepare).
    extern _Thread_local int vk_par_worker;
    if (ep != T.epoch && !vk_par_worker) {
        T.epoch = ep;
        update();
        static uint64_t last_log;
        uint64_t f = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
        if (f >= last_log + 1800) {
            last_log = f;
            rt_log("vk_texpool: frame %llu: %llu slots resolved, %llu descriptors written so far",
                   (unsigned long long)f, (unsigned long long)T.resolved, (unsigned long long)T.written);
        }
    }
    if (__builtin_expect(vk_live_check_on(), 0)) live_check_bind();
    vkCmdBindDescriptorSets(cb, bp, layout, GFX_POOL_SET, 1, &T.set, 0, NULL);
}

// DEBUG-WHONAMES (the device fault report): pool entries that show the image
void vk_tpool_who_names(uint64_t image) {
    if (__atomic_load_n(&T.state, __ATOMIC_ACQUIRE) <= 0 || !T.img) return;
    int n = vk_texture_slot_count(), shown = 0;
    if (n > T.ntex) n = T.ntex;
    for (int id = 0; id < n; id++)
        if ((uint64_t)(uintptr_t)T.img[id] == image && shown++ < 8)
            rt_log("  pool slot %d shows it (dirty %d, written for serial %u, serial now %u)", id, (int)T.dirty[id], T.reg[id],
                   (uint32_t)(vk_texture_slot_token(id) >> 32));
    rt_log("  pool: %d slots show it; %u images gone and not yet taken in", shown, T.ngone);
}

void vk_tpool_shutdown(void) {
    VkCtx *c = vk_ctx();
    if (T.state > 0 && c && c->device) {
        vkDestroyDescriptorPool(c->device, T.pool, NULL);
        vkDestroyDescriptorSetLayout(c->device, T.layout, NULL);
        vkDestroyDescriptorSetLayout(c->device, T.empty, NULL);
    }
    free(T.reg), free(T.img), free(T.dirty), free(T.v2d), free(T.v2da), free(T.gone);
    memset(&T, 0, sizeof T);
}
