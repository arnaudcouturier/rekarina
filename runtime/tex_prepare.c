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
//
// Since 1.2.8 the runtime reads the archives itself (rekarina.exe --scan-textures, bntx_walk: the reading of the tool):
// the game's entry step runs it as a child beside its pipeline builds to fill what the cache lacks before play,
// counted in the step's picture (tex_prepare_entry_start), and the launcher's background work runs it while the game
// is closed. A walk that read every archive writes tex-cache/prepared.txt with what the cache was made from
// (TEX_STAMP); a later start that finds the same stamp reads no archive at all.
#include <fcntl.h>
#include <stdio.h>
#ifdef _WIN32
#include <io.h>
#endif
#include <stdlib.h>
#include <string.h>

#include <zstd.h>

#include "cfg.h"
#include "fs.h"
#include "host.h"
#include "nvn_tex.h"
#include "platform/platform.h"
#include "prep.h"
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
    vk_mem_collect_idle();   // the decoder's buffers of the batch (no frames here collect them)
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

// ---- one walk over records: the batch, the reduced shapes, the counts
typedef struct {
    TxJob *jobs;
    int nj, check_all, shapes;
    uint64_t batch, t0, said;
    const char *dir;
} TxRun;

// What the cache holds on the disk against its limit (a listing of the folder: done once a walk is going to write).
static void run_count_disk(TxRun *x) {
    const char *mb = cfg_get("REKARINA_TEX_CACHE_MB");
    disk_limit = (int64_t)(mb && atoi(mb) >= 256 ? atoi(mb) : 14336) << 20;
    disk_bytes = 0;
    plat_file_list(x->dir, ".bc3", count_one, (void *)x->dir);
    plat_file_list(x->dir, ".bc1", count_one, (void *)x->dir);
}

// The cache's folder and which shapes this machine prepares (count: and what the folder holds against its limit); 0 when
// this device and these settings keep no texture cache (native ASTC, the CPU decoder, the cache off).
static int run_open(TxRun *x, int count) {
    memset(x, 0, sizeof *x);
    x->dir = vk_texture_cache_dir();
    if (!x->dir || !vk_astc_gpu() || !vk_astc_bc()) return 0;
    x->check_all = cfg_get("REKARINA_PREPARE_TEXTURES_CHECK") && atoi(cfg_get("REKARINA_PREPARE_TEXTURES_CHECK")) == 1;
    if (count) run_count_disk(x);
    // The reduced-quality shapes: "all" always, "full" never; by default when play could pick half or quarter here
    // (REKARINA_TEX_QUALITY says so, or on auto a card of less than 10 GiB, whose free memory can fall under 7.25 GiB).
    const char *lv = cfg_get("REKARINA_PREPARE_TEXTURES_LEVELS"), *q = cfg_get("REKARINA_TEX_QUALITY");
    uint64_t vram = 0;
    for (uint32_t h = 0; h < vk_ctx()->mem_props.memoryHeapCount; h++)
        if (vk_ctx()->mem_props.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) vram += vk_ctx()->mem_props.memoryHeaps[h].size;
    x->shapes = lv && !strcmp(lv, "all") ? 1 : lv && !strcmp(lv, "full") ? 0
              : q && (!strcmp(q, "half") || !strcmp(q, "quarter")) ? 1 : q && !strcmp(q, "full") ? 0 : vram < (10ull << 30);
    x->jobs = calloc(BATCH_JOBS, sizeof *x->jobs);
    x->t0 = x->said = plat_now_ms();
    return x->jobs != NULL;
}

