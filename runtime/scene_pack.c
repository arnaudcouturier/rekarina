// The port's scene resolution on a mod's Bootup pack (OoT).
//
// Pack/Bootup.Nin_Ounce_NVN2.pack.zs holds System/GameGraphics/ModelSceneExtensionParam, whose ResolutionReduceScale
// values set the scene as a share of the 2560x1440 picture: 0.75 at best, and palettes that step it down to 0.33 while
// the GPU looks loaded. On PC the load the game measures includes the replay's latency, so the port's romfs-patches serve
// the pack with every one of them at 1.0 (tools/make_oot_prompts.py scene_scale_pack; every prompt folder carries the same
// pack). A mod that ships its own Bootup pack (model mods carry it for their actor data) took the place of the port's,
// and the scene went soft again. romfs.c scene_overlay serves the mod's pack with the same values instead
// (scene_pack_build).
//
// The pack is a SARC archive compressed with the dictionary of Pack/ZsDic.pack.zs its frame names. A float of a BYML is
// stored in its node, so the new values take the bytes of the old ones: the pack keeps its size and the resource size
// table its entry.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <zstd.h>

#include "host.h"

#define MEMBER "System/GameGraphics/ModelSceneExtensionParam/Default.game__gfx__ModelSceneExtensionParam.bgyml"
#define KEY "ResolutionReduceScale"
#define SCALE 1.0f              // make_oot_prompts.py --scene-scale, which the setup leaves at its default
#define MAX_RAW (64u << 20)
#define MAX_VALUES 256

static int u32_at(const uint8_t *p, size_t n, size_t off, uint32_t *v) {
    if (off > n || n - off < 4) return 0;
    memcpy(v, p + off, 4);
    return 1;
}

// Calls each(name, data, size, ctx) for every member of the SARC archive p[0..n) until it returns nonzero; that value,
// 0 when no call did, -1 when p is not an archive this reads.
static int sarc_each(const uint8_t *p, size_t n, int (*each)(const char *, const uint8_t *, size_t, void *), void *ctx) {
    uint32_t data;
    uint16_t head, count, bom;
    if (n < 0x14 || memcmp(p, "SARC", 4)) return -1;
    memcpy(&head, p + 4, 2);
    memcpy(&bom, p + 6, 2);
    if (bom != 0xfeff || !u32_at(p, n, 12, &data) || head + 12u > n || memcmp(p + head, "SFAT", 4)) return -1;
    memcpy(&count, p + head + 6, 2);
    size_t names = head + 12 + (size_t)count * 16;
    if (names + 8 > n || memcmp(p + names, "SFNT", 4)) return -1;
    names += 8;
    for (unsigned i = 0; i < count; i++) {
        uint32_t attr, lo, hi;
        size_t e = head + 12 + (size_t)i * 16;
        if (!u32_at(p, n, e + 4, &attr) || !u32_at(p, n, e + 8, &lo) || !u32_at(p, n, e + 12, &hi)) return -1;
        size_t s = names + (size_t)(attr & 0xffffff) * 4;
        if (!(attr >> 24) || s >= n || !memchr(p + s, 0, n - s) || lo > hi || data + (size_t)hi > n) continue;
        int r = each((const char *)p + s, p + data + lo, hi - lo, ctx);
        if (r) return r;
    }
    return 0;
}

typedef struct { const char *name; size_t at, size; const uint8_t *base; } Find;

static int find_cb(const char *name, const uint8_t *data, size_t size, void *ctx) {
    Find *f = ctx;
    if (strcmp(name, f->name)) return 0;
    f->at = (size_t)(data - f->base);
    f->size = size;
    return 1;
}

typedef struct { unsigned id; ZSTD_DDict *dd; const uint8_t *dict; size_t dict_n; } DictFind;

static int dict_cb(const char *name, const uint8_t *data, size_t size, void *ctx) {
    (void)name;
    DictFind *f = ctx;
    ZSTD_DDict *dd = ZSTD_createDDict(data, size);
    if (dd && ZSTD_getDictID_fromDDict(dd) == f->id) {
        f->dd = dd;
        f->dict = data;
        f->dict_n = size;
        return 1;
    }
    ZSTD_freeDDict(dd);
    return 0;
}

