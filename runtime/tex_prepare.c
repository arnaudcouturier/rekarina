// rekarina.exe --prepare-textures: the texture cache (vk_texture.c, "the texture cache") filled for every ASTC texture of
// the game before play, by the decoder and BC3 encoder play uses (vk_astc_record), so a texture's first use reads its
// blocks from the disk instead of decoding them on the GPU in the frame that draws it: with an empty cache a new
// player's first visit of the field had 1 % low 15-24 fps and 74-136 frames over 25 ms in 46 s (2026-11-16).
//
// Input: records on stdin from tools/oot_prepare_textures.py, which reads the BNTX archives of the player's dump:
// "TXPR", u32 NVN format, target, width, height, depth (layers of an array), levels, packaged layout byte 0, then u64
// storage bytes and the storage as the game loads it into memory. Each record is keyed as play keys it
// (vk_texture_content_hash of the storage, tex_cache_path's name: the hash, the format, the size, levels x layers),
// skipped when that file is there already (so a stopped preparation goes on where it was), else deswizzled per level
// and layer (nvn_deswizzle), decoded and encoded on the GPU, read back and written as the file play would write. The
// storage's size must equal the runtime's own layout of the descriptor (nvn_tex_layout), or the record is left out.
// REKARINA_PREPARE_TEXTURES_CHECK=1 makes it encode the records whose file is there and compare the bytes (a test of
// the descriptor read from the archives against what play wrote).
// Texture quality (vk_texture.c "texture quality": half and quarter, which auto picks with less than 7.25 or 6.2 GiB of
// video memory free) loads an ASTC texture of 512 texels and more from its second level, of 1,024 and more at quarter
// from its third: another storage range under another key. REKARINA_PREPARE_TEXTURES_LEVELS=all prepares those shapes
// too (the same rules as texq_shape1: a 2D texture of two levels or more whose later levels keep their tiles and
// offsets); the default prepares the full textures.
#include <fcntl.h>
#include <stdio.h>
#ifdef _WIN32
#include <io.h>
#endif
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "nvn_tex.h"
#include "platform/platform.h"
#include "tex.h"
#include "vk.h"

typedef struct {
    char magic[4];
    uint32_t format, target, width, height, depth, levels, layout0;
    uint64_t size;
} TxRec;

typedef struct {
    char path[600];
    VkBuffer sb, rb;
    VmaAllocation sa, ra;
    uint8_t *rmap;
    uint64_t rsize;
    VkImage img;
    VmaAllocation ia;
    int check;   // compare with the file instead of writing it
    int bc1_ok;  // opaque blocks may go to the .bc1 name (REKARINA_TEX_BC1)
    int dead;    // nothing was recorded: the buffers and the image only go
} TxJob;

#define BATCH_JOBS 48
#define BATCH_BYTES (192ull << 20)

static struct {
    uint64_t read, prepared, existed, same, differ, skipped, bytes, size_mismatch, over, bc1;
} S;

// The folder's limit as play keeps it (vk_texture.c tex_cache_dir, REKARINA_TEX_CACHE_MB, default 14336 MiB): play deletes
// the files over it at start, so nothing is written past it here.
static int64_t disk_bytes, disk_limit;
static int count_one(const char *name, void *ctx) {
    char path[700];
    snprintf(path, sizeof path, "%s/%s", (const char *)ctx, name);
    int64_t n = plat_file_size(path);
    if (n > 0) disk_bytes += n;
    return 1;
}

static void batch_finish(TxJob *jobs, int *n) {
    VkCtx *c = vk_ctx();
    vk_upload_flush(1);
    for (int i = 0; i < *n; i++) {
        TxJob *j = &jobs[i];
        vmaInvalidateAllocation(c->vma, j->ra, 0, VK_WHOLE_SIZE);
        if (j->dead) {
        } else {
            // opaque blocks are kept as their BC1 colour halves under the .bc1 name, as play writes them (vk_texture.c,
            // the BC1 section); REKARINA_TEX_BC1=0 keeps every file BC3
            size_t pl = strlen(j->path);
            if (j->bc1_ok && vk_texture_bc3_to_bc1(j->rmap, j->rsize, j->rmap)) j->rsize /= 2, j->path[pl - 1] = '1', S.bc1++;
            if (j->check) {
                uint64_t fn = 0;
                uint8_t *f = plat_file_read(j->path, &fn);
                if (f && fn == j->rsize && !memcmp(f, j->rmap, (size_t)fn)) S.same++;
                else S.differ++;
                free(f);
            } else if (disk_bytes + (int64_t)j->rsize > disk_limit) {
                S.over++;
            } else if (plat_file_write_atomic(j->path, j->rmap, (size_t)j->rsize)) {
                S.prepared++, S.bytes += j->rsize, disk_bytes += (int64_t)j->rsize;
            } else {
                S.skipped++;
            }
        }
        vmaDestroyBuffer(c->vma, j->sb, j->sa);
        vmaDestroyBuffer(c->vma, j->rb, j->ra);
        vmaDestroyImage(c->vma, j->img, j->ia);
    }
    *n = 0;
}