// One texture as the archive holds it: the full one, then the half and the quarter shapes texture quality loads.
static void run_feed(TxRun *x, const TxRec *r, const uint8_t *storage) {
    S.read++;
    TxJob *j = &x->jobs[x->nj];
    memset(j, 0, sizeof *j);
    if (one(r, storage, x->dir, j, x->check_all)) x->batch += j->rsize, x->nj++;
    if (x->shapes) {   // half, then quarter (texq_shape: quarter is the half of the half)
        nvn_texdesc d, d2, d4;
        memset(&d, 0, sizeof d);
        d.target = (int)r->target, d.format = (int)r->format, d.width = r->width, d.height = r->height;
        d.depth = umax(r->depth, 1), d.levels = umax(r->levels, 1), d.pkg_block_h = (int)r->layout0 + 1;
        const uint8_t *m2, *m4;
        uint64_t s2, s4;
        TexVkFormat f;
        if (tex_vk_format(d.format, &f) && f.astc_bc && shape1(&d, storage, r->size, &d2, &m2, &s2)) {
            if (x->nj == BATCH_JOBS || x->batch >= BATCH_BYTES) batch_finish(x->jobs, &x->nj), x->batch = 0;
            memset(&x->jobs[x->nj], 0, sizeof x->jobs[x->nj]);
            if (one_desc(&d2, m2, s2, x->dir, &x->jobs[x->nj], x->check_all)) x->batch += x->jobs[x->nj].rsize, x->nj++;
            if (shape1(&d2, m2, s2, &d4, &m4, &s4)) {
                if (x->nj == BATCH_JOBS || x->batch >= BATCH_BYTES) batch_finish(x->jobs, &x->nj), x->batch = 0;
                memset(&x->jobs[x->nj], 0, sizeof x->jobs[x->nj]);
                if (one_desc(&d4, m4, s4, x->dir, &x->jobs[x->nj], x->check_all)) x->batch += x->jobs[x->nj].rsize, x->nj++;
            }
        }
    }
    // the batch's storage is copied to its staging buffers (one_desc): the caller's may go after this call
    if (x->nj == BATCH_JOBS || x->batch >= BATCH_BYTES) batch_finish(x->jobs, &x->nj), x->batch = 0;
}

static void run_say(TxRun *x, int force) {
    uint64_t now = plat_now_ms();
    if (!force && now - x->said < 2000) return;
    x->said = now;
    printf("prepare-textures: %llu read, %llu prepared (%.2f GiB), %llu there already, %llu left out, %.0f s\n",
           (unsigned long long)S.read, (unsigned long long)S.prepared, S.bytes / 1073741824.0,
           (unsigned long long)S.existed, (unsigned long long)(S.skipped + S.size_mismatch), (now - x->t0) / 1000.0);
    fflush(stdout);
}

static void run_close(TxRun *x) {
    batch_finish(x->jobs, &x->nj);
    free(x->jobs), x->jobs = NULL;
    printf("prepare-textures: done: %llu textures read, %llu prepared (%.2f GiB; %llu opaque ones as BC1), %llu there already, "
           "%llu checked equal, %llu checked different, %llu left out (%llu of another layout size than the runtime's), %llu "
           "past the folder's limit of %.2f GiB (it holds %.2f GiB), %.0f s\n",
           (unsigned long long)S.read, (unsigned long long)S.prepared, S.bytes / 1073741824.0, (unsigned long long)S.bc1,
           (unsigned long long)S.existed, (unsigned long long)S.same, (unsigned long long)S.differ,
           (unsigned long long)(S.skipped + S.size_mismatch), (unsigned long long)S.size_mismatch,
           (unsigned long long)S.over, disk_limit / 1073741824.0, disk_bytes / 1073741824.0, (plat_now_ms() - x->t0) / 1000.0);
    rt_log("tex_prepare: %llu textures read, %llu prepared (%.2f GiB), %llu there already, %llu left out, %llu past the "
           "folder's limit, %.0f s", (unsigned long long)S.read, (unsigned long long)S.prepared, S.bytes / 1073741824.0,
           (unsigned long long)S.existed, (unsigned long long)(S.skipped + S.size_mismatch), (unsigned long long)S.over,
           (plat_now_ms() - x->t0) / 1000.0);
}

// ---- the archives of the dump (the reading of tools/oot_prepare_textures.py textures(), in C)
// The NVN format of a BNTX ASTC format (vk_formats.c tex_vk_format: 0x79.. UNORM, 0x87.. sRGB), 0 for any other.
static int bntx_nvn_format(uint32_t fmt) {
    uint32_t t = fmt >> 8, v = fmt & 0xff;
    if (t < 0x2d || t > 0x3a || (v != 1 && v != 6)) return 0;
    return (int)((v == 6 ? 0x87 : 0x79) + (t - 0x2d));
}

static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