// The offsets of the float values (type 0xD2) stored under KEY anywhere in the BYML p[0..n): dictionaries (0xC1),
// arrays (0xC0) and the hash-keyed dictionaries of BYML v7 (0x20), at most MAX_VALUES. -1 when p is not one.
typedef struct {
    const uint8_t *p;
    size_t n, kt;
    uint32_t nkeys, seen[4096];   // seen: the containers walked, each once
    size_t at[MAX_VALUES];
    int count, nodes;
} Walk;

static int walk_node(Walk *w, uint32_t off, int depth);

static int walk_value(Walk *w, uint8_t type, size_t at, const char *key, int depth) {
    uint32_t v;
    if (type == 0xC0 || type == 0xC1 || type == 0x20) return u32_at(w->p, w->n, at, &v) ? walk_node(w, v, depth + 1) : -1;
    if (type == 0xD2 && key && !strcmp(key, KEY)) {
        if (at + 4 > w->n) return -1;
        for (int i = 0; i < w->count; i++)
            if (w->at[i] == at) return 0;   // a node several parents share is met again: one value
        if (w->count >= MAX_VALUES) return -1;
        w->at[w->count++] = at;
    }
    return 0;
}

static int walk_node(Walk *w, uint32_t off, int depth) {
    uint32_t head;
    for (int i = 0; i < w->nodes && i < 4096; i++)
        if (w->seen[i] == off) return 0;   // shared by several parents: walked already
    if (depth > 32 || w->nodes >= 4096 || !u32_at(w->p, w->n, off, &head)) return -1;
    w->seen[w->nodes++] = off;
    uint32_t type = head & 0xff, count = head >> 8;
    for (uint32_t i = 0; i < count; i++) {
        int r = 0;
        if (type == 0xC1) {   // dictionary: {key index:24, type:8, value:32} each
            uint32_t e;
            if (!u32_at(w->p, w->n, off + 4 + 8 * (size_t)i, &e) || (e & 0xffffff) >= w->nkeys) return -1;
            uint32_t ko;
            if (!u32_at(w->p, w->n, w->kt + 4 + 4 * (size_t)(e & 0xffffff), &ko) || w->kt + ko >= w->n ||
                !memchr(w->p + w->kt + ko, 0, w->n - w->kt - ko))
                return -1;
            r = walk_value(w, (uint8_t)(e >> 24), off + 8 + 8 * (size_t)i, (const char *)w->p + w->kt + ko, depth);
        } else if (type == 0xC0) {   // array: the types, padded to 4, then the values
            size_t types = off + 4, values = off + 4 + ((count + 3) & ~3u);
            if (types + count > w->n) return -1;
            r = walk_value(w, w->p[types + i], values + 4 * (size_t)i, NULL, depth);
        } else if (type == 0x20) {   // hash dictionary: {hash, value} each, then the types
            size_t types = off + 4 + 8 * (size_t)count;
            if (types + count > w->n) return -1;
            r = walk_value(w, w->p[types + i], off + 8 + 8 * (size_t)i, NULL, depth);
        } else {
            return 0;   // a leaf container this walk does not need
        }
        if (r < 0) return r;
    }
    return 0;
}

static int byml_values(Walk *w, const uint8_t *p, size_t n) {
    uint32_t kt, root, head;
    memset(w, 0, sizeof *w);
    w->p = p, w->n = n;
    if (n < 16 || memcmp(p, "YB", 2) || !u32_at(p, n, 4, &kt) || !u32_at(p, n, 12, &root) || !u32_at(p, n, kt, &head) ||
        (head & 0xff) != 0xC2)
        return -1;
    w->kt = kt;
    w->nkeys = head >> 8;
    return walk_node(w, root, 0);
}