static void image_general(VkCommandBuffer cb, VkImage img) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
                               .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT, .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                               .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .image = img,
                               .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0,
                                                    VK_REMAINING_ARRAY_LAYERS}};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &d);
}

static uint32_t umax(uint32_t a, uint32_t b) { return a > b ? a : b; }

// The texture d of storage at mem as texture quality shapes it, one level smaller (vk_texture.c texq_shape1); 0 when
// the shape does not apply.
static int shape1(const nvn_texdesc *d, const uint8_t *mem, uint64_t size, nvn_texdesc *d2, const uint8_t **mem2,
                  uint64_t *size2) {
    if (d->target != NVN_TEXTURE_TARGET_2D || d->levels < 2 || d->samples > 1 || (d->width < 512 && d->height < 512)) return 0;
    if (nvn_tex_is_linear(d)) return 0;
    *d2 = *d;
    d2->width = umax(d->width >> 1, 1), d2->height = umax(d->height >> 1, 1), d2->levels = d->levels - 1;
    uint64_t off1 = nvn_tex_level_offset(d, 1);
    if (!off1) return 0;
    for (unsigned l = 0; l < d2->levels; l++) {
        unsigned th, td, th2, td2;
        nvn_tex_level_tiles(d, l + 1, &th, &td);
        nvn_tex_level_tiles(d2, l, &th2, &td2);
        if (th != th2 || td != td2 || nvn_tex_level_offset(d2, l) != nvn_tex_level_offset(d, l + 1) - off1) return 0;
    }
    nvn_texlayout L2;
    nvn_tex_layout(d2, &L2);
    if (off1 + L2.size > size) return 0;
    *mem2 = mem + off1, *size2 = L2.size;
    return 1;
}

static int one_desc(const nvn_texdesc *dd, const uint8_t *storage, uint64_t ssize, const char *dir, TxJob *j, int check_all);

// One record: 1 queued into the batch, 0 left out (counted). *shapes: the reduced-quality forms to queue next.
static int one(const TxRec *r, const uint8_t *storage, const char *dir, TxJob *j, int check_all) {
    TexVkFormat f;
    if (!tex_vk_format((int)r->format, &f) || !f.astc_bc || !f.astc_bw) return S.skipped++, 0;
    nvn_texdesc d;
    memset(&d, 0, sizeof d);
    d.target = (int)r->target, d.format = (int)r->format, d.width = r->width, d.height = r->height;
    d.depth = umax(r->depth, 1), d.levels = umax(r->levels, 1), d.pkg_block_h = (int)r->layout0 + 1;
    nvn_texlayout L;
    nvn_tex_layout(&d, &L);
    if (L.size != r->size) return S.size_mismatch++, 0;
    return one_desc(&d, storage, r->size, dir, j, check_all);
}

