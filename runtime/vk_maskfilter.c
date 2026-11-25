// The shadow mask's temporal filter (OoT): REKARINA_MASK_FILTER=<percent> (the history's weight, 1 to 95; 80 when unset,
// the default since 2026-11-24; 0: off, as before, a test value).
//
// Every sun shadow the player sees goes through one screen-space mask before the lighting reads it: an RGBA8 target of
// the scene's size, MIN-blended by full-screen passes, first those with the scene depth attached (the grass's and the
// characters' square maps), last the sun's own pass without it (the three cascades and the static map); green holds
// the hard shadows, blue the grass's soft one (docs/oot-notes.md, "foliage shadows"). The game refits its cascades to
// the view every frame, so while the camera turns or the hero walks each leaf's shadow lands on another sub-texel phase
// of the cascades' grid in every frame and the far ground shimmers (cascades 1 and 2 carry a perspective warp and
// cannot be held still in the world without losing resolution). The console's temporal upscaler hides it; base mode
// shows it as it is.
//
// After the sun's pass of a frame (vk_exec.c vk_exec_pass_end, before the frame dump's hooks) a compute pass
// (runtime/mask_filter_spv.h, tools/make_mask_filter.py) takes each pixel's surface to last frame's picture by the
// camera's motion (the pixel's depth and this frame's view-projection against last frame's, both from the game's scene
// context block, as the motion vectors of DLAA take them: vk_dlaa.c mv_pass), reads last frame's filtered mask there,
// holds it within the range of this frame's 3x3 neighbourhood (a shadow that moves, or ground that comes into view,
// takes this frame's value at once: no trail) and mixes it in by the weight. Green and blue only; red and alpha pass
// through. The result is copied over the mask and kept as the next frame's history. A frame without a camera or with a
// new mask size runs unfiltered (and starts the history again).
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "gfx.h"
#include "host.h"
#include "nvn_api.h"
#include "platform/platform.h"
#include "vk.h"
#include "vk_dlaa.h"
#include "vk_exec.h"
#include "vk_rec.h"
#include "mask_filter_spv.h"

#define OCAM_BLOCK 0x3500u   // OoT's scene context block (the scene draws' c3), as vk_dlaa.c takes it
#define OCAM_BYTES 0xb0u     // its rows 0-10: the view and the projection
#define CAMS 8               // scene context blocks counted in a frame

typedef struct { float m[16]; float p[4]; } FilterPush;   // rows of the reprojection; size, weight, history valid

static struct {
    int on;               // -1 not read, 0 off, 1 on
    float weight;
    PlatMutex lock;       // the camera counts (draws on the chunk threads too)
    uint64_t cam_frame;   // the present the counts belong to
    uint64_t cam_addr[CAMS];
    uint32_t cam_n[CAMS];
    int ncam;
    uint64_t mask_frame;  // the present whose first mask pass was seen (the replay thread only from here on)
    VkImage mask;         // that frame's mask image
    VkExecTarget depth;   // the depth attached to it: the scene's
    int done;             // the frame's filter ran (or was given up)
    int ready;            // the pipeline: 0 not made, 1 made, -1 failed
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkPipeline pipe;
    VkSampler lin, near;
    uint32_t w, h;        // the history's size
    VkImage hist[2];
    VmaAllocation hmem[2];
    VkImageView hview[2];
    int cur, hist_ok, fresh;
    double prev_vp[16];
    int prev_ok;
    uint64_t ran, skipped;
} M = {.on = -1, .lock = PLAT_MUTEX_INIT};

static int filter_on(void) {
    if (__builtin_expect(M.on >= 0, 1)) return M.on;
    // On by default in OoT: it halves the far ground's shadow shimmer in motion at no measurable frame rate
    // (docs/oot-notes.md, "the shadow mask filtered over time"); 0 turns it off.
#ifdef GAME_OOT
    const char *def = "80";
#else
    const char *def = "0";
#endif
    const char *e = cfg_env("REKARINA_MASK_FILTER");
    int pct = atoi(e && *e ? e : def);
    M.on = pct > 0;
    if (M.on) {
        M.weight = (pct > 95 ? 95 : pct) / 100.0f;
        rt_log("vk: the shadow mask is filtered over time, the history's weight %.2f (REKARINA_MASK_FILTER=0 turns it off)",
               M.weight);
    } else {
        rt_log("vk: the shadow mask is not filtered over time (REKARINA_MASK_FILTER=%s)", e && *e ? e : def);
    }
    return M.on;
}

