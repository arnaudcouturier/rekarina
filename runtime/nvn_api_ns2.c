// NVN front end, Switch 2 slice (GAME_NS2): the NVN2 entry points a Switch 2 game reaches beyond the Switch 1 ones,
// and the Switch 1 entry points TotK never calls that NVN2 games do. A Switch 1 build compiles this file to nothing.
//
// Ported from MacNativeGames games/totk/third_party/d2r_gfx/gfx/nvn/hle_nvn.c, its GAME_NS2 blocks @ 5843ee56 (MIT,
// Copyright (c) 2026 MacNativeGames contributors), and seedds/nxnative dm/gfx/nvn/hle_nvn.c @ 865cc35f (MIT, Copyright
// (c) 2026 seedds), where MacNativeGames took them from (nxnative 8dd0c98a, a7bd05f4). Behaviour is ported, the code is
// written for this front end (arguments from the AAPCS64 registers, records in nvn_obj.h's format).
//
// Which entries: the procs OoT's main calls (its proc pointers loaded anywhere but in the proc loader: 313 of
// the 736 it asks for, 38 of them without an implementation before) and those the references implement for Donkey Kong
// Bananza, Pokemon Pokopia and the other Switch 2 games. Every other NVN2 name answers through nvn_procs.c's logged stub.
#ifdef GAME_NS2
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "gfx.h"
#include "host.h"
#include "nvn_api.h"

#define ONCE(...) do { static uint8_t once_; if (!__atomic_exchange_n(&once_, 1, __ATOMIC_RELAXED)) rt_log("nvn: " __VA_ARGS__); } while (0)

static cmd_t *cmd_of(const Cpu *c) { return arg_p(c, 0); }

// ---------- Device
// NVNdeviceFlagBits nvnDeviceGetFlags(const NVNdevice *d)
NVN_API(nvnDeviceGetFlags) { ret_x(c, ((const dev_t_ *)arg_p(c, 0))->flags); }
// NVNwindowOriginMode nvnDeviceGetWindowOriginMode(const NVNdevice *d), NVNdepthMode nvnDeviceGetDepthMode(d): the modes
// set, NVN's defaults (lower left, near = -w; both 0) until a Set
NVN_API(nvnDeviceGetWindowOriginMode) { ret_x(c, ((const dev_t_ *)arg_p(c, 0))->origin); }
NVN_API(nvnDeviceGetDepthMode) { ret_x(c, ((const dev_t_ *)arg_p(c, 0))->depth_mode); }
// void nvnInstallGlobalDebugCallback(PFNNVNDEBUGCALLBACKPROC cb, void *data): debug-layer messages; the host has no debug
// layer, so nothing is ever reported through it
NVN_API(nvnInstallGlobalDebugCallback) { (void)c; }
// void nvnDeviceSetIntermediateShaderCache(NVNdevice *d, int max): shaders are translated and cached on the host
NVN_API(nvnDeviceSetIntermediateShaderCache) { (void)c; }

// ---------- Memory pools
// size_t nvnMemoryPoolBuilderGetSize(const NVNmemoryPoolBuilder *b), void *...GetMemory(b), int ...GetFlags(b),
// NVNdevice *...GetDevice(b)
NVN_API(nvnMemoryPoolBuilderGetSize) { ret_x(c, ((const pool_builder_t *)arg_p(c, 0))->size); }
NVN_API(nvnMemoryPoolBuilderGetMemory) { ret_x(c, (uintptr_t)((const pool_builder_t *)arg_p(c, 0))->mem); }
NVN_API(nvnMemoryPoolBuilderGetFlags) { ret_x(c, ((const pool_builder_t *)arg_p(c, 0))->flags); }
NVN_API(nvnMemoryPoolBuilderGetDevice) { ret_x(c, (uintptr_t)((const pool_builder_t *)arg_p(c, 0))->dev); }
// void nvnMemoryPoolBuilderSetNativeHandleNVX(NVNmemoryPoolBuilder *b, uint64_t handle) and its getter: the sdk's
// native-image glue (nvnTextureInitializeFromNativeImage, sdk 22.2.8) builds a pool over memory a native handle names
// (an nn::gs image). The host has no such memory: the handle is kept and the pool gets no storage from it, so its
// nvnMemoryPoolInitialize fails unless the builder also has storage (said once).
NVN_API(nvnMemoryPoolBuilderSetNativeHandleNVX) {
    pool_builder_t *b = arg_p(c, 0);
    b->native = arg_x(c, 1);
    ONCE("MemoryPoolBuilderSetNativeHandleNVX %#llx: native-handle pools get no host storage",
         (unsigned long long)b->native);
}
NVN_API(nvnMemoryPoolBuilderGetNativeHandleNVX) { ret_x(c, ((const pool_builder_t *)arg_p(c, 0))->native); }