// Each ASTC texture of a BNTX file: the "Ounc" section's table at +0x20 (count +4, table pointer +8); BRTI: dim +0x11,
// levels u16 +0x16, format +0x1C, width/height/depth/layers +0x24.., packaged layout +0x34, storage bytes +0x50, pointer
// to the level offsets +0x70.
static void bntx_walk(TxRun *x, const uint8_t *data, uint64_t n) {
    if (n < 0x30 || memcmp(data, "BNTX", 4)) return;
    uint32_t count = rd32(data + 0x24);
    uint64_t table = rd64(data + 0x28);
    for (uint32_t i = 0; i < count && table + 8ull * i + 8 <= n; i++) {
        uint64_t o = rd64(data + table + 8ull * i);
        if (o + 0x78 > n || memcmp(data + o, "BRTI", 4)) continue;
        uint8_t dim = data[o + 0x11];
        uint16_t levels;
        memcpy(&levels, data + o + 0x16, 2);
        uint32_t fmt = rd32(data + o + 0x1c), layers = rd32(data + o + 0x30);
        int32_t w, h, d;
        memcpy(&w, data + o + 0x24, 4), memcpy(&h, data + o + 0x28, 4), memcpy(&d, data + o + 0x2c, 4);
        uint8_t layout0 = data[o + 0x34];
        uint32_t size = rd32(data + o + 0x50);
        uint64_t ptrs = rd64(data + o + 0x70);
        int nf = bntx_nvn_format(fmt);
        if (!nf || !(ptrs > 0 && ptrs + 8 <= n) || w <= 0 || h <= 0) continue;
        uint64_t first = rd64(data + ptrs);
        if (first > n || size > n - first) continue;
        TxRec r = {{'T', 'X', 'P', 'R'}, (uint32_t)nf, 1, (uint32_t)w, (uint32_t)h, 1, levels, layout0, size};
        if (dim == 3 && d > 1) r.target = 2, r.depth = (uint32_t)d;   // NVN_TEXTURE_TARGET_3D
        else if (layers > 1) r.target = 4, r.depth = layers;          // NVN_TEXTURE_TARGET_2D_ARRAY
        run_feed(x, &r, data + first);
    }
}

static int ends_with(const char *s, const char *tail) {
    size_t a = strlen(s), b = strlen(tail);
    return a >= b && !strcmp(s + a - b, tail);
}

// What the cache's files were made from, as tex-cache/prepared.txt holds it: the walk's own version, the dump (path,
// size, last write), the shapes and the BC1 rule. Empty when the dump is not known.
// The dump's path goes in in one form, absolute with '/': the game opens it as its settings give it ("D:/x.nup"), the
// entry step's child as REKARINA_ROMFS, which prep_child_paths makes absolute with the system's own separators
// ("D:\x.nup"). The same file came out as two texts, so the stamp the child wrote after its walk never matched the one
// the game wanted, and every start walked the 21,658 archives again (since 1.2.8's child walk).
#define TEX_STAMP "rekarina texture cache 1"
static void stamp_of(const TxRun *x, char *out, size_t n) {
    uint64_t size = 0, wt = 0;
    out[0] = 0;
    fs_init();   // the dump opened (once per process)
    const char *image = romfs_image_path();
    if (!image || !romfs_image_identity(&size, &wt)) return;
    char path[PLAT_PATH_MAX * 2];
    if (!plat_full_path(image, path, sizeof path)) snprintf(path, sizeof path, "%s", image);
    for (char *c = path; *c; c++)
        if (*c == '\\') *c = '/';
    snprintf(out, n, "%s|%s|%llu|%llu|shapes %d|bc1 %d\n", TEX_STAMP, path, (unsigned long long)size,
             (unsigned long long)wt, x->shapes, rt_knob("TEX_BC1", "1") != 0);
}

static void stamp_path(const TxRun *x, char *out, size_t n) { snprintf(out, n, "%s/prepared.txt", x->dir); }

static int stamp_current(const TxRun *x) {
    char want[1400], path[700];
    stamp_of(x, want, sizeof want);
    stamp_path(x, path, sizeof path);
    uint64_t n = 0;
    char *f = (char *)plat_file_read(path, &n);
    int same = want[0] && f && n == strlen(want) && !memcmp(f, want, (size_t)n);
    free(f);
    return same;
}

// The archives of the RomFS index (the dump's rows, not the mods' overlays): their rows, and how many.
static uint64_t *archives(size_t *count) {
    fs_init();
    uint64_t n = romfs_file_count(), *rows = n ? malloc(n * sizeof *rows) : NULL;
    size_t k = 0;
    for (uint64_t i = 0; rows && i < n; i++) {
        const char *name = romfs_entry(i, NULL, NULL);
        if (name && (ends_with(name, ".bntx.zs") || ends_with(name, ".bntx"))) rows[k++] = i;
    }
    *count = k;
    return rows;
}