static int one_desc(const nvn_texdesc *dd, const uint8_t *storage, uint64_t ssize, const char *dir, TxJob *j, int check_all) {
    nvn_texdesc d = *dd;
    TexVkFormat f;
    tex_vk_format(d.format, &f);
    nvn_texlayout L;
    nvn_tex_layout(&d, &L);
    // levels and layers as create_tex makes the image, the order and sizes as upload stages the regions
    int is3d = d.target == NVN_TEXTURE_TARGET_3D;
    uint32_t w0 = umax(d.width, 1), h0 = umax(d.height, 1), dep = is3d ? d.depth : 1;
    uint32_t layers = (d.target == NVN_TEXTURE_TARGET_2D_ARRAY || d.target == NVN_TEXTURE_TARGET_CUBEMAP ||
                       d.target == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY) ? L.layers : 1;
    uint32_t levels = umax(d.levels, 1), most = 1;
    for (uint32_t s = umax(umax(w0, h0), dep); s > 1; s >>= 1) most++;
    if (levels > most) levels = most;
    uint64_t hash = vk_texture_content_hash(storage, ssize);
    snprintf(j->path, sizeof j->path, "%s/%016llx-%x-%ux%ux%u-%ux%u.bc3", dir, (unsigned long long)hash, (unsigned)d.format,
             d.width, d.height, d.depth, levels, layers);
    j->check = plat_file_exists(j->path);
    j->bc1_ok = rt_knob("TEX_BC1", "1") != 0;
    if (!j->check && j->bc1_ok) {   // kept as BC1 (opaque): the same name with .bc1
        size_t pl = strlen(j->path);
        j->path[pl - 1] = '1';
        j->check = plat_file_exists(j->path);
        j->path[pl - 1] = '3';
    }
    if (j->check && !check_all) return S.existed++, 0;
    if (!j->check && disk_bytes >= disk_limit) return S.over++, 0;
    int bw, bh, bpb = nvn_fmt_bpb(d.format, &bw, &bh);
    uint32_t n = levels * layers, k = 0;
    VkAstcRegion *ar = calloc(n, sizeof *ar);
    uint64_t total = 0, bc_total = 0;
    for (uint32_t lv = 0; lv < levels; lv++)
        for (uint32_t ly = 0; ly < layers; ly++) {
            uint32_t w = umax(w0 >> lv, 1), h = umax(h0 >> lv, 1), sl = is3d ? umax(dep >> lv, 1) : 1;
            uint64_t raw = (uint64_t)(w + (uint32_t)bw - 1) / (uint32_t)bw * (uint32_t)bpb * ((h + (uint32_t)bh - 1) / (uint32_t)bh) * sl;
            ar[k++] = (VkAstcRegion){.src = total, .w = w, .h = h, .slices = sl, .level = lv, .layer = ly};
            total += (raw + 15) & ~15ull;
            bc_total += vk_astc_bc_bytes(w, h, sl);
        }
    uint8_t *stg = vk_staging(total, &j->sb, &j->sa);
    for (k = 0; k < n; k++) {
        uint32_t w = ar[k].w, rb = (w + (uint32_t)bw - 1) / (uint32_t)bw * (uint32_t)bpb;
        if (nvn_deswizzle(&d, ar[k].level, ar[k].layer, storage, stg + ar[k].src, rb)) {
            free(ar);
            vmaDestroyBuffer(vk_ctx()->vma, j->sb, j->sa);
            return S.skipped++, 0;
        }
    }
    j->rmap = vk_staging(bc_total, &j->rb, &j->ra);
    j->rsize = bc_total;
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = is3d ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D,
                            .format = f.vk, .extent = {w0, h0, dep}, .mipLevels = levels, .arrayLayers = layers,
                            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(vk_ctx()->vma, &ci, &ai, &j->img, &j->ia, NULL) != VK_SUCCESS) {
        free(ar);
        vmaDestroyBuffer(vk_ctx()->vma, j->sb, j->sa);
        vmaDestroyBuffer(vk_ctx()->vma, j->rb, j->ra);
        return S.skipped++, 0;
    }
    VkCommandBuffer cb = vk_upload_begin();
    image_general(cb, j->img);
    vk_astc_bc_readback(j->rb);
    int ok = vk_astc_record(cb, j->sb, total, ar, (int)n, f.astc_bw, f.astc_bh, f.astc_srgb, 1, j->img,
                            VK_IMAGE_ASPECT_COLOR_BIT, VK_NULL_HANDLE);
    vk_upload_end();
    free(ar);
    if (!ok) S.skipped++, j->dead = 1;   // nothing recorded reads them: they go after the next flush
    return 1;
}