// ---------- Programs from a builder (NVN2): SetDefaults, SetDevice, SetShaders(count, NVNshaderData[]), then
// nvnProgramInitializeFromBuilder. The stages are 16-byte {data, control} pairs as in NVN. The builder keeps the caller's
// array, which the game keeps until it initializes the program, as other NVN builders keep their pointers.
typedef struct { NVNdevice *dev; const NVNshaderData *sd; int32_t n, sample_shading; } prog_builder_t;
_Static_assert(sizeof(prog_builder_t) == 24, "seedds/nxnative prog_builder_t is 24 bytes; NVN2 games give at least that");
#define PB(c) ((prog_builder_t *)arg_p(c, 0))
NVN_API(nvnProgramBuilderSetDefaults) { memset(PB(c), 0, sizeof(prog_builder_t)); }
NVN_API(nvnProgramBuilderSetDevice) { PB(c)->dev = arg_p(c, 1); }
NVN_API(nvnProgramBuilderGetDevice) { ret_x(c, (uintptr_t)PB(c)->dev); }
// void nvnProgramBuilderSetShaders(NVNprogramBuilder *b, int n, const NVNshaderData *sd)
NVN_API(nvnProgramBuilderSetShaders) {
    PB(c)->n = (int32_t)arg_x(c, 1);
    PB(c)->sd = arg_p(c, 2);
}
NVN_API(nvnProgramBuilderGetShaderCount) { ret_x(c, (uint64_t)(int64_t)PB(c)->n); }
// void nvnProgramBuilderSetSampleShading(NVNprogramBuilder *b, NVNsampleShadingMode mode) and its getter: kept, not
// applied (as nvnProgramSetSampleShading below)
NVN_API(nvnProgramBuilderSetSampleShading) { PB(c)->sample_shading = (int32_t)arg_x(c, 1); }
NVN_API(nvnProgramBuilderGetSampleShading) { ret_x(c, (uint64_t)(int64_t)PB(c)->sample_shading); }
// NVNboolean nvnProgramInitializeFromBuilder(NVNprogram *p, const NVNprogramBuilder *b)
NVN_API(nvnProgramInitializeFromBuilder) {
    NVN_BLOCKING();
    const prog_builder_t *b = arg_p(c, 1);
    if (b->sample_shading) ONCE("program with sample shading mode %d: shaded per pixel", b->sample_shading);
    ret_x(c, nvn_prog_init_shaders(arg_p(c, 0), b->n, b->sd));
}
// NVNboolean nvnProgramSetSampleShading(NVNprogram *p, NVNsampleShadingMode mode): per-sample shading is not forced; a
// shader that reads the sample index still runs per sample
NVN_API(nvnProgramSetSampleShading) {
    int mode = (int)arg_x(c, 1);
    if (mode) ONCE("nvnProgramSetSampleShading mode %d: shaded per pixel unless the shader reads the sample index", mode);
    ret_x(c, 1);
}

