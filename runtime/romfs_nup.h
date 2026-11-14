// RomFS from a NUP container (runtime/romfs_nup.c): the index romfs.c reads, built from the container's TOC, and the
// inflated bytes of its zstd-stored files.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "platform/platform.h"

typedef struct { uint8_t codec; uint64_t csize; } NupExtra;   // codec 0 raw, 1 zstd; csize: stored bytes

// The TOTKRFS1 index (malloc'd) of the RomFS files of every base and update package in the container, the highest
// layer winning a path; fatal on a damaged or unknown container.
uint8_t *nup_index(PlatFile *f, const char *nup, size_t *index_size, uint64_t *count, uint64_t *strings_offset,
                   NupExtra **extra);
int nup_compressed(uint64_t i);   // file index i is stored as zstd
void nup_acquire(uint64_t i);     // a handle to file i opened
void nup_release(uint64_t i);     // closed: the last close frees its inflated bytes
// The whole file i inflated (extent: its offset in the container, size: uncompressed); valid until its last release.
const uint8_t *nup_inflated(PlatFile *f, uint64_t i, uint64_t extent, uint64_t size);