// Every archive of rows, in order: read, inflated, walked. stop: set from another thread to end after the archive in
// hand; part: the entry step's picture counts each archive there (-1: none); say: the background job's count line.
// 1 when every archive was walked.
static char scan_stop[PLAT_PATH_MAX * 2 + 48];   // the stop file (--scan-textures: stop_file_of)
static int scan_rows(TxRun *x, const uint64_t *rows, size_t n, volatile int32_t *stop, int part, int say) {
    uint64_t said = 0, bytes = 0, all = 0;
    const char *stop_file = scan_stop[0] ? scan_stop : NULL;
    for (size_t k = 0; say && k < n; k++) {
        uint64_t sz = 0;
        romfs_entry(rows[k], NULL, &sz);
        all += sz;
    }
    for (size_t k = 0; k < n; k++) {
        if (stop && __atomic_load_n(stop, __ATOMIC_ACQUIRE)) return 0;
        if (stop_file && *stop_file && plat_file_exists(stop_file)) {
            printf("prepare-textures: %s appeared: stopped after %zu of %zu archives\n", stop_file, k, n);
            return 0;
        }
        uint64_t size = 0;
        uint8_t *raw = romfs_entry_read(rows[k], &size), *data = raw;
        const char *name = romfs_entry(rows[k], NULL, NULL);
        uint64_t len = size;
        if (raw && ends_with(name, ".zs")) {
            unsigned long long c = ZSTD_getFrameContentSize(raw, (size_t)size);
            data = c && c != ZSTD_CONTENTSIZE_ERROR && c != ZSTD_CONTENTSIZE_UNKNOWN && c < (2ull << 30) ? malloc((size_t)c) : NULL;
            len = data ? ZSTD_decompress(data, (size_t)c, raw, (size_t)size) : 0;
            if (data && (ZSTD_isError(len) || len != c)) free(data), data = NULL;
        }
        if (data) bntx_walk(x, data, len);
        if (data != raw) free(data);
        free(raw);
        if (part >= 0) prep_part_step_work(part, 1, (int64_t)size);   // (its pace by the archives' bytes)
        bytes += size;
        // installer/oot.py TEXTURE_ARCHIVES and the entry step (entry_line) read it
        if (say && (k + 1 == n || plat_now_ms() - said >= 1000)) {
            said = plat_now_ms();
            printf("textures: %zu of %zu archives, %llu of %llu bytes\n", k + 1, n, (unsigned long long)bytes,
                   (unsigned long long)all);
            fflush(stdout);
        }
    }
    return 1;
}

// A walk that read every archive: the stamp, so that the next start reads none. A disk that stopped the cache at its
// limit stamps it too (the rest is made in play, as the cache's limit says).
static void stamp_write(const TxRun *x) {
    char want[1400], path[700];
    stamp_of(x, want, sizeof want);
    stamp_path(x, path, sizeof path);
    if (want[0] && !plat_file_write_atomic(path, want, strlen(want))) rt_log("tex_prepare: cannot write %s", path);
}

// ---- the entry step's part (vk_pipeline.c vk_pipeline_prewarm)
static struct {
    PlatThread *th;
    TxRun x;
    size_t n;
    volatile unsigned long long reported;   // archives the child has walked (entry_line)
} TE;

// The child's count lines (scan_rows) into the picture's: archives for the count, bytes for the pace.
static void entry_line(const char *line, void *ctx) {
    (void)ctx;
    unsigned long long k, n, b, all;
    if (sscanf(line, "textures: %llu of %llu archives, %llu of %llu bytes", &k, &n, &b, &all) != 4) return;
    static unsigned long long k0, b0;
    if (k > k0) prep_part_step_work(PREP_PART_TEX, (int64_t)(k - k0), (int64_t)(b > b0 ? b - b0 : 0)), k0 = k, b0 = b;
    TE.reported = k;
}

static void entry_main(void *arg) {
    (void)arg;
    // in a child of this exe (prep.c prep_child): in the game's process the walk's buffers and images were never given
    // back (11.6 GiB in 11,648 allocations after the walk)
    if (!prep_child("--scan-textures", entry_line, NULL)) rt_log("tex_prepare: the texture walk did not start");
    if (TE.reported < TE.n) prep_part_step(PREP_PART_TEX, (int64_t)(TE.n - TE.reported));   // (the count ends whole)
    rt_log("tex_prepare: --scan-textures walked %llu of %zu archives", (unsigned long long)TE.reported, TE.n);
}