// vk_exec_draw.c, each draw (any thread): the scene context block the scene's draws into its depth use, counted for the
// frame (the one most of them use is the camera, as vk_dlaa.c vk_dlaa_draw picks it).
void vk_maskfilter_draw(VkExec *ex) {
    if (!filter_on()) return;
    const VkExecTarget *z = &ex->depth;
    const p_bind_buf *b = &ex->ubo[0][0];   // c3
    if (!z->image || z->format != VK_FORMAT_D32_SFLOAT || z->width < 640 || b->size != OCAM_BLOCK || !b->addr ||
        !ex->color[0].image)
        return;
    uint64_t f = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) + 1;
    plat_mutex_lock(&M.lock);
    if (M.cam_frame != f) M.cam_frame = f, M.ncam = 0;
    int i = 0;
    while (i < M.ncam && M.cam_addr[i] != b->addr) i++;
    if (i == M.ncam && M.ncam < CAMS) M.cam_addr[M.ncam] = b->addr, M.cam_n[M.ncam++] = 0;
    if (i < M.ncam) M.cam_n[i]++;
    plat_mutex_unlock(&M.lock);
}

static int make_pipeline(void) {
    if (M.ready) return M.ready > 0;
    M.ready = -1;
    const VkCtx *c = vk_ctx();
    VkDescriptorSetLayoutBinding b[4] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                           .bindingCount = 4, .pBindings = b};
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(FilterPush)};
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR,
                              .minFilter = VK_FILTER_LINEAR, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                              .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    VkSamplerCreateInfo sn = si;
    sn.magFilter = sn.minFilter = VK_FILTER_NEAREST;
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof mask_filter_spv,
                                   .pCode = mask_filter_spv};
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateDescriptorSetLayout(c->device, &dci, NULL, &M.dsl) != VK_SUCCESS) goto fail;
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                     .pSetLayouts = &M.dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    if (vkCreatePipelineLayout(c->device, &li, NULL, &M.layout) != VK_SUCCESS ||
        vkCreateSampler(c->device, &si, NULL, &M.lin) != VK_SUCCESS ||
        vkCreateSampler(c->device, &sn, NULL, &M.near) != VK_SUCCESS ||
        vkCreateShaderModule(c->device, &mi, NULL, &m) != VK_SUCCESS)
        goto fail;
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                      .layout = M.layout};
    VkResult r = vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &ci, NULL, &M.pipe);
    vkDestroyShaderModule(c->device, m, NULL);
    if (r != VK_SUCCESS) goto fail;
    return (M.ready = 1);
fail:
    rt_log("vk: the shadow mask's filter could not be set up: the mask stays unfiltered");
    return 0;
}

// The two history images at the mask's size (made again when it changes; the old ones go once the GPU is past them).
static int history(uint32_t w, uint32_t h) {
    if (M.w == w && M.h == h && M.hist[0]) return 1;
    const VkCtx *c = vk_ctx();
    for (int i = 0; i < 2; i++) {
        if (M.hview[i]) vk_defer_view(M.hview[i]);
        if (M.hist[i]) vk_defer_image(M.hist[i], M.hmem[i]);
        M.hview[i] = VK_NULL_HANDLE, M.hist[i] = VK_NULL_HANDLE, M.hmem[i] = NULL;
    }
    M.w = M.h = 0, M.hist_ok = 0;
    for (int i = 0; i < 2; i++) {
        VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                                .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
                                .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                         VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        if (vmaCreateImage(c->vma, &ii, &ai, &M.hist[i], &M.hmem[i], NULL) != VK_SUCCESS) return 0;
        VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = M.hist[i],
                                    .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
                                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        if (vkCreateImageView(c->device, &vi, NULL, &M.hview[i]) != VK_SUCCESS) return 0;
    }
    M.w = w, M.h = h, M.fresh = 1;
    rt_log("vk: the shadow mask's filter keeps its history at %ux%u", w, h);
    return 1;
}

static void barrier_all(VkCommandBuffer cb) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
}

// This frame's camera: the reprojection from this frame's clip space to last frame's into out (rows); 0 when there is
// none (no camera this frame, or none last frame).
static int camera(float out[16]) {
    uint64_t f = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) + 1, addr = 0;
    uint32_t best = 0;
    plat_mutex_lock(&M.lock);
    if (M.cam_frame == f)
        for (int i = 0; i < M.ncam; i++)
            if (M.cam_n[i] > best) best = M.cam_n[i], addr = M.cam_addr[i];
    plat_mutex_unlock(&M.lock);
    const float *rows = addr ? (const float *)vk_usnap_bytes(addr, OCAM_BYTES) : NULL;
    if (!rows) {
        M.prev_ok = 0;
        return 0;
    }
    double vp[16], inv[16];
    vk_dlaa_ocam_vp(rows, vp);
    int ok = M.prev_ok && vk_dlaa_inv4(vp, inv);
    if (ok)
        for (int r = 0; r < 4; r++)
            for (int k = 0; k < 4; k++) {
                double v = 0;
                for (int j = 0; j < 4; j++) v += M.prev_vp[r * 4 + j] * inv[j * 4 + k];
                out[r * 4 + k] = (float)v;
            }
    memcpy(M.prev_vp, vp, sizeof vp);
    M.prev_ok = 1;
    return ok;
}