// ---------- Queues
// void nvnQueueBuilderSetPrimaryQueue(NVNqueueBuilder *b, NVNqueue *primary): a queue NVN2 ties to a primary queue
// (Donkey Kong Bananza's second queue). Every queue's submits replay in order on the one Vulkan queue, so the tie needs
// no work; the builder has no room left to keep it (queue_builder_t fills its 0x40 bytes), so the getter answers NULL.
NVN_API(nvnQueueBuilderSetPrimaryQueue) { (void)c; }
NVN_API(nvnQueueBuilderGetPrimaryQueue) { ret_x(c, 0); }
// void nvnQueueBuilderSetQueuePriority(NVNqueueBuilder *b, NVNqueuePriority p): one host queue
NVN_API(nvnQueueBuilderSetQueuePriority) { (void)c; }
NVN_API(nvnQueueBuilderGetQueuePriority) { ret_x(c, 1); }   // NVN_QUEUE_PRIORITY_NORMAL
#define QB(c) ((const queue_builder_t *)arg_p(c, 0))
NVN_API(nvnQueueBuilderGetDevice) { ret_x(c, (uintptr_t)QB(c)->dev); }
NVN_API(nvnQueueBuilderGetFlags) { ret_x(c, QB(c)->flags); }
NVN_API(nvnQueueBuilderGetCommandMemorySize) { ret_x(c, QB(c)->cmd_sz); }
NVN_API(nvnQueueBuilderGetComputeMemorySize) { ret_x(c, QB(c)->comp_sz); }
NVN_API(nvnQueueBuilderGetControlMemorySize) { ret_x(c, QB(c)->ctl_sz); }
NVN_API(nvnQueueBuilderGetCommandFlushThreshold) { ret_x(c, QB(c)->flush); }
NVN_API(nvnQueueBuilderGetMemorySize) { ret_x(c, QB(c)->mem_sz); }
NVN_API(nvnQueueBuilderGetMemory) { ret_x(c, (uintptr_t)QB(c)->mem); }
// NVNboolean nvnQueueWaitSync(NVNqueue *q, const NVNsync *s): orders this queue after the sync. Every queue replays in
// submit order on one Vulkan queue and a sync is a fence of that order (nvn_api_core.c), so the order already holds
// (seedds/nxnative hle_nvn.c: says once when the sync has no fence yet).
NVN_API(nvnQueueWaitSync) {
    const sync_t *s = arg_p(c, 1);
    if (s && !s->fenced) ONCE("QueueWaitSync on a sync with no fence yet: the host does not wait");
    ret_x(c, 1);
}
// size_t nvnQueueGetTotal{Command,Control,RayTracing}MemoryUsed(const NVNqueue *q), void ResetMemoryUsageCounts(q):
// statistics of the queue's own memory, which the host does not use
NVN_API(nvnQueueGetTotalCommandMemoryUsed) { ret_x(c, 0); }
NVN_API(nvnQueueGetTotalControlMemoryUsed) { ret_x(c, 0); }
NVN_API(nvnQueueGetTotalRayTracingMemoryUsed) { ret_x(c, 0); }
NVN_API(nvnQueueResetMemoryUsageCounts) { (void)c; }
// void nvnQueueSetTimeoutNVX / SetTimesliceNVX(NVNqueue *q, uint64_t value): GPU scheduling of the console's driver
NVN_API(nvnQueueSetTimeoutNVX) { (void)c; }
NVN_API(nvnQueueSetTimesliceNVX) { (void)c; }