int scene_pack_build(const void *in, size_t n, const void *zsdic, size_t zsdic_n, void **out, size_t *out_n) {
    int result = -1;
    uint8_t *dics = NULL, *raw = NULL;
    void *z = NULL;
    DictFind df = {0};
    ZSTD_CDict *cd = NULL;
    *out = NULL;
    // The dictionaries: Pack/ZsDic.pack.zs is a SARC compressed without one.
    unsigned long long dn = ZSTD_getFrameContentSize(zsdic, zsdic_n);
    if (dn == ZSTD_CONTENTSIZE_UNKNOWN || dn == ZSTD_CONTENTSIZE_ERROR || dn > MAX_RAW || !(dics = malloc((size_t)dn)) ||
        ZSTD_decompress(dics, (size_t)dn, zsdic, zsdic_n) != dn) {
        rt_log("scene scale: Pack/ZsDic.pack.zs does not unpack");
        goto done;
    }
    unsigned long long rn = ZSTD_getFrameContentSize(in, n);
    df.id = ZSTD_getDictID_fromFrame(in, n);
    if (rn == ZSTD_CONTENTSIZE_UNKNOWN || rn == ZSTD_CONTENTSIZE_ERROR || rn > MAX_RAW || !(raw = malloc((size_t)rn)) ||
        (df.id && sarc_each(dics, (size_t)dn, dict_cb, &df) != 1)) {
        rt_log("scene scale: the Bootup pack is not a zstd frame with its size and a dictionary of Pack/ZsDic.pack.zs");
        goto done;
    }
    ZSTD_DCtx *dctx = ZSTD_createDCtx();
    size_t got = !dctx ? 0 : df.dd ? ZSTD_decompress_usingDDict(dctx, raw, (size_t)rn, in, n, df.dd)
                                   : ZSTD_decompressDCtx(dctx, raw, (size_t)rn, in, n);
    ZSTD_freeDCtx(dctx);
    Find f = {MEMBER, 0, 0, raw};
    Walk w;
    if (ZSTD_isError(got) || got != rn || sarc_each(raw, (size_t)rn, find_cb, &f) != 1 ||
        byml_values(&w, raw + f.at, f.size) < 0 || !w.count) {
        rt_log("scene scale: no " KEY " in the Bootup pack's %s", MEMBER);
        goto done;
    }
    int changed = 0;
    float first = 0;
    for (int i = 0; i < w.count; i++) {
        float v;
        memcpy(&v, raw + f.at + w.at[i], 4);
        if (!i) first = v;
        if (v != SCALE) {
            float s = SCALE;
            memcpy(raw + f.at + w.at[i], &s, 4);
            changed++;
        }
    }
    if (!changed) {   // the mod's pack has the port's values already
        rt_log("scene scale: the mod's Bootup pack has its %d " KEY " values at %.2f already", w.count, SCALE);
        result = 0;
        goto done;
    }
    cd = df.dd ? ZSTD_createCDict(df.dict, df.dict_n, 9) : NULL;
    size_t cap = ZSTD_compressBound((size_t)rn), zn = 0;
    ZSTD_CCtx *cctx = ZSTD_createCCtx();
    if (cctx && (z = malloc(cap)))
        zn = cd ? ZSTD_compress_usingCDict(cctx, z, cap, raw, (size_t)rn, cd) : ZSTD_compressCCtx(cctx, z, cap, raw, (size_t)rn, 9);
    ZSTD_freeCCtx(cctx);
    if (!z || !zn || ZSTD_isError(zn) || (df.dd && (cd == NULL))) {
        rt_log("scene scale: cannot pack the Bootup pack again");
        goto done;
    }
    *out = z;
    *out_n = zn;
    z = NULL;
    result = changed;
    rt_log("scene scale: the mod's Bootup pack gets the port's scene scale: %d of its %d " KEY " values (the first was "
           "%.2f) set to %.2f", changed, w.count, first, SCALE);
done:
    free(z);
    free(raw);
    free(dics);
    ZSTD_freeDDict(df.dd);
    ZSTD_freeCDict(cd);
    return result;
}
