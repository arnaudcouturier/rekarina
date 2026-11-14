// From MacNativeGames games/totk/runtime/romfs_path.h @ b75be8cf (MIT, Copyright (c) 2026 MacNativeGames
// contributors).
// RomFS path validation shared by the runtime (romfs.c) and its probe (fs_host.c probe_fs).
#pragma once
#include <stddef.h>
#include <string.h>

// A RomFS-relative path: non-empty, no leading '/', no '\', no empty, "." or ".." component.
static inline int romfs_path_valid_relative(const char *path) {
    if (!path || !*path || *path == '/' || strchr(path, '\\')) return 0;
    for (const char *part = path; *part;) {
        const char *end = strchr(part, '/');
        size_t size = end ? (size_t)(end - part) : strlen(part);
        if (!size || (size == 1 && part[0] == '.') || (size == 2 && part[0] == '.' && part[1] == '.')) return 0;
        if (!end) break;
        part = end + 1;
    }
    return 1;
}

// "<mount>:/<path>" -> "<path>" in buf, or NULL. Audio stream requests join folders that end in '/' with resources
// that begin with one, producing "content://Sound/Resource/Stream/x.bwav" for music and
// "content://Voice/Resource//Stream/x.bwav" for OoT's voice lines; nn::fs reads a run of separators as
// one, so runs are folded here, at the root and inside the path. Traversal, backslashes and
// a path longer than buf are still rejected.
static inline const char *romfs_path_relative(const char *mount, const char *path, char *buf, size_t size) {
    if (!mount || !*mount || !path || !buf || !size) return NULL;
    size_t n = strlen(mount);
    if (strncmp(path, mount, n) || path[n] != ':' || path[n + 1] != '/') return NULL;
    path += n + 2;
    while (*path == '/') ++path;
    size_t o = 0;
    for (; *path; ++path) {
        if (*path == '/' && path[1] == '/') continue;   // a run of separators: its last one is kept
        if (o + 1 >= size) return NULL;
        buf[o++] = *path;
    }
    buf[o] = 0;
    return romfs_path_valid_relative(buf) ? buf : NULL;
}