// ---------- Command buffers: modes and hints with no effect on the replay's results
// void nvnCommandBufferSetRecordingMode(NVNcommandBuffer *cmd, int mode): NVN2 modes set on every command buffer Bananza
// begins. Recording modes are 0, 1 and 2 (Pokemon Pokopia selects all three; kirakira-dev/ouncenative's NVN2 layer
// accepts only these): the host records every command buffer alike, and another value is said once.
NVN_API(nvnCommandBufferSetRecordingMode) {
    int mode = (int)arg_x(c, 1);
    if (mode < 0 || mode > 2) ONCE("SetRecordingMode %d: NVN2 has modes 0-2; recorded as the others", mode);
}
// SetGeometryProcessingMode, SetComputeOverlapMode (whether compute may overlap the next work), workload priorities:
// how the GPU front end schedules the commands, not what they produce. The replay runs everything in order.
NVN_API(nvnCommandBufferSetGeometryProcessingMode) { (void)c; }
NVN_API(nvnCommandBufferSetComputeOverlapMode) { (void)c; }
NVN_API(nvnCommandBufferSetComputeWorkloadPriority) { (void)c; }
NVN_API(nvnCommandBufferSetGraphicsWorkloadPriority) { (void)c; }
NVN_API(nvnCommandBufferSetResourceOccupancy) { (void)c; }
// void nvnCommandBufferSetStencilCullCriteria(NVNcommandBuffer *cmd, NVNstencilFunc func, int ref): ZCull's stencil
// criteria; the host does no ZCull and the draws' own stencil test still applies
NVN_API(nvnCommandBufferSetStencilCullCriteria) { (void)c; }
// void nvnCommandBufferSetConservativeRasterMode(NVNcommandBuffer *cmd, int mode): conservative rasterisation is not
// replayed (nvn_ext.c SetConservativeRasterEnable says so when it is enabled)
NVN_API(nvnCommandBufferSetConservativeRasterMode) { (void)c; }
// Variable rate shading: the image, its palettes and their mode ask the hardware to shade some tiles at a coarser rate.
// Shading every pixel is the most a rate can ask, so ignoring them changes only sharpness (seedds/nxnative: nothing is
// recorded).
NVN_API(nvnCommandBufferBindShadingRateImage) { (void)c; }
NVN_API(nvnCommandBufferShadingRateImagePalette) { (void)c; }
NVN_API(nvnCommandBufferSetShadingRatePaletteMode) { (void)c; }
// void nvnCommandBufferSetProgramSubroutines(cmd, program, stage, first, n, const int *values) and
// BindCoverageModulationTable(cmd, int n, const float *entries): subroutines are resolved in the translated shaders
// (none of the games seen uses them) and coverage modulation needs target-independent rasterisation, which is off
NVN_API(nvnCommandBufferSetProgramSubroutines) { ONCE("SetProgramSubroutines: ignored"); (void)c; }
NVN_API(nvnCommandBufferBindCoverageModulationTable) { (void)c; }
// void nvnCommandBufferSetRepresentativeFragmentTest(cmd, NVNboolean on): lets the GPU skip fragments of a primitive
// whose result only matters as "some fragment passed"; running all of them gives the same result
NVN_API(nvnCommandBufferSetRepresentativeFragmentTest) { (void)c; }
// void nvnCommandBufferSetFloatBlendClamp(cmd, NVNboolean clamp): blending into float targets clamps its inputs;
// not replayed, said once when turned on
NVN_API(nvnCommandBufferSetFloatBlendClamp) {
    if (nvn_arg_bool(c, 1)) ONCE("SetFloatBlendClamp on: float targets blend unclamped");
}
// Exclusive scissors (cmd, int first, int n, const int *rects; EnableExclusiveScissor(cmd, int index, NVNboolean)): a
// second scissor test, not replayed; said once when enabled
NVN_API(nvnCommandBufferEnableExclusiveScissor) {
    if (nvn_arg_bool(c, 2)) ONCE("EnableExclusiveScissor: the exclusive scissor test is not applied");
}
NVN_API(nvnCommandBufferSetExclusiveScissor) { (void)c; }
NVN_API(nvnCommandBufferSetExclusiveScissors) { (void)c; }
// Debug groups and markers per engine: meaningful only to NVN debugging tools
NVN_API(nvnCommandBufferInsertDebugMarkerGraphics) { (void)c; }
NVN_API(nvnCommandBufferInsertDebugMarkerCompute) { (void)c; }
NVN_API(nvnCommandBufferPushDebugGroupGraphics) { (void)c; }
NVN_API(nvnCommandBufferPushDebugGroupCompute) { (void)c; }
NVN_API(nvnCommandBufferPopDebugGroupGraphics) { (void)c; }
NVN_API(nvnCommandBufferPopDebugGroupCompute) { (void)c; }