static void run(VkExec *ex, const VkExecTarget *t) {
    VkCommandBuffer cb = ex->cb;
    const VkTexImage *mi = t->img;
    float m[16];
    int have = camera(m);
    if (!mi || !(mi->usage & VK_IMAGE_USAGE_SAMPLED_BIT) || t->is3d || t->blocks || !M.depth.image || !M.depth.img ||
        M.depth.width != t->width || M.depth.height != t->height || !make_pipeline() || !history(t->width, t->height)) {
        static int told;
        if (!told++)
            rt_log("vk: the shadow mask's filter left a frame unfiltered (mask %ux%u, sampled %d, depth %ux%u)", t->width,
                   t->height, mi && (mi->usage & VK_IMAGE_USAGE_SAMPLED_BIT) ? 1 : 0, M.depth.width, M.depth.height);
        M.hist_ok = 0, M.skipped++;
        return;
    }
    if (M.fresh) {   // new history images: UNDEFINED -> GENERAL
        VkImageMemoryBarrier2 ib[2];
        for (int i = 0; i < 2; i++)
            ib[i] = (VkImageMemoryBarrier2){.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                            .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
                                            .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                            .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = M.hist[i],
                                            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 2, .pImageMemoryBarriers = ib};
        vkCmdPipelineBarrier2(cb, &di);
        M.fresh = 0;
    }
    VkImageView cur = vk_exec_target_view(mi, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT,
                                          t->level, t->base_layer, 1, 0);
    VkImageView zv = vk_exec_target_view(M.depth.img, M.depth.format, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_DEPTH_BIT,
                                         M.depth.level, M.depth.base_layer, 1, 0);
    int o = M.cur, h = o ^ 1;   // write hist[o], read last frame's hist[h]
    VkDescriptorImageInfo ci = {M.near, cur, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo hi = {M.lin, M.hview[h], VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo zi = {M.near, zv, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo oi = {VK_NULL_HANDLE, M.hview[o], VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet wr[4] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ci},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &hi},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 2, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &zi},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 3, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &oi}};
    FilterPush pc = {{0}, {(float)t->width, (float)t->height, M.weight, have && M.hist_ok ? 1.0f : 0.0f}};
    if (have) memcpy(pc.m, m, sizeof m);
    barrier_all(cb);   // the sun's pass wrote the mask; last frame's copy out of hist[h] is done
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, M.pipe);
    rec_push(vk_pipeline_push_fn(), cb, VK_PIPELINE_BIND_POINT_COMPUTE, M.layout, 0, 4, wr);
    vkCmdPushConstants(cb, M.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
    vkCmdDispatch(cb, (t->width + 7) / 8, (t->height + 7) / 8, 1);
    barrier_all(cb);
    VkImageCopy r = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                     .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, t->level, t->base_layer, 1},
                     .extent = {t->width, t->height, 1}};
    vkCmdCopyImage(cb, M.hist[o], VK_IMAGE_LAYOUT_GENERAL, t->image, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    barrier_all(cb);   // the lighting reads the mask
    ex->recorded += 7;
    M.cur = h, M.hist_ok = 1, M.ran++;
    if (M.ran == 1 || M.ran % 6000 == 0)
        rt_log("vk: the shadow mask's filter ran %llu frames (%llu left unfiltered), %ux%u", (unsigned long long)M.ran,
               (unsigned long long)M.skipped, t->width, t->height);
}

// vk_exec.c vk_exec_pass_end, the replay thread, after the pass ended: a pass into one RGBA8 target of the scene depth's
// size with that depth attached is the mask's (the first of the frame names the mask and the depth); a later pass into
// the same image without a depth is the sun's, the mask's last: the filter runs after it.
void vk_maskfilter_pass_end(VkExec *ex) {
    if (!filter_on()) return;
    const VkExecTarget *c = &ex->color[0], *z = &ex->depth;
    if (ex->pass_ncolor != 1 || !c->image || c->format != VK_FORMAT_R8G8B8A8_UNORM || c->width < 640 || c->samples > 1)
        return;
    uint64_t f = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) + 1;
    if (z->image && z->width == c->width && z->height == c->height && z->format == VK_FORMAT_D32_SFLOAT) {
        if (M.mask_frame != f) M.mask_frame = f, M.mask = c->image, M.depth = *z, M.done = 0;
        return;
    }
    if (z->image || M.mask_frame != f || c->image != M.mask || M.done) return;
    M.done = 1;
    run(ex, c);
}
