// RomFS read straight from the player's NUP container (Rekarina, Switch 2 games): the game's files stay in the one
// .nup file the player dumped, nothing is unpacked to disk.
//
// NUP (a55uka/nup SPEC.md, MIT): a 0x80-byte superblock, a table of contents (packages, directories, files, a children
// array, a string pool) and a blob region of per-file extents, each stored raw or as one zstd frame holding the whole
// file. nup_index() walks the RomFS tree of every package (base first, then updates by version, so a later package's
// file replaces an earlier one's) and writes the TOTKRFS1 index romfs.c already reads: rows sorted by the bytes of the
// path, offset = the extent's absolute offset in the .nup, size = the file's uncompressed size. A raw file is then read
// in place like a romfs.bin range; a zstd file is inflated whole on its first read (nup_inflated) and kept until its
// last handle closes (nup_release).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zstd.h>

#include "host.h"
#include "platform/platform.h"
#include "romfs_nup.h"
#include "sha256.h"

#define SB_SIZE 0x80
#define PKG_SIZE 0x40
#define DIR_SIZE 0x18
#define FILE_SIZE 0x50
#define NONE 0xFFFFFFFFu

typedef struct { uint32_t name_off, name_len, first_child, child_count; } Dir;
typedef struct { char *path; uint64_t extent, csize, size; uint8_t codec; int layer; } Row;

static struct {
    const uint8_t *toc;
    uint64_t toc_size, dir_count, file_count, pool_off, pool_size, children_off;
    Row *rows;
    size_t nrows, cap;
} T;

static NupExtra *extras;
static uint64_t nextras;
static void **inflated;      // per file index: the inflated bytes of a zstd file, NULL until read
static uint32_t *refs;       // per file index: open handles
static PlatMutex zlock = PLAT_MUTEX_INIT;

static uint32_t u32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t u64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }

static void need(int c, const char *path, const char *what) {
    if (!c) rt_fatal("NUP %s: %s", path, what);
}

static uint64_t dirs_off;   // TOC offset of dirs[]
static uint64_t files_off;  // TOC offset of files[]

static Dir dir_rec(uint64_t i) {
    const uint8_t *p = T.toc + dirs_off + i * DIR_SIZE;
    return (Dir){u32(p), u32(p + 4), u32(p + 8), u32(p + 12)};
}

static void push(Row r) {
    if (T.nrows == T.cap) {
        T.cap = T.cap ? T.cap * 2 : 65536;
        T.rows = realloc(T.rows, T.cap * sizeof *T.rows);
        if (!T.rows) rt_fatal("NUP: out of memory for the file list");
    }
    T.rows[T.nrows++] = r;
}

// Every file under directory d, with its path below the RomFS root (prefix "" at the root, "a/b/" deeper).
static void walk(const char *nup, uint64_t d, const char *prefix, int layer, int depth) {
    need(depth < 64, nup, "directory tree too deep");
    Dir dr = dir_rec(d);
    for (uint32_t k = 0; k < dr.child_count; k++) {
        uint64_t slot = T.children_off + 4ull * ((uint64_t)dr.first_child + k);
        need(slot + 4 <= T.toc_size, nup, "children bounds");
        uint32_t node = u32(T.toc + slot);
        uint32_t noff, nlen;
        if (node < T.dir_count) {
            Dir c = dir_rec(node);
            noff = c.name_off, nlen = c.name_len;
        } else {
            need(node - T.dir_count < T.file_count, nup, "file node");
            const uint8_t *f = T.toc + files_off + (uint64_t)(node - T.dir_count) * FILE_SIZE;
            noff = u32(f), nlen = u32(f + 4);
        }
        need((uint64_t)noff + nlen <= T.pool_size, nup, "name bounds");
        size_t plen = strlen(prefix);
        char *path = malloc(plen + nlen + 2);
        if (!path) rt_fatal("NUP: out of memory");
        memcpy(path, prefix, plen);
        memcpy(path + plen, T.toc + T.pool_off + noff, nlen);
        path[plen + nlen] = 0;
        if (node < T.dir_count) {
            path[plen + nlen] = '/', path[plen + nlen + 1] = 0;
            walk(nup, node, path, layer, depth + 1);
            free(path);
            continue;
        }
        const uint8_t *f = T.toc + files_off + (uint64_t)(node - T.dir_count) * FILE_SIZE;
        Row r = {path, u64(f + 0x10), u64(f + 0x18), u64(f + 8), f[0x40], layer};
        need(r.codec == 0 || r.codec == 1, nup, "unknown codec (only raw and zstd are read)");
        need(r.codec != 0 || r.csize == r.size, nup, "raw extent size");
        push(r);
    }
}