// ---------- Command buffers: NVN2 commands
// void nvnCommandBufferPush{Graphics,Compute}Constants[Deferred](NVNcommandBuffer *cmd, uint64_t word, uint64_t offset,
// uint64_t size, const void *data): bytes of a constant block of the graphics stages or of compute, which their shaders
// read as c[24] (NVN2_PUSH_BANK; NVN2 compute's own c0 layout starts with them, and the translator maps that region
// there). Donkey Kong Bananza's upscaler pushes (1 / w, 1 / h, w, h) of its target for its graphics passes and two
// integer rectangles for its compute network (seedds/nxnative a7bd05f4). The word before the offset was always 0 in the
// references' games; another value is said once. A *Deferred push reads its bytes at submit (nvn_api_cmd.c
// resolve_deferred), as the *Deferred binds do.
static void push_const(Cpu *c, int compute, int deferred) {
    uint64_t w = arg_x(c, 1), off = arg_x(c, 2), n = arg_x(c, 3);
    const void *data = arg_p(c, 4);
    if (w) ONCE("push constants with the word before the offset %#llx: its meaning is not known", (unsigned long long)w);
    if (!data || !n || off >= NVN2_PUSH_SIZE || n > NVN2_PUSH_SIZE - off) {
        if (n) ONCE("push constants at %#llx + %#llx: beyond the %#x-byte block, dropped", (unsigned long long)off,
                    (unsigned long long)n, NVN2_PUSH_SIZE);
        return;
    }
    p_push_const *r = nvn_rec(cmd_of(c), OP_PUSH_CONST, offsetof(p_push_const, data) + n);
    r->compute = (uint16_t)compute;
    r->offset = (uint16_t)off;
    r->size = (uint32_t)n;
    r->src = deferred ? (uint64_t)(uintptr_t)data : 0;
    memcpy(r->data, data, n);
}
NVN_API(nvnCommandBufferPushGraphicsConstants) { push_const(c, 0, 0); }
NVN_API(nvnCommandBufferPushComputeConstants) { push_const(c, 1, 0); }
NVN_API(nvnCommandBufferPushGraphicsConstantsDeferred) { push_const(c, 0, 1); }
NVN_API(nvnCommandBufferPushComputeConstantsDeferred) { push_const(c, 1, 1); }
// void nvnCommandBufferDrawMeshTasks(NVNcommandBuffer *cmd, int first, int count): count task workgroups numbered from
// first, or mesh workgroups in a program with no task shader (seedds/nxnative nvn_geometry.inc mesh_tasks_draw)
NVN_API(nvnCommandBufferDrawMeshTasks) {
    int first = (int)arg_x(c, 1), count = (int)arg_x(c, 2);
    if (count <= 0) return;
    *(p_draw_mesh *)nvn_rec(cmd_of(c), OP_DRAW_MESH, sizeof(p_draw_mesh)) = (p_draw_mesh){(uint32_t)first, (uint32_t)count};
}
// void nvnCommandBufferDrawMeshTasksIndirect(NVNcommandBuffer *cmd, NVNbufferAddress indirect): OoT's
// opening scene calls it with the command buffer in x0 and the record's address in x1 (seedds/nxnative's OoT note;
// x2 not set). The record is read when the GPU runs the draw (vk_exec_draw.c).
NVN_API(nvnCommandBufferDrawMeshTasksIndirect) {
    ((p_draw_mesh_ind *)nvn_rec(cmd_of(c), OP_DRAW_MESH_IND, sizeof(p_draw_mesh_ind)))->ind = arg_x(c, 1);
}
// void nvnCommandBufferFenceSync(NVNcommandBuffer *cmd, NVNsync *s, NVNsyncCondition cond, int flags): the replay fences
// the sync with the submission that reaches this point (OP_FENCE_SYNC). Not cleared at the record: Pokemon Pokopia
// polled the sync of a command buffer it had not submitted yet and stopped its frame loop when the record cleared it
// (MacNativeGames ns2-port.md).
NVN_API(nvnCommandBufferFenceSync) {
    ((p_fence_sync *)nvn_rec(cmd_of(c), OP_FENCE_SYNC, sizeof(p_fence_sync)))->sync = arg_p(c, 1);
}
// void nvnCommandBufferSetCopyImageStride(NVNcommandBuffer *cmd, ptrdiff_t stride): the bytes between the 2D images of
// a 3D texture or array copied to or from a buffer (0 = rows x row stride), as SetCopyRowStride for rows
NVN_API(nvnCommandBufferSetCopyImageStride) {
    ((p_copy_stride *)nvn_rec(cmd_of(c), OP_COPY_IMAGE_STRIDE, sizeof(p_copy_stride)))->stride = (int64_t)arg_x(c, 1);
}

// ---------- State objects
// NVNboolean nvnMultisampleStateGetSampleLocationsGridEnable(const NVNmultisampleState *s) and its setter
NVN_API(nvnMultisampleStateSetSampleLocationsGridEnable) { ((ms_st *)arg_p(c, 0))->sl_grid = nvn_arg_bool(c, 1) != 0; }
NVN_API(nvnMultisampleStateGetSampleLocationsGridEnable) { ret_x(c, ((const ms_st *)arg_p(c, 0))->sl_grid); }

// ---------- Windows
// uint64_t nvnWindowGetAcquireTimeout(const NVNwindow *w) and the setters: WindowAcquireTexture never fails here, so the
// timeout is never reached; the answer is "wait for ever"
NVN_API(nvnWindowGetAcquireTimeout) { ret_x(c, UINT64_MAX); }
NVN_API(nvnWindowBuilderGetAcquireTimeout) { ret_x(c, UINT64_MAX); }
NVN_API(nvnWindowSetAcquireTimeout) { (void)c; }
NVN_API(nvnWindowBuilderSetAcquireTimeout) { (void)c; }
#endif
