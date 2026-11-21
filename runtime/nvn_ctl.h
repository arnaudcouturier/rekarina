// An NVN shader's control section, NVN's (Switch 1) or the Switch 2's NVN2: what the runtime reads of it. The layout
// facts follow MacNativeGames games/totk/third_party/d2r_gfx/gfx/nvn/nvn_ctl.h @ 5843ee56 (MIT, Copyright (c) 2026
// MacNativeGames contributors), which read them from seedds/nxnative dm/gfx/tools/dmsh_ampere.py, shader_split.py and
// dmsh_meshstage.py (MIT, Copyright (c) 2026 seedds); written anew here in C.
//
// NVN (Switch 1): 0x880 bytes; +0x700 the c1 segment's start in the code area, +0x704 the code area's length, +0x714 the
// stage, +0x718 the fragment flags, +0x748 compute's thread group. The code area starts with a 0x30-byte header (magic
// 0x12345678 graphics, 0x12345679 compute), then the 0x50-byte SPH.
// NVN2 (magic 0x98761234 at +0, version 0xc9 at +4): +0x30 the section's own length in BNSH files (0xd8; 0xd0 in
// effect archives) and 0 in memory and in agl's BAHS files; +0x38 the instructions' length (the 0x80-byte SPH v4 of the
// graphics stages included), +0x40 that rounded up to 0x100, +0x44 the code area's length, the c1 segment lying
// between the two; +0x4c the register count; +0x56 the stage byte (5 compute, 6 task, 7 mesh; the graphics stages say
// it in their SPH); compute: +0x58 the thread group (3 x u32), +0x64 its shared memory bytes, +0xa9 its push-constant
// words, +0xb1..+0xb5 its uniform buffer, storage buffer, texture, separate texture and separate sampler entry counts in
// its own c[0] layout, +0xba (u16) that layout's size; task and mesh: +0x58 the workgroup's threads (u16), +0x5a the
// payload bytes (u16), +0x5e whether a mesh shader has a task shader, +0x5f the vertices of a mesh shader's primitive
// (3 triangles, 1 points: the grass's shadow points), +0x60 / +0x62 a mesh shader's most vertices and
// primitives, +0x64 the offset of its per-primitive SPH when +0x68 bit 7 is set. The code area has no header: a graphics
// stage starts with its SPH, whose word 0 bits 10..13 give the shader type (1 vertex, 2 / 3 tessellation, 4 geometry,
// 5 pixel; 0 with a type-1 SPH is a vertex shader, as Ryujinx's ShaderHeader reads it).
//
// Without GAME_NS2 nvn2_ctl is 0, so a Switch 1 build reads every section as NVN's, as it always did.
#pragma once
#include <stdint.h>
#include <string.h>

#define NVN_CTL_MAGIC 0x98761234u
#define NVN_CTL_SIZE 0x880                 // an NVN section's bytes
#define NVN2_CTL_VERSION 0xc9u
#define NVN2_CTL_KEY 0x48                  // the bytes nxnative's and MacNativeGames' shader keys take (+0x30 as 0)
#define NVN2_CTL_READ 0xd0                 // bytes of an NVN2 section the runtime reads (the shortest seen is 0xd0)
// NVN shader stages past compute: NVN2's task and mesh shaders (control +0x56). The replay keeps their bindings in the
// tessellation stages' places (3 and 4): a program has mesh shaders or tessellation, never both (seedds/nxnative
// hle_nvn.c st_host).
#define NVN2_STAGE_TASK 6
#define NVN2_STAGE_MESH 7
#define NVN2_SLOT_TASK 3
#define NVN2_SLOT_MESH 4

static inline uint32_t nvn_ctl_u32(const uint8_t *c, uint32_t off) {
    uint32_t v;
    memcpy(&v, c + off, 4);
    return v;
}

static inline int nvn2_ctl(const uint8_t *c) {
#ifdef GAME_NS2
    return c && nvn_ctl_u32(c, 0) == NVN_CTL_MAGIC && nvn_ctl_u32(c, 4) == NVN2_CTL_VERSION;
#else
    (void)c;
    return 0;
#endif
}

// The code area's length (the bytes the replay binds and the cache hashes).
static inline uint32_t nvn_ctl_code_len(const uint8_t *c) { return nvn_ctl_u32(c, nvn2_ctl(c) ? 0x44 : 0x704); }
// Where the c1 constant segment starts in the code area; it runs to the code area's end.
static inline uint32_t nvn_ctl_c1_off(const uint8_t *c) { return nvn_ctl_u32(c, nvn2_ctl(c) ? 0x40 : 0x700); }
// Bytes of the code area the translator reads: NVN graphics drops the 0x30-byte header (from *skip), NVN compute keeps
// it; NVN2 has no header and its instructions end at +0x38.
static inline uint32_t nvn_ctl_xlate_span(const uint8_t *c, int compute, uint32_t *skip) {
    if (nvn2_ctl(c)) {
        *skip = 0;
        return nvn_ctl_u32(c, 0x38);
    }
    *skip = compute ? 0 : 0x30;
    return nvn_ctl_code_len(c);
}