static int by_path_layer(const void *a, const void *b) {
    const Row *x = a, *y = b;
    int c = strcmp(x->path, y->path);
    return c ? c : (x->layer > y->layer) - (x->layer < y->layer);
}

typedef struct { uint32_t root; uint32_t version; uint8_t kind; } Pkg;
static int by_layer(const void *a, const void *b) {
    const Pkg *x = a, *y = b;
    int kx = x->kind == 0 ? 0 : 1, ky = y->kind == 0 ? 0 : 1;   // base, then updates by version
    if (kx != ky) return kx - ky;
    return (x->version > y->version) - (x->version < y->version);
}

uint8_t *nup_index(PlatFile *f, const char *nup, size_t *index_size, uint64_t *count, uint64_t *strings_offset,
                   NupExtra **extra) {
    uint8_t sb[SB_SIZE];
    need(plat_file_pread(f, sb, SB_SIZE, 0) == SB_SIZE, nup, "superblock read");
    need(!memcmp(sb, "NUP\x01", 4) && u32(sb + 4) == (0x80u | (1u << 16)), nup, "not a NUP v1 container");
    uint64_t toc_off = u64(sb + 8), toc_size = u64(sb + 0x10), blob_off = u64(sb + 0x38);
    need(toc_off == SB_SIZE && toc_size >= 0x30 && toc_size < (1ull << 30) && blob_off >= toc_off + toc_size, nup,
         "table of contents bounds");
    uint8_t *toc = malloc(toc_size);
    need(toc != NULL, nup, "out of memory for the table of contents");
    for (uint64_t done = 0; done < toc_size;) {
        uint32_t n = toc_size - done > (64u << 20) ? (64u << 20) : (uint32_t)(toc_size - done);
        int64_t r = plat_file_pread(f, toc + done, n, toc_off + done);
        need(r > 0, nup, "table of contents read");
        done += (uint64_t)r;
    }
    uint8_t d[32];
    rt_sha256(toc, toc_size, d);
    need(!memcmp(d, sb + 0x18, 32), nup, "the table of contents fails its SHA-256 (damaged file)");
    need(!memcmp(toc, "TOC\x01", 4), nup, "TOC magic");
    uint32_t npkg = u32(toc + 4);
    T.toc = toc, T.toc_size = toc_size;
    T.dir_count = u64(toc + 8), T.file_count = u64(toc + 0x10);
    T.pool_off = u64(toc + 0x18), T.pool_size = u64(toc + 0x20);
    dirs_off = 0x30 + (uint64_t)npkg * PKG_SIZE;
    files_off = dirs_off + T.dir_count * DIR_SIZE;
    T.children_off = files_off + T.file_count * FILE_SIZE;
    need(npkg >= 1 && npkg == u32(sb + 0x48) && T.children_off <= toc_size && T.pool_off + T.pool_size <= toc_size, nup,
         "table of contents layout");

    Pkg pk[64];
    int np = 0;
    for (uint32_t i = 0; i < npkg && np < 64; i++) {
        const uint8_t *p = toc + 0x30 + (uint64_t)i * PKG_SIZE;
        uint32_t romfs = u32(p + 0x10);
        uint8_t kind = p[0x18];
        if (romfs == NONE || (kind != 0 && kind != 1)) continue;   // add-on content is not served yet
        need(romfs < T.dir_count, nup, "package RomFS directory");
        pk[np++] = (Pkg){romfs, u32(p + 0x14), kind};
        rt_log("NUP %s: package %u %s %016llx v%u: RomFS", nup, i, kind ? "update" : "base",
               (unsigned long long)u64(p), u32(p + 0x14));
    }
    need(np > 0, nup, "no package with a RomFS");
    qsort(pk, np, sizeof *pk, by_layer);
    for (int i = 0; i < np; i++) walk(nup, pk[i].root, "", i, 0);
    qsort(T.rows, T.nrows, sizeof *T.rows, by_path_layer);
    size_t n = 0;   // keep the highest layer of each path
    for (size_t i = 0; i < T.nrows; i++) {
        if (n && !strcmp(T.rows[n - 1].path, T.rows[i].path)) {
            free(T.rows[n - 1].path);
            T.rows[n - 1] = T.rows[i];
        } else {
            T.rows[n++] = T.rows[i];
        }
    }
    T.nrows = n;

    // TOTKRFS1: <8sQQ> header, count rows <QQII> (offset, size, name offset, name length with the NUL), then the names
    size_t strings = 0;
    for (size_t i = 0; i < n; i++) strings += strlen(T.rows[i].path) + 1;
    size_t soff = 24 + n * 24, total = soff + strings;
    uint8_t *ix = malloc(total);
    NupExtra *ex = calloc(n ? n : 1, sizeof *ex);
    need(ix && ex, nup, "out of memory for the index");
    memcpy(ix, "TOTKRFS1", 8);
    uint64_t cnt = n, so = soff;
    memcpy(ix + 8, &cnt, 8);
    memcpy(ix + 16, &so, 8);
    size_t at = 0;
    for (size_t i = 0; i < n; i++) {
        const Row *r = &T.rows[i];
        uint32_t len = (uint32_t)strlen(r->path) + 1, name = (uint32_t)at;
        uint8_t *row = ix + 24 + i * 24;
        memcpy(row, &r->extent, 8);
        memcpy(row + 8, &r->size, 8);
        memcpy(row + 16, &name, 4);
        memcpy(row + 20, &len, 4);
        memcpy(ix + soff + at, r->path, len);
        at += len;
        ex[i] = (NupExtra){r->codec, r->csize};
        free(r->path);
    }
    free(T.rows);
    free(toc);
    memset(&T, 0, sizeof T);
    extras = ex;
    nextras = n;
    inflated = calloc(n ? n : 1, sizeof *inflated);
    refs = calloc(n ? n : 1, sizeof *refs);
    need(inflated && refs, nup, "out of memory for the inflate table");
    *index_size = total;
    *count = n;
    *strings_offset = soff;
    *extra = ex;
    return ix;
}