int tex_prepare_main(void) {
    if (!vk_init()) {
        fprintf(stderr, "prepare-textures: no Vulkan device\n");
        return 1;
    }
    const char *dir = vk_texture_cache_dir();
    if (!dir || !vk_astc_gpu() || !vk_astc_bc()) {   // native ASTC, a CPU decode or the cache off: play keeps no cache
        printf("prepare-textures: this device and these settings keep no texture cache (native ASTC, the CPU decoder or "
               "tex_cache off): nothing to prepare\n");
        return 0;
    }
    int check_all = cfg_get("REKARINA_PREPARE_TEXTURES_CHECK") && atoi(cfg_get("REKARINA_PREPARE_TEXTURES_CHECK")) == 1;
    const char *mb = cfg_get("REKARINA_TEX_CACHE_MB");
    disk_limit = (int64_t)(mb && atoi(mb) >= 256 ? atoi(mb) : 14336) << 20;
    plat_file_list(dir, ".bc3", count_one, (void *)dir);
    plat_file_list(dir, ".bc1", count_one, (void *)dir);
    // The reduced-quality shapes: "all" always, "full" never; by default when play could pick half or quarter here
    // (REKARINA_TEX_QUALITY says so, or on auto a card of less than 10 GiB, whose free memory can fall under 7.25 GiB).
    const char *lv = cfg_get("REKARINA_PREPARE_TEXTURES_LEVELS"), *q = cfg_get("REKARINA_TEX_QUALITY");
    uint64_t vram = 0;
    for (uint32_t h = 0; h < vk_ctx()->mem_props.memoryHeapCount; h++)
        if (vk_ctx()->mem_props.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) vram += vk_ctx()->mem_props.memoryHeaps[h].size;
    int shapes = lv && !strcmp(lv, "all") ? 1 : lv && !strcmp(lv, "full") ? 0
               : q && (!strcmp(q, "half") || !strcmp(q, "quarter")) ? 1 : q && !strcmp(q, "full") ? 0 : vram < (10ull << 30);
    printf("prepare-textures: %s (%.1f GiB of video memory; REKARINA_PREPARE_TEXTURES_LEVELS: all, full)\n",
           shapes ? "full textures and the half and quarter shapes texture quality loads" : "full textures",
           vram / 1073741824.0);
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
#endif
    TxJob *jobs = calloc(BATCH_JOBS, sizeof *jobs);
    int nj = 0;
    uint64_t batch = 0, t0 = plat_now_ms(), said = t0;
    TxRec r;
    while (fread(&r, sizeof r, 1, stdin) == 1) {
        if (memcmp(r.magic, "TXPR", 4) || r.size > (4ull << 30)) {
            fprintf(stderr, "prepare-textures: bad record\n");
            break;
        }
        uint8_t *storage = malloc(r.size ? r.size : 1);
        if (!storage || fread(storage, 1, r.size, stdin) != r.size) {
            free(storage);
            break;
        }
        S.read++;
        TxJob *j = &jobs[nj];
        memset(j, 0, sizeof *j);
        if (one(&r, storage, dir, j, check_all)) batch += j->rsize, nj++;
        if (shapes) {   // half, then quarter (texq_shape: quarter is the half of the half)
            nvn_texdesc d, d2, d4;
            memset(&d, 0, sizeof d);
            d.target = (int)r.target, d.format = (int)r.format, d.width = r.width, d.height = r.height;
            d.depth = umax(r.depth, 1), d.levels = umax(r.levels, 1), d.pkg_block_h = (int)r.layout0 + 1;
            const uint8_t *m2, *m4;
            uint64_t s2, s4;
            TexVkFormat f;
            if (tex_vk_format(d.format, &f) && f.astc_bc && shape1(&d, storage, r.size, &d2, &m2, &s2)) {
                if (nj == BATCH_JOBS || batch >= BATCH_BYTES) batch_finish(jobs, &nj), batch = 0;
                memset(&jobs[nj], 0, sizeof jobs[nj]);
                if (one_desc(&d2, m2, s2, dir, &jobs[nj], check_all)) batch += jobs[nj].rsize, nj++;
                if (shape1(&d2, m2, s2, &d4, &m4, &s4)) {
                    if (nj == BATCH_JOBS || batch >= BATCH_BYTES) batch_finish(jobs, &nj), batch = 0;
                    memset(&jobs[nj], 0, sizeof jobs[nj]);
                    if (one_desc(&d4, m4, s4, dir, &jobs[nj], check_all)) batch += jobs[nj].rsize, nj++;
                }
            }
        }
        free(storage);
        if (nj == BATCH_JOBS || batch >= BATCH_BYTES) batch_finish(jobs, &nj), batch = 0;
        uint64_t now = plat_now_ms();
        if (now - said >= 2000) {
            said = now;
            printf("prepare-textures: %llu read, %llu prepared (%.2f GiB), %llu there already, %llu left out, %.0f s\n",
                   (unsigned long long)S.read, (unsigned long long)S.prepared, S.bytes / 1073741824.0,
                   (unsigned long long)S.existed, (unsigned long long)(S.skipped + S.size_mismatch), (now - t0) / 1000.0);
            fflush(stdout);
        }
    }
    batch_finish(jobs, &nj);
    free(jobs);
    printf("prepare-textures: done: %llu textures read, %llu prepared (%.2f GiB; %llu opaque ones as BC1), %llu there already, "
           "%llu checked equal, %llu checked different, %llu left out (%llu of another layout size than the runtime's), %llu "
           "past the folder's limit of %.2f GiB (it holds %.2f GiB), %.0f s\n",
           (unsigned long long)S.read, (unsigned long long)S.prepared, S.bytes / 1073741824.0, (unsigned long long)S.bc1,
           (unsigned long long)S.existed, (unsigned long long)S.same, (unsigned long long)S.differ,
           (unsigned long long)(S.skipped + S.size_mismatch), (unsigned long long)S.size_mismatch,
           (unsigned long long)S.over, disk_limit / 1073741824.0, disk_bytes / 1073741824.0, (plat_now_ms() - t0) / 1000.0);
    return 0;
}