// The NVN stage of a shader (0 vertex, 1 fragment, 2 geometry, 3 tessellation control, 4 tessellation evaluation,
// 5 compute; NVN2 also 6 task, 7 mesh), or -1. code: the code area (NVN2 graphics stages read its SPH).
static inline int nvn_ctl_stage(const uint8_t *c, const uint8_t *code) {
    if (!nvn2_ctl(c)) {
        uint32_t st = nvn_ctl_u32(c, 0x714);
        return st <= 5 ? (int)st : -1;
    }
    uint8_t s = c[0x56];
    if (s == 5 || s == NVN2_STAGE_TASK || s == NVN2_STAGE_MESH) return s;
    if (!code) return -1;
    static const int from_sph[6] = {0, 0, 3, 4, 2, 1};
    uint32_t w0;
    memcpy(&w0, code, 4);
    unsigned type = (w0 >> 10) & 0xf;
    return (w0 & 0x1f) && type <= 5 ? from_sph[type] : -1;
}

// The replay's stage slot of an NVN stage: task and mesh take the tessellation places (above); others are themselves.
static inline int nvn_stage_slot(int nvn_stage) {
#ifdef GAME_NS2
    return nvn_stage == NVN2_STAGE_TASK ? NVN2_SLOT_TASK : nvn_stage == NVN2_STAGE_MESH ? NVN2_SLOT_MESH : nvn_stage;
#else
    return nvn_stage;
#endif
}

// Compute's thread group.
static inline void nvn_ctl_local(const uint8_t *c, uint32_t l[3]) { memcpy(l, c + (nvn2_ctl(c) ? 0x58 : 0x748), 12); }
// NVN2 compute's shared memory bytes (0: none or not NVN2).
static inline uint32_t nvn2_ctl_shared(const uint8_t *c) { return nvn2_ctl(c) ? nvn_ctl_u32(c, 0x64) : 0; }
// A mesh program that outputs points or lines, or a point size (the OMAP's system values, SPH byte 0x35 bits 0..3):
// what translator patch 0040 translates otherwise than before (gfx_shader.c build_code_hash).
static inline int nvn2_ctl_mesh_points(const uint8_t *c, const uint8_t *code, uint64_t code_size) {
    return nvn2_ctl(c) && code_size > 0x35 && (c[0x5f] != 3 || (code[0x35] & 0x0f));
}
// An NVN2 program with a VOTEU whose vote goes to a uniform predicate other than UPT (bits 81..83 of the 16-byte
// instruction): what translator patch 0042 translates otherwise than before (gfx_shader.c build_code_hash). A word of
// the program header that reads as one only gives that program a key of its own.
static inline int nvn2_ctl_voteu_pred(const uint8_t *c, const uint8_t *code, uint64_t code_size) {
    if (!nvn2_ctl(c)) return 0;
    for (uint64_t o = 0; o + 16 <= code_size; o += 16) {
        uint64_t lo, hi;
        memcpy(&lo, code + o, 8), memcpy(&hi, code + o + 8, 8);
        if ((lo & 0xfff) == 0x886 && ((hi >> 17) & 7) != 7) return 1;
    }
    return 0;
}
// An NVN2 program with REDUX (0x3c4), MATCH (0x3a1), HMMA (0x23c), LDSM (0x83b) or MOVM (0x23a): the warp collectives
// that translator patch 0044 translates otherwise in compute shaders translated for subgroups wider than 32 lanes
// (gfx_shader.c build_code_hash).
static inline int nvn2_ctl_warp_lanes(const uint8_t *c, const uint8_t *code, uint64_t code_size) {
    if (!nvn2_ctl(c)) return 0;
    for (uint64_t o = 0; o + 16 <= code_size; o += 16) {
        uint64_t lo;
        memcpy(&lo, code + o, 8);
        uint32_t op = (uint32_t)(lo & 0xfff);
        if (op == 0x3c4 || op == 0x3a1 || op == 0x23c || op == 0x83b || op == 0x23a) return 1;
    }
    return 0;
}
// An NVN2 task or mesh shader's workgroup threads (control +0x58, u16).
static inline uint32_t nvn2_ctl_mesh_threads(const uint8_t *c) {
    uint16_t t;
    memcpy(&t, c + 0x58, 2);
    return t ? t : 1;
}