int nup_compressed(uint64_t i) { return i < nextras && extras[i].codec == 1; }

void nup_acquire(uint64_t i) {
    if (!nup_compressed(i)) return;
    plat_mutex_lock(&zlock);
    refs[i]++;
    plat_mutex_unlock(&zlock);
}

const uint8_t *nup_inflated(PlatFile *f, uint64_t i, uint64_t extent, uint64_t size) {
    plat_mutex_lock(&zlock);
    void *have = inflated[i];
    plat_mutex_unlock(&zlock);
    if (have) return have;
    uint64_t cs = extras[i].csize;
    uint64_t t0 = plat_now_ns();   // DEBUG-NUPSTAT
    uint8_t *z = malloc(cs ? cs : 1), *out = malloc(size ? size : 1);
    if (!z || !out) rt_fatal("NUP: out of memory inflating a %llu-byte file", (unsigned long long)size);
    for (uint64_t done = 0; done < cs;) {
        uint32_t n = cs - done > (64u << 20) ? (64u << 20) : (uint32_t)(cs - done);
        int64_t r = plat_file_pread(f, z + done, n, extent + done);
        if (r <= 0) rt_fatal("NUP: read failure at %#llx", (unsigned long long)(extent + done));
        done += (uint64_t)r;
    }
    uint64_t t1 = plat_now_ns();
    size_t got = ZSTD_decompress(out, size, z, cs);
    free(z);
    {   // DEBUG-NUPSTAT: every inflation over 4 MiB, and a summary every 200
        static uint64_t n, bytes, read_ns, zstd_ns, again;
        static uint8_t *seen;
        uint64_t t2 = plat_now_ns();
        plat_mutex_lock(&zlock);
        if (!seen) seen = calloc((size_t)(nextras / 8 + 1), 1);
        int re = seen && (seen[i / 8] >> (i % 8) & 1);
        if (seen) seen[i / 8] |= (uint8_t)(1u << (i % 8));
        n++, bytes += size, read_ns += t1 - t0, zstd_ns += t2 - t1, again += re;
        if (size > (4u << 20))
            rt_log("NUPSTAT file %llu: %.1f MiB (%.1f MiB packed) read %.1f ms inflate %.1f ms%s", (unsigned long long)i,
                   size / 1048576.0, cs / 1048576.0, (t1 - t0) / 1e6, (t2 - t1) / 1e6, re ? " (again)" : "");
        if (!(n % 200))
            rt_log("NUPSTAT %llu inflations, %.1f MiB, read %.0f ms, inflate %.0f ms, %llu of them again", (unsigned long long)n,
                   bytes / 1048576.0, read_ns / 1e6, zstd_ns / 1e6, (unsigned long long)again);
        plat_mutex_unlock(&zlock);
    }
    if (ZSTD_isError(got) || got != size)
        rt_fatal("NUP: file %llu does not inflate to %llu bytes (%s)", (unsigned long long)i, (unsigned long long)size,
                 ZSTD_isError(got) ? ZSTD_getErrorName(got) : "short");
    plat_mutex_lock(&zlock);
    if (inflated[i]) {   // another thread got there first
        free(out);
        out = inflated[i];
    } else {
        inflated[i] = out;
    }
    plat_mutex_unlock(&zlock);
    return out;
}

void nup_release(uint64_t i) {
    if (!nup_compressed(i)) return;
    void *drop = NULL;
    plat_mutex_lock(&zlock);
    if (refs[i] && !--refs[i]) {
        drop = inflated[i];
        inflated[i] = NULL;
    }
    plat_mutex_unlock(&zlock);
    free(drop);
}