// The textures the cache lacks: counted (an archive an item, its bytes for the pace, PREP_PART_TEX) and walked by
// --scan-textures from now on, beside the entry step's builds; nothing when the cache's stamp is current or the device
// keeps no cache. REKARINA_ENTRY_TEXTURES=0 leaves them to play (each made at its first use).
void tex_prepare_entry_start(void) {
    if (TE.th || !rt_knob("ENTRY_TEXTURES", "1") || !run_open(&TE.x, 0)) return;
    free(TE.x.jobs), TE.x.jobs = NULL;   // (the child has its own)
    if (stamp_current(&TE.x)) {
        rt_log("tex_prepare: the texture cache holds every texture of this dump (%s/prepared.txt)", TE.x.dir);
        return;
    }
    uint64_t *rows = archives(&TE.n);
    uint64_t bytes = 0;
    for (size_t k = 0; k < TE.n; k++) {
        uint64_t sz = 0;
        romfs_entry(rows[k], NULL, &sz);
        bytes += sz;
    }
    free(rows);
    if (!TE.n) return;
    // (until the walk has a pace: 40 MB of archives a second; this machine's NVMe with an RTX 5070 did 10-90)
    prep_part_work(PREP_PART_TEX, (int64_t)bytes, 1.0 / 40e6);
    prep_part_total(PREP_PART_TEX, (int64_t)TE.n, 0);
    rt_log("tex_prepare: %zu texture archives (%.2f GiB) to walk for the texture cache (%s)", TE.n, bytes / 1073741824.0,
           TE.x.dir);
    TE.th = plat_thread_new(entry_main, NULL, "entry textures");
    if (!TE.th) prep_part_step(PREP_PART_TEX, (int64_t)TE.n);
}

void tex_prepare_entry_join(void) {
    if (TE.th) plat_thread_join(TE.th), TE.th = NULL;
}

// --scan-textures: its stdin is a pipe its starter holds (the game's entry step, the launcher); its end stops the walk
// after the archive in hand, as the stop file does.
static volatile int32_t scan_stopped;
static void scan_stdin_watch(void *arg) {
    (void)arg;
    char b[64];   // (the descriptor, not stdio: a stdio read would hold stdin's lock against the exit's flush)
    while (plat_stdin_read(b, sizeof b) > 0) {}
    __atomic_store_n(&scan_stopped, 1, __ATOMIC_RELEASE);
}

// rekarina.exe --scan-textures: the archives of the dump walked into the texture cache (the reading of
// tools/oot_prepare_textures.py), a count line a second ("textures: N of M archives, B of T bytes"), the stamp when
// every archive was walked; nothing when the stamp is current. REKARINA_WARM_STOP names a stop file (the launcher's
// background job; never its file by default: Play leaves it behind, and the game's entry step starts this mode).
int tex_prepare_scan_main(void) {
    if (!vk_init()) {
        fprintf(stderr, "scan-textures: no Vulkan device\n");
        return 1;
    }
    const char *stop = cfg_get("REKARINA_WARM_STOP");
    if (stop && *stop) snprintf(scan_stop, sizeof scan_stop, "%s", stop);
    plat_thread_new(scan_stdin_watch, NULL, "scan input");   // (not joined: it ends with stdin)
    TxRun x;
    if (!run_open(&x, 1)) {
        printf("scan-textures: this device and these settings keep no texture cache: nothing to prepare\n"
               "textures: 0 of 0 archives, 0 of 0 bytes\n");
        return 0;
    }
    if (stamp_current(&x) && !x.check_all) {
        free(x.jobs);
        printf("scan-textures: the cache holds every texture of this dump (prepared.txt)\n"
               "textures: 0 of 0 archives, 0 of 0 bytes\n");
        return 0;
    }
    size_t n = 0;
    uint64_t *rows = archives(&n);
    int all = scan_rows(&x, rows, n, &scan_stopped, -1, 1);
    free(rows);
    run_close(&x);
    vk_mem_footprint_log("after the texture walk");   // (a few hundred MiB: it ran beside the game's entry step)
    if (all && n) stamp_write(&x);
    return all ? 0 : 3;
}

int tex_prepare_main(void) {
    if (!vk_init()) {
        fprintf(stderr, "prepare-textures: no Vulkan device\n");
        return 1;
    }
    TxRun x;
    if (!run_open(&x, 1)) {   // native ASTC, a CPU decode or the cache off: play keeps no cache
        printf("prepare-textures: this device and these settings keep no texture cache (native ASTC, the CPU decoder or "
               "tex_cache off): nothing to prepare\n");
        return 0;
    }
    printf("prepare-textures: %s (REKARINA_PREPARE_TEXTURES_LEVELS: all, full)\n",
           x.shapes ? "full textures and the half and quarter shapes texture quality loads" : "full textures");
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
#endif
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
        run_feed(&x, &r, storage);
        free(storage);
        run_say(&x, 0);
    }
    run_close(&x);
    return 0;
}
