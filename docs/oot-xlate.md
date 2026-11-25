# Switch 2 (NVN2, Ampere) shaders in the translator

`totk_xlate` translates the Ampere (sm_86) machine code of NVN2 shaders as well as Maxwell code. This page is the
contract with the runtime (`runtime/gfx_shader.c` `build_request` / `ns2_request`): the request fields an NVN2
shader takes, where each comes from in the NVN2 control section, what changes in the reply, and what the runtime has to
do with it. Measurements are on OoT 1.1.1 (title 0400486024DEC000); the procedure is at the end.

## Translator sources

Patches on Ryujinx 1.3.2 (`tools/xlate/patches/`, applied after 0001-0016 by `tools/build_xlate.py`):

| Patch | From | What |
|---|---|---|
| 0017-ampere-switch-2-shaders | MacNativeGames `games/totk/tools/totk_xlate/patches/0006` @ `5843ee5` (itself seedds/nxnative's, MIT) | Ampere decoder and emitters, tensor pass (HMMA), NVN2 compute c[0] layout, memory pool table for global memory at loaded addresses, `TranslationFlags.Ampere` |
| 0018-fixed-input-geometry-as-compute | MacNativeGames `patches/0007` | geometry as compute kernels (Metal); inert here, kept for parity |
| 0019-ampere-frontend-nxnative-oot-wip | seedds/nxnative branch `oot-wip` @ `ee9b776` | nxnative's current Ampere frontend files: the texture forms through registers and uniform registers, ATOMS.CAS, F2I.U64, BMSK, 0x925, separate handles in NVN2 compute c[0], guarded deferred writes, `.FTZ` compares, task / mesh stage memory |
| 0020-global-memory-map-hint-and-bitmap-log | seedds/nxnative `9fb4467` | the pool table's chunk hint and the write log as a bitmap |
| 0021-global-memory-map-job-flag | ours | the pool table path only when the job asks for it (`gmem_map`) |
| 0022-ampere-vulkan-fixes | ours | Vulkan fixes found on OoT: 1D fetches, TXQ components, `StorageImageReadWithoutFormat`, ATOMG.CAS, float add reductions; MNG's F2FP.MERGE_C .H1 and CCTL kept; MNG's goto elimination rule for Ampere only |
| 0023-ampere-f2ip-packed-conversion | ours | F2IP (0x243), the packed float-to-byte conversion |
| 0024-nvn2-task-and-mesh-shaders | ours, after nxnative's `dmsh_meshstage.py` | task and mesh shaders as VK_EXT_mesh_shader `TaskEXT` / `MeshEXT` entry points (below) |
| 0025-ampere-ordered-memory-loads | ours | the AST optimizer moves a memory load to its use only when nothing that may write that memory lies between (Ampere jobs; "NN noise" below) |
| 0026-dead-code-keeps-multi-destination-sources | ours | dead code removal keeps the sources of an operation that still has a destination in use (HMMA's Rd / Rd + 1; "NN noise" below) |
| 0027-ampere-answered-integer-image-format | ours | a formatted surface load or store of an image answered with an integer `img` fmt declares that format and moves raw bits ("NN noise" below) |
| 0028-ampere-guarded-warp-collectives | ours | a guarded VOTE, VOTEU, SHFL, MATCH or REDUX runs in every active lane; the guard masks only its register writes |
| 0029-global-memory-map-8192-entries | ours | the pool table for global memory at loaded addresses has 8192 entries (runtime GMEM_ENTRIES) |
| 0030-warp-collectives-stay-in-place | ours | the AST optimizer never moves a ballot, vote or shuffle to its uses ("Warp-aggregated atomics" below); Maxwell jobs too |

A Maxwell job (no `arch`, or `"arch": "sm50"`) goes through none of the new paths; "Verifying" below has the
comparison.

## Request (`job`) fields for NVN2 shaders

All other fields keep their meaning (`tools/xlate/Program.cs` header): `key`, `debug`, `tex`, `img`, `attr`, `fout`,
`reduced`, `softfloat64`, `zero_locals`, `sbuf_align`, `subgroup`, `gather_bias`. NVN2 control section offsets are
byte offsets from its start (magic `0x98761234`, version `0xc9` at +4; `runtime/nvn_ctl.h`).

| Field | Type | Meaning |
|---|---|---|
| `arch` | string | `"sm86"`: Ampere code, translated by the Ampere frontend (`TranslationFlags.Ampere`). Absent or `"sm50"`: Maxwell. Any other value fails the job (`ArgumentException: arch ... unknown`). |
| `stage` | string | `"vs"`, `"ps"`, `"cs"`, `"ts"` (task) or `"ms"` (mesh) with `sm86`; `"gs"` fails (`stage gs not supported for sm86 (vs, ps, cs, ts, ms)`). `"ts"` / `"ms"` without `sm86` fail too. |
| `code` | path | The code area's bytes `[0, control + 0x38)`: the instructions, for graphics, task and mesh stages preceded by their 0x80-byte SPH (version 4); NVN2 code areas have no 0x30-byte header (`nvn_ctl_xlate_span`). |
| `c1` | hex string | The code area's bytes `[control + 0x40, control + 0x44)`: the c[1] segment (BRX jump tables; the decoder ends an Ampere jump table at its end at the latest). |
| `local` | [x, y, z] | Compute: the thread group, control +0x58, three u32. Task and mesh: `[threads, 1, 1]`, threads the u16 at +0x58 (1 if 0); at most 128. |
| `shared` | int (bytes) | Compute: control +0x64 (u32) rounded up to 16. The translator adds its own scratch for HMMA (256 words per 32-thread subgroup) on top. Ignored for task and mesh (their shared memory is the stage memory). |
| `nvn2_cs` | array of 5 or 7 ints | Compute only: `[control[0xb1] uniform buffer entries, control[0xb2] storage buffer entries, control[0xb3] texture entries, u16 at 0xba (c[0] layout size in bytes), control[0xa9] * 4 (push constant bytes)]`, optionally followed by `control[0xb4]` separate texture entries and `control[0xb5]` separate sampler entries (seedds/nxnative sends all seven; OoT's 4,123 compute shaders have 0 in both, so the five-entry form the runtime sends is equivalent for OoT). Without it a compute shader's c[0] reads are taken as NVN's fixed layout. |
| `mesh` | object | Task and mesh only, as `ns2_request` sends it: `payload` (u16 +0x5a, bytes), `task` (byte +0x5e: a mesh shader launched by a task), `max_vertices` (u16 +0x60), `max_primitives` (u16 +0x62), `primitive_sph` (u32 +0x64 when +0x68 bit 7, else 0: the offset in the code area of a mesh shader's per-primitive SPH). Optional, for a per-primitive SPH outside the c1 segment's start: `c1_offset` (u32 +0x40) or `primitive_sph_hex` (its 0x80 bytes). |
| `gmem_map` | bool | `true`: the host binds the memory pool table and its write log (below), and a global memory access the translator traces to no storage buffer goes through it. Absent or `false`: such an access is dropped as in Ryujinx (a load reads 0, a store or atomic does nothing) and the module needs no physical pointers. OoT: 2,130 of its 42,331 vertex, pixel and compute shaders have such accesses (2,111 loads among them). |
| `mvprev` | bool | Vertex shaders: also write last frame's position, from last frame's blocks at slot bank + 15 (patch 0015). With `sm86`, a position whose banks need a slot the shader reads itself gets none: NVN2 shaders read their push constants as c[24], c9's last-frame slot ("Last frame's position" below). |

The NVN2 compute c[0] layout (`nvn2_cs`, `InstEmitAmpere.AMapC0`): from c[0] + 0x10 the push constants
(`push` bytes, rounded up to 16), then a 0x10-byte entry per uniform buffer slot, one per storage buffer slot, then
8-byte handles per texture slot, separate texture slot, separate sampler slot and image slot, the layout `size` bytes
long. The translator maps them where the Maxwell layout keeps them, so the host fills NVN's tables as for any stage:
push constants → c[24] + offset, uniform slot r → c[0] + 0x210 + 0x10 r, storage slot s → c[0] + 0x310 + 0x10 s,
texture slot t → c[2] + 0x20 + 8 t, separate texture r → c[2] + 0x168 + 8 r, separate sampler r → c[2] + 0x568 + 8 r,
image slot i → c[2] + 0x120 + 8 i. Task and mesh shaders read their c[0] as graphics stages do (no `nvn2_cs`).

`tex` / `img` slot numbers with `sm86` (Program.cs `TexSlot`, the same numbers as `gfx_shader.c` `handle_slot`): a
texture handle word at c[2] + 0x168 + 8 r is slot r, one at c[2] + 0x20 + 8 r (combined handles, NVN2 compute textures)
also slot r; an image handle word at c[2] + 0x120 + 8 r is image slot r (r < 9). A Maxwell job keeps the separate
table alone, as before. A size query on an image handle asks `img`. Only the handle's first word is looked at, as
`handle_slot` does.

`img` `fmt` with `sm86` (patch 0027): a formatted surface load or store (SULD.P / SUST.P) asks for its image slot's
`fmt` (the slot goes into `img_queried`). An integer format there (`R32Uint`, `R16G16Uint`, `R32Sint`, ...) is the
format of the view the game binds; the image is then declared with it (`res.json` `images` `format`) and the registers
are stored and loaded as raw bits, so the host must bind a view of exactly that format (for an NVN R32UI view on an
R11G11B10F or RGBA8 texture: an R32_UINT view of a `MUTABLE_FORMAT` image). No answer, or a float format, leaves the
image without a format, as before.

## Task and mesh shaders

The Ampere code runs as a compute kernel whose stage memory is its shared memory (seedds/nxnative's lowering, patch
0019: ISBERD mode 0x78 reads the workgroup number, 0x38 the input payload, 0xb8 the task's own output payload; AST
0x927 mode 0xb8 writes the task's payload, 0xf8 a mesh shader's attribute words, 0x88 the primitive count byte, 0x80 an
index byte). Patch 0024 puts a `TaskEXT` or `MeshEXT` entry point named `main` around it
(`CodeGen/Spirv/MeshStageWrapper.cs`), the Vulkan counterpart of nxnative's Metal object and mesh functions
(`dm/gfx/tools/dmsh_meshstage.py`):

1. the stage memory is zeroed; a mesh shader under a task copies the `TaskPayloadWorkgroupEXT` payload to its input;
2. thread 0 writes the workgroup number: a task's, and a mesh shader's without a task, is the u32 at **c[0] + 0x9F0**
   (`VK_C0_MESH_FIRST`, the draw's first workgroup, which the runtime fills) plus `WorkgroupId.x`; a mesh shader under
   a task takes `WorkgroupId.x` alone, also as the input payload's word 0;
3. the kernel runs; an early EXIT returns to the entry point;
4. a task copies its output payload (`payload` bytes) to its `TaskPayloadWorkgroupEXT` block and calls
   `EmitMeshTasksEXT(min(output word 1, 65535), 1, 1)`; a mesh shader calls `SetMeshOutputsEXT(max_vertices, n)` with n
   = the primitive count byte (at most `max_primitives`), writes each vertex's position and generic attributes from
   its attribute block and three index bytes per triangle (`OutputTrianglesEXT`). A shader of points or lines
   (`mesh.prim_verts` 1 or 2, patch 0040) has one or two index bytes a primitive and `OutputPoints` / `OutputLinesEXT`.
   With `mesh.whole_indices` (patch 0041) a thread stores a primitive's whole index vector.

Stage memory layout (bytes): 0 workgroup number, 0x10 input (the payload), then at the next 16-byte boundary the
outputs (a task's payload, or a mesh shader's attribute area: first its vertex blocks, 32 vertices per block, a word
per component, the components the SPH's OMAP enables in its order: the system values of byte 0x35 bits 0..3
(primitive id, layer, viewport index, point size), the position, the generic attributes; then, after the last vertex
block, its primitive blocks, 32 primitives per block, a word per component of the per-primitive SPH's OMAP), then a
mesh shader's primitive bytes (count at +3, index bytes from +4). The guest code addresses the attribute area itself,
in that order (patch 0048 below); the primitive bytes' place is the translator's (`MeshStage.Offsets[3]`), so a
component left out of the list puts the primitive bytes inside the last attribute block. A shader with LDS / STS / ATOMS of its own is refused (its shared
memory would overlap the stage memory, as nxnative refuses it); none of OoT's is.

What a task or mesh module holds, for the runtime's pipeline (`vk_pipeline.c` `stages_create`):

- SPIR-V 1.4 (`SPV_EXT_mesh_shader` needs it), capability `MeshShadingEXT`, extension `SPV_EXT_mesh_shader`; the entry
  point lists every global variable; storage buffers are `Block` in the `StorageBuffer` class (1.4 has no
  `BufferBlock`). Bindings, set 0 and the support buffer at binding 0 are as for other stages, so `spirv_rebind`
  applies.
- `LocalSize threads 1 1`; a mesh shader also `OutputVertices max_vertices`, `OutputPrimitivesEXT max_primitives` and
  `OutputTrianglesEXT`.
- Mesh outputs: `Position` (and `PointSize` where the SPH enables it or the shader outputs points) in a per-vertex
  block array; generic attribute n as a `vec4` array at `Location n` (the
  locations a vertex shader's outputs have, so the fragment shaders of vertex programs fit); per-primitive attribute n
  as a plain `vec4` array at `Location n` with each primitive on vertices of its own (patch 0048; where that does not
  fit in 256 vertices, a `vec4` array decorated `PerPrimitiveEXT`); `PrimitiveTriangleIndicesEXT` (points:
  `PrimitivePointIndicesEXT`, lines: `PrimitiveLineIndicesEXT`). A component the shader does not write reads 0 (w 1).
- `res.json` gains `"mesh": {threads, payload_words, max_vertices, max_primitives, outputs: [locations],
  per_primitive_outputs: [locations], stage_memory: bytes}`; `stage` is `"ts"` / `"ms"` as sent.

What the runtime has to do (contract changes against `e31d224` / `efbb06a`; runtime files are not changed here):

- **Required**: `gfx_shader.c` `reflection_valid` accepts only `"stage"` `"vs"`, `"ps"` and `"cs"`, so every task and
  mesh translation is thrown away as "res.json is malformed, translating again" and the draw is skipped (43 shaders,
  2,395 attempts in a 245 s run of build/oot with this translator). Under `GAME_NS2` it must also accept `"ts"`, `"ms"`
  (and `"gs"`), as `reflect` already does:
  `!strncmp(st, "\"ts\"", 4) || !strncmp(st, "\"ms\"", 4) || !strncmp(st, "\"gs\"", 4)`.
- Nothing for the request: `ns2_request` already sends what the translator reads, and c[0] + 0x9F0 is read as the
  runtime's comment says. The runtime must keep filling c[0] + 0x9F0 for task shaders and for mesh shaders drawn
  without a task.
- Per-primitive attributes (4 of OoT's 500 mesh shaders, all agl `AglShader.sharcb`): their second SPH is the first
  0x80 bytes of the c1 segment in all four, which the translator uses when `c1` starts with an SPH (type 1, version 4).
  Since patch 0048 they go out as plain vertex outputs (each primitive on vertices of its own), which the fragment
  shader, translated without knowing its mesh shader, reads as the flat inputs it declares; nothing for the runtime.
  Only a shader whose primitives would need more than 256 vertices so keeps `PerPrimitiveEXT` outputs (none in OoT):
  its fragment shader's inputs at `res.json mesh.per_primitive_outputs` would need the same decoration.
- Fragment inputs the mesh shader does not write (`res.json mesh.outputs`): there is no vertex-output fix-up for mesh
  pipelines; Vulkan rejects a fragment input with no matching output.
- Subgroups: the kernel assumes 32 lanes (as compute); `stages_create` pins 32 only for non-mesh pipelines. On NVIDIA
  this is the native width; elsewhere `VkPipelineShaderStageRequiredSubgroupSizeCreateInfo` 32 should be chained for
  task and mesh stages where `requiredSubgroupSizeStages` allows it.
- Limits OoT reaches: mesh stage memory up to 21,648 bytes with payloads up to 3,200 (Vulkan guarantees 28,672 for
  payload plus shared memory), mesh outputs up to about 29.7 KB (guaranteed 32 KB before granularity), 13 generic
  attributes, 160 vertices and 128 primitives.

## Reply

The reply line and the files are as for Maxwell (`ok <key>` / `fail <key> <message>`; `<key>.spv`, `<key>.res.json`).
What an Ampere translation adds:

- `res.json` `notes`: the translator's own messages, prefixed `translator: ` (Maxwell jobs do not get them). OoT's
  vertex shaders all log `Attribute offset 0x5C is not valid.` (an output Ampere code writes and the Maxwell attribute
  map does not have; the store is dropped).
- `res.json` `cbufs`: slot 24 is the push constant block (NVN2_PUSH_BANK), bound like the other uniform buffers.
- `res.json` `sbufs`, with `gmem_map`: `cb_slot` 255 (`GlobalToStorage.GmemMapCbSlot`) marks the pool table
  (`cb_offset` 0, read) and its write log (`cb_offset` 1, `flags` with the write bit). The table is 4096 entries of
  8 u32: start (lo, hi), end (lo, hi), delta (lo, hi), hint, 0; sorted by start, entry 0 empty (start 0, end 0),
  unused entries with start ~0. An address in [start, end) is accessed at address + delta (64-bit; the host makes it
  the buffer device address of its copy of that pool) through a `PhysicalStorageBuffer` pointer. The hint (word 6 of
  entry `c & 4095`, c = address >> 21) is `(c << 12) | entry` when the whole 2 MiB chunk c lies in that entry, else
  0; it is only tried below 2^41. The write log is a bitmap of 4096 bits (128 u32): a store or atomic through entry e
  sets bit e. The module then declares `PhysicalStorageBufferAddresses` and `Int64` and uses
  `SPV_KHR_physical_storage_buffer`: the device needs `bufferDeviceAddress` and `shaderInt64`.
- `res.json` `mesh` for task and mesh shaders (above).
- Storage images without a known format declare `StorageImageReadWithoutFormat` as well as
  `StorageImageWriteWithoutFormat` (315 and 313 of OoT's modules).
- Ampere code assumes 32-lane subgroups (shuffles, ballots, the HMMA scratch blocks): the pipeline must pin the
  subgroup size to 32, or the job must pass `subgroup`.

SPIR-V of every OoT translation validates with `spirv-val --target-env vulkan1.3`; besides the capabilities above the
modules declare `Float64` (unless `softfloat64`), `DrawParameters` (vertex), `FragmentShaderPixelInterlockEXT`
(pixel, as Maxwell's) and the group non-uniform ballot, shuffle and vote capabilities.

## Instructions found on OoT and their basis

- F2IP (0x243; patch 0023): two floats converted to saturated unsigned bytes and packed as PTX `cvt.pack` packs
  (d = c[15:0] << 16 | u8(a) << 8 | u8(b)), NaN as 0, rounding at 78..80 as F2I's (0 to nearest even, 3 towards zero).
  OoT's 8 foliage and 2 agl compute shaders use the register form rounding to nearest (upper word `0x000004ff`);
  zortos293/Lichii `35c043b3` (GPL-3.0, read for behaviour only) lowers the truncating form `0x0000c4ff` with a = RZ to
  the same byte. Not checked on hardware: the order of a and b in the result (taken from `cvt.pack` and F2FP.PACK_AB).
- ISBERD mode 0xb8 in a task shader (0024): a read of the task's own output payload (`5d3a096025e375a1` writes its
  workgroup number at payload + 0x10 and reads it back from there).
- ATOMG.CAS (0x3a9), RED / ATOMG `.F32.FTZ.RN` adds (0022): see the patch header.

## Not supported

- Geometry shaders (`"gs"`) with `sm86`: OoT has none; Program.cs has no input topology to give them.
- agl_common's compute shader `68bbe98b3bfd9876`: its BAHS entry pairs a compute control section with a code area that
  starts with a vertex SPH; it fails at offset 0 (`instruction 481`). It is probably a pairing of the container, not a
  shader the game dispatches.
- Task / mesh shaders with shared memory of their own, more than 128 threads, more than 256 vertices or primitives, or
  no position output: refused with a message (none in OoT).

## Verifying

The corpus is every NVN2 shader of the RomFS files that hold them (`romfs/Shader`, `romfs/Lib`, `romfs/Effect`,
`romfs/Layout2Shader`, `romfs/Movie/Ndve`; zstd frames with the dictionaries of `Pack/ZsDic.pack.zs`): BNSH programs
(their NVN2 task and mesh slots 7 and 8 included), agl's BAHS and NDSR packs and the movie library's NDVE pack, read
as seedds/nxnative `dm/gfx/tools/shader_split.py` reads them, keyed as the runtime keys shaders (sha256 of the control
section's first 0x48 bytes with +0x30 zeroed, and the code area). The modules hold none. 42,851 distinct shaders:
15,852 vertex, 22,356 pixel, 4,123 compute, 500 mesh, 20 task (the same key set as nxnative's `shader_map.tsv` for the
game). Each was translated with the default bind state (no `tex`/`img`/`attr`/`fout` answers) and the fields
`ns2_request` sends (`c1`, `local`, `shared`, `nvn2_cs` with seven entries, `mesh`), once with and once without
`gmem_map`, and every module checked with spirv-val from the Vulkan SDK 1.4.363.1 (SPIRV-Tools v2026.4, `--target-env
vulkan1.3`).

| | vertex | pixel | compute | task | mesh |
|---|---|---|---|---|---|
| translated, valid | 15,852 | 22,356 | 4,122 | 20 | 500 |
| failed | 0 | 0 | 1 (`68bbe98b3bfd9876`) | 0 | 0 |

Maxwell: 3,763 TotK 1.4.3 jobs (the 766 variants of a play session's `shader-answers.bin` and 2,997 drawn from the
prepared `seed-answers.bin`, programs from `shader-programs.bin` / `seed-programs.bin`, requests as `build_request`
writes them) translate to byte-identical SPIR-V and `res.json` with this translator and with one built from
0001-0016 alone; all 3,763 modules pass spirv-val.

## NN noise (2026-11-06)

The title movie and the 3D frame go through agl's NDVE network: 12 compute passes, from the YUV to RGB frame (tex 1938,
1920 x 1080 RGBA8) to an RGB10A2 image (tex 1942), then an upscaler to the 2560 x 1440 output. Two translator bugs
garbled it: loads moved past stores (patch 0025) broke the network, and dead code removal across a two-destination
HMMA (patch 0026) broke the upscaler; the 3D frame's pass before its upscaler also needs integer image views (patch 0027
and the runtime). The passes, with the game's buffers (NVN agent dump `fdump3/passes.txt`) and the
shapes the probes below found:

| pass | shader | in | out | weights |
|---|---|---|---|---|
| 1 | `ba079f68ae288dba` | frame | 960x540x16 | 6x6 stride 2, RGBA to 16 |
| 2, 11 | `a8b4fb48fee0ccf8` | 960x540x16 | 960x540x16 | 3x3 |
| 3 | `e2eebd7ba18fe35f` | 960x540x16 | 480x270x32 | 2x2 stride 2 |
| 4, 9 | `e6be82d835fbe4d3` | 480x270x32 | 480x270x32 | 3x3 |
| 5 | `9442573ee4ace011` | 480x270x32 | 240x135x64 | 2x2 stride 2 |
| 6, 7 | `0a56002cdc7e1fce` | 240x135x64 | 240x135x64 | 3x3 |
| 8 | `d15370a382630d4e` | 240x135x64 | 480x270x32 | 2x2 transposed, plus the skip input (pass 4) |
| 10 | `7f7ff0f36b54d90f` | 480x270x32 | 960x540x16 | 2x2 transposed, plus the skip input (pass 2) |
| 12 | `5a7d69b067a7f011` | 960x540x16 | image | 3x3, 16 channels to 2 x 2 pixels, plus the frame |
| 13 | `4f41ccf24f163e0d` | image | 2560x1440 RGBA8 | upscaler: 4 warps, 24 x 6 input pixels to 32 x 8 a workgroup, 46 HMMAs, MOVM, HMNMX2, SHFL; weights in a 12 KiB storage slot (c0 + 0x330), blue noise (tex 1937) for dithering |

Activations are f16, NHWC.

### Cause found: loads moved past stores (patch 0025)

The AST optimizer (`StructuredIr/AstOptimizer.cs Optimize`, upstream Ryujinx) moves an expression that has one use to
that use, also a load from memory, also past later stores to the same memory. The runtime sends `"debug": false`, so
this runs on every translation; seedds/nxnative sends `debug` on by default (`dmsh_build.py`, `XLATE_DEBUG`), which skips
the optimizer, so its Metal build never met this. The network shaders stage their weights through one shared buffer,
four blocks per warp: `STS.128` of block k, `LDSM` of it, `STS.128` of block k + 1 to the same place, and so on
(`7f7ff0f36b54d90f` instructions 72-87, no barrier: one warp runs them in order). After the optimizer every `LDSM` load
sat after the last store, all four HMMA groups used the last block's weights, and passes 6-12 computed garbage.

The fix keeps such a load in place for Ampere jobs (`StructuredProgramContext.OrderedMemoryLoads`, set with the Ampere
flag; `AstOptimizer.MutableReads`, `WritesMemory`, `UseFollows`): an expression that reads shared, local, storage or
global memory, outputs or storage images moves to its one use only when no store, atomic, barrier, call, fragment
interlock or vertex emission that may write that memory lies between them. Shared and local arrays are told apart by
id; storage and global memory count as one, since two storage slots can be the same buffer (the network's input and
output are, in passes 2, 11 and 12). Maxwell jobs keep Ryujinx's behaviour.

It is `tools/xlate/patches/0025-ampere-ordered-memory-loads.patch` (`AstOptimizer.cs`, `StructuredProgram.cs`,
`StructuredProgramContext.cs`); the translator built with it alone had the identity `1552f13167f3edbd…` (JIT build as
before: no MSVC here).

Measured:

- `STS.128` / `LDSM` four times over one buffer (`t_interleave.py`): every load read the last block before, all
  correct after (also with `debug` on).
- Whole network replayed on the RTX 5070 (`netrun.py`, random weights): all 12 pass outputs bit-identical to the same
  translation in DebugMode; before the fix passes 6-12 differ.
- SPIR-V of the ten network shaders: +0.15 % in total (`7f7f` +1.0 %, `4f41` +2.9 %, `ba07`, `a8b4`, `e2ee`, `e6be`,
  `9442` unchanged).
- OoT corpus (42,851 shaders, with and without `gmem_map`): every one translates and passes spirv-val as before (the one
  compute failure stays); changed modules: 1,144 vertex, 977 pixel, 2,260 compute, 198 mesh, 1 task.
- Maxwell: the 3,763 TotK jobs of "Verifying" give byte-identical SPIR-V and `res.json` (the patch is behind the Ampere
  flag).
- In game (the coordinator, dev2 build through `REKARINA_XLATE`): the title movie is recognisable (the field, the hero on
  The horse, the ranch). Left: horizontal blocks about 16 x 4 px at 1920 x 1080, brightened towards white; in the sky
  (top 12 %) 14 % of the pixels are pure white and 6 % pure black (`build/oot/shots_fix3`, `build/oot/fix3.log`).
  The first run compiled the network pipelines for 132 s (a pipeline cache miss on the changed modules, not growth; see
  below).

### Checked and correct

Tests in `C:\Rekarina-tmp\nntest` (Python `vulkan` in `C:\Rekarina-tmp\venvvk`; programs assembled from the network
shaders' own instruction words, `asm.py`):

- `HMMA.16816.F16` (the `dmsh_hmma16816_f16` helper) against PTX `mma.m16n8k16` on random f16: exact.
- `LDSM` x1, x2, x4, plain and `.MT88`, against PTX `ldmatrix`: exact.
- One lane's `STS` read by another lane's `LDS` with no barrier (`t_lanes.py`): never stale.
- One-hot weights (one weight 1, the rest and the bias 0) on random input: every pass from 1 to 12 writes one output
  channel equal to one input channel at one offset, as a convolution must (`t_pass1.py`, `t_passes.py`, `t_pass12.py`).
  Pass 1 on the real frame: the 144 taps of output channel 0 are 108 distinct RGB taps of a 6 x 6 window and 36 alpha.
  Pass 12 with zero weights writes the frame (error below one RGB10A2 step). Passes 8 and 10 copy their skip input
  exactly. Pass 10 reads only the first 4 KiB of its 16 KiB weight buffer.

The replay needs the support buffer as the runtime fills it (`vk_exec_draw.c support_fill`): with render scale 0 (offset
192) Ryujinx's texel fetch scaling makes every coordinate 0, and pass 1 read texel (0, 0) everywhere. That was a harness
error, not a translator one; with the scale at 1 the passes above check out.

### Residual dashes: the upscaler lost its HMMAs (patch 0026)

With 0025 the movie was recognisable but kept horizontal dashes, worst in a band across the top and where the frame is
bright. A private game run with `REKARINA_DUMP=1500` and `REKARINA_DUMP_BUF=0x195eea000:0x27C9320` (the weights and the whole
activation arena; `vk_dump.c buf_snap` copies the range only when it lies in one pool, so a range starting at
`0x195e00000` gave nothing) put the arena after every pass in `C:\Rekarina-tmp\oottest\dump4`. Each of passes 1-11,
replayed in the harness from the game's arena before it with the game's weights and frame (`gamecmp.py`), is
bit-identical to the game's arena after it, every row; the values are small (largest magnitude 1 to 12) and finite.
Pass 12's image (tex 1942) is clean; pass 13's image (the 2560 x 1440 output) has the dashes. So neither the network,
nor the pool table, nor the mirrors were at fault.

The translation of `4f41ccf24f163e0d` had one HMMA left of 46, called with 11 uninitialised arguments, and its output
changed from run to run in the harness (66 % of pixels; with `zero_locals` the image was clean and 1.3 % changed).
`Optimizer.RemoveNode` (upstream Ryujinx) follows a removed node's sources into their producers and strips the
producers' own sources' use lists once a source has no uses left. An HMMA is one operation with two destinations
(Rd, Rd + 1). The upscaler's last HMMA writes R16 and R17, and a `HADD2.F32` overwrites R17 before anything reads it, so
the copy of R17 was removed, the HMMA's sources lost their uses although R16 was still used, and the next pass deleted
the lane and block computation, the LDS, LDSM and MOVM results and the 45 HMMAs before it. The network passes read both
halves of every HMMA result, which is why only the upscaler was hit. Patch 0026 follows a producer only when every one
of its destinations is unused; a single-destination producer is followed as before.

Measured with 0026: the upscaler's SPIR-V has its 46 HMMA calls (64 KiB to 189 KiB) and passes spirv-val; replayed on
the game's inputs (pass 12 from the dump; push constants reconstructed from what the shader reads: c[24] [0].zw = 1919,
1079, [2].zw = 1 / 2560, 1 / 1440, [4].zw = 1439, 0, [5].x = 0) it is the same from run to run and gives a clean
upscale whose mean colour matches its input. In game (private copy, frame 1500 dump): the upscaler's output is clean
(no pure white pixels, 0.0008 % pure black, no band). Maxwell: the 3,763 TotK jobs stay byte-identical, so the fix is
not gated. OoT corpus, with and without `gmem_map`: every shader translates and passes spirv-val as before; 8 compute
modules change, 5 of them get their HMMAs back (`4f41ccf24f163e0d` 1 to 46, `54c6b94ddf539d9f` 55 to 220,
`927ceaf3cf1be240` 8 to 176, `aa1243f635baa8e8` 5 to 88, `fa3033d05c0a41d6` 8 to 176: the forest cutscene's upscaler, pass
243 of the coordinator's `kdump3`), 3 keep sources of another multi-destination operation (`04942f42cae10d0f`,
`69c96afef37c6010`, `9dc103f230b6cfde`, a few hundred bytes each). Translator identity with 0025 and 0026:
`753348a107618b64…`.

Also measured on the way: the whole network replayed twice on the real frame is bit-identical (no race), and the runtime
puts a barrier before every dispatch (`vk_exec_draw.c op_dispatch`).

### The 3D frame's pass before the upscaler: integer image views (patch 0027; runtime side open)

In the coordinator's forest cutscene dump (`kdump3`, frame 17476) the pass before the upscaler, 242
(`a3a11cb3b5a9758b`, corpus `a335c1db6270c355`, no HMMA, the same SPIR-V before and after 0026), already writes a broken
image: red only, largest 65024, 34 distinct values, 200,171 NaN pixels. Its code packs three square-rooted channels as
shared-exponent words (9-bit mantissas at bits 0, 9 and 18, a 5-bit exponent at 27, RGB9E5's layout) and writes each word
as the X component of a formatted SUST to image slot 2, which the game binds through an NVN view of format 0xB (R32UI;
`view-nvn-fmt 0xb` in the dump) on a texture the runtime knows as R11G11B10F (294). The translation declares that image
without a format and stores `vec4(intBitsToFloat(word), 0, 0, 0)` (`ShaderProperties.GetTextureFormat` answers Unknown
because the host loads formatted images); converted into an R11G11B10F view, those words give exactly the dump's
statistics. What it needs: an R32_UINT view on that image (the NVN view format), the request's `img` answer for the slot
with `"fmt": "R32Uint"`, and a translation that takes that answer. Patch 0027 does the last: with the answer the store
becomes `imageStore(uimage2D r32ui, uvec4(word, ...))` (checked with spirv-val; the slot is now in `img_queried`);
without it the SPIR-V is the same as before. Measured with 0027 (translator identity `385051813205e543…`): the 3,763
TotK jobs stay byte-identical; on the OoT corpus, with and without `gmem_map`, every module is byte-identical to 0026's
and still passes spirv-val (no answers there), and 225 compute and 90 pixel `res.json` files gain the slots of their
formatted surface accesses in `img_queried`. The view and the answer are the runtime's (being done by the coordinator).
The upscaler (pass 243) then gathers the texture as floats, so the sampled view must decode the words as RGB9E5; the
dump names that texture R11G11B10F too. Pass 241 also stores through an R32UI view (`tex 293`, RGBA8).

### Mesh and network compile time

The mesh wrapper (`MeshStageWrapper.cs`) is about 130 SPIR-V instructions of loops (zeroing, payload copy, vertices,
primitives, indices): no unrolled stage-memory copies and no switch. The size is the guest code: the largest mesh
shader, `e9db800a4acc9aec`, has 18,432 Ampere instructions, 8,976 of them predicated, and becomes 2.2 MB of SPIR-V with
9,639 selection constructs and 15,800 function-scope variables (6 of the 500 mesh shaders are over 1 MB). The network
modules are 0.1 to 1.7 MB; `d15370a382630d4e` calls the HMMA helper from 768 places, each with its arguments copied to
variables, and the driver inlines every call. Trimming that (fewer call copies, or `VK_KHR_cooperative_matrix`) is for
later.

## Mesh shaders that output points (patch 0040, 2026-11-14)

Seven of OoT's 854 mesh shaders (`Foliage.Nin_Ounce_NVN2.bfsha`) draw the grass into a shadow map of its own as points:
32 threads, a blade each, the vertex is the blade's tip and its point size comes from the blade's height
(`c4[31].x`, 6 texels at most). Their control section has 1 at +0x5f where every other mesh shader has 3 (the vertices
of a primitive), and their SPH's OMAP byte 0x35 is 0xf8: the point size (bit 3) before the position. The code writes
the size to attribute block 0 and x, y, z, w to blocks 1 to 4, one index byte a point.

Until 0040 the stage description took the position as the first component and the entry point every shader as
triangles. So the size was x, the position's x, y and z were y, z and w, and the primitive bytes (placed after four
blocks) lay inside the w block: the count byte was the top byte of a float and the index bytes were bytes of w. The
draw rasterized a few triangles between unrelated blades and the map's centre, a wedge over half the shadow map that
changed with the order of the frame's grass list. On screen: large sun shadows that came and went from frame to frame
wherever grass grows (docs/oot-notes.md, 2026-11-14).

The request's `mesh` object gains `"prim_verts"` (control +0x5f; absent means 3). `Program.cs` lists the system values
of byte 0x35 in the attribute area (`MeshStage.PointSize`; bits 0..2 as `MeshStage.Unused`, a block that goes
nowhere), and `MeshStageWrapper` declares the topology, the index built-in and the `PointSize` member by them.

The cache: the patch changes these seven translations and no other, so `tools/build_xlate.py` keeps the translator
identity of patch 0039 (`SAME_AS`: the hash of today's sources maps to the earlier identity, and any later source
change ends that by itself), and `gfx_shader.c build_code_hash` ends the hash of a mesh program with point output with
a tag, which gives exactly these programs new input ids and cache keys. A player's prepared cache stays valid; the
seven are translated when first drawn. Checked by translating every stored input of the test build with the
translators before and after the patch into fresh caches under one made-up identity (`REKARINA_XLATE`, `REKARINA_XLATE_ID`,
`REKARINA_SHADER_CACHE` with the pack linked in; the prewarm translates what the cache lacks, ten minutes a run): 51,115
translations each, 102,230 files, and the ten that differ are mesh shaders of 32 vertices and 32 primitives, the
variants of the seven. (One vertex shader's `res.json` differed in the provisional key it was first written under.)

## A primitive's indices stored whole (patch 0041, 2026-11-14)

The entry point let thread n store index byte n: `mesh_indices[n / 3][n % 3] = byte`. A triangle's three components
so came from three threads, each a store to one component of the same vector. AMD's Windows driver for RDNA 1 and 2
("AMD proprietary shader compiler", 2026.04.17, on an integrated Radeon) loses such stores: every mesh draw of the
game came out empty there, which was the missing tree crowns, bushes, ground plants and their shadows. With
`"mesh": {"whole_indices": 1}` thread n builds primitive n's vector from its bytes and stores it once
(`MeshStage.WholeIndices`), and the same frame shows them. The compare-and-swap loop of the byte stores is not what
was lost: it runs there as it is.

The runtime asks for it on every device but NVIDIA's (`vk_device.c`, `REKARINA_MESH_INDICES=auto|whole|split`), where
the split stores have run since the first mesh draw and stay, so no NVIDIA translation changes. Where it is on, the
program hash of a mesh program ends with a tag (`gfx_shader.c build_code_hash`), so on those devices the mesh shaders
get new cache keys and nothing else does; the translator identity stays (`tools/build_xlate.py SAME_AS`).

A draw through a task shader (the short grass) stopped the GPU or crashed that driver as soon as it emitted a
primitive, and the runtime left those draws out there until 2026-11-15. The trigger is the GPU render condition the
game puts around them: drawn outside conditional rendering they work there (`vk_exec_draw.c mesh_task_cond`,
`REKARINA_MESH_TASK_COND`; the integrated Radeon shows the grass and three runs, one of five minutes running through the
forest, had no fault). The grass an occlusion query would cull is drawn anyway on that driver. What was tried before
is in `tools/xlate/experiments/README.md`.

## VOTEU's predicate is a uniform one (patch 0042, 2026-11-15)

The whites of the hero's eyes were cyan (seen on the pause menu's equipment page, where the model stands close). The
eye material is drawn forward, and its first light is the object's light probe: where the probe is not empty the
shader scales the light per channel by the probe (the usual ratio of the first band to the constant term, a power of
the cosine). The NVN2 compiler wrote the "probe is not empty" test as

    VOTEU.ANY UR63, UP0, P0     ; UP0 = any lane's P0
    PLOP3     P0 = UP0          ; (lut a & b & c with a = b = PT, c = UP0 by its uniform bit 67)

and guards the red channel's instructions with P0 afterwards. `AVote` wrote the vote into the lane predicate of that
number for VOTE and VOTEU alike, so UP0 kept what an earlier, unrelated test had left in it (false here), P0 became
that, and the red factor stayed the register's old content: -0.29 on that page, a light of (-0.79, 4.19, 8.0) where
the scene's is (2.72, 2.81, 2.96). Green and blue take the vote by other instructions and were right. Now a VOTEU's
vote goes to the uniform predicate, as its ballot goes to a uniform register. The culling kernels' votes name UPT and
are unchanged.

How it was found: a frame dump of the page (`REKARINA_DUMP`), the eye draw's module decompiled with spirv-cross, and the
module's colour output replaced by three values of choice at a time (`REKARINA_SPV_FILE=<hash>=<file>`, a copy of the
dump's `sh_<hash>.spv` edited by a script; the pass target read back as floats with `REKARINA_DUMP_ONLY` and
`REKARINA_DUMP_RAW4`): final colour, then its terms, the light loop's sum, the first light's colour, the factor, the two
conditions. 393 of the corpus's 42,851 programs have a VOTEU with a predicate other than UPT. Their program hash ends
with a tag (`gfx_shader.c build_code_hash`, `nvn_ctl.h nvn2_ctl_voteu_pred`), so they are translated anew and every
other cache entry stays; the translator identity is kept (`tools/build_xlate.py SAME_AS`).

## Last frame's position (patch 0045, 2026-11-21)

Per-object motion vectors draw a model a second time with its vertex shader's last-frame variant (patch 0015,
`Transforms/MotionPrevious.cs`): every operation the position depends on that reads a user bank c3..c16 is repeated
reading slot bank + 15, where the runtime binds that bank's block of last frame (`vk_exec_draw.c` `ubo_info`,
`vk_skin_motion.inc`). Program.cs answered `mvprev` with false for every Ampere job, because NVN2 shaders read their
push constants as c[24], which is c9's last-frame slot. Without the variant every skinned draw of OoT kept the camera's
motion: the runtime's model motion summary counted all of them as "position reads other blocks".

The transform now collects the constant slots the whole shader reads (`ConstantSlotsRead`) and declines only a
position whose banks need one of them; it reports the banks with location -1, so the shader's `.res.json` shows why
(`mvprev` -1 with `mvprev_banks` set), and the runtime, which takes a variant only when it adds an output, leaves that
draw to the camera. The bindings of the last-frame slots are made after that check, in the same order as before, so a
Maxwell translation (whose slots stop at c17) is byte for byte the same, and so is every job without `mvprev`. The
translator identity is kept (`tools/build_xlate.py SAME_AS`); the keys of NVN2 last-frame variants carry a tag
(`gfx_shader.c put_mvprev_tag`), so the plain translations the cache holds under their old keys are never found.

Checked on the RTX 4090 at 4K DLAA, booting a save in the hero's house (`--load-save latest --seconds 100`): of 292,940
skinned draws, 113,294 were drawn with their own motion (39 %), 179,170 were carried by another draw of the same mesh,
and none was refused for "position reads other blocks" (all of them before the patch). The 48 variants translated
there all wrote the position (banks c3 c4 c6, or c3 to c7); none depended on c9. No shader failed.

## The foliage's raw global-memory helpers called, not inlined (patch 0046, 2026-11-22)

A job with `gmem_map` reaches an address traced to no storage buffer through helper functions that look it up in the
memory pool table (`GlobalToStorage.TryGenerateRawStorageOp`, functions named `*Raw`, `LoadMultiRaw` the commonest).
The translator emits every helper with `FunctionControl` None, and the driver inlines every call. Eight of the game's
mesh shaders, the forest's foliage, call them 950 to 982 times (179k to 193k SPIR-V words): inlined, they reach the
register ceiling of 255 and 1 to 2 MB of machine code. Called instead, the foliage takes some 45% less GPU time there
(about 1.1 ms a frame). On the smaller mesh shaders the calls cost up to 29%, so only those eight ask for them.

The patch marks the `*Raw` helpers `DontInline` in a mesh shader (not a task shader) whose `MeshStage.Large` is set.
`tools/xlate/Program.cs` `MakeMeshStage` sets it for a code file (control +0x38, the SPH included) of 60,000 to 70,000
bytes, and `nvn_ctl.h` `nvn2_ctl_mesh_large` tests the same length. A scan of the 857 mesh inputs of a build's shader
cache pack (`shader-cache.pack`) set the band:

| Code file (bytes) | Programs | Calls | What |
|---|---|---|---|
| 184,704 to 294,912 | 10 | 22 to 38 | every buffer access resolves to c0: no `*Raw` helper |
| 60,288 to 69,376 | 8 | 950 to 982 | the foliage: one `DontInline` helper each after the patch |
| 62,848 to 62,976 | 6 | 30 | in the band, but no `*Raw` helper: translated byte for byte as before |
| 46,592 and below | the rest | up to 550 | medium mesh shaders that inlining serves better |

A size larger than 0x20000 bytes, the first gate tried, selected the ten c0-only programs and changed nothing. The
programs in the band end their program hash with a tag (`gfx_shader.c` `build_code_hash`, `TKML`; `tools/prepare_shaders.py`
`code_hash` writes the same), so a cache made before the patch is not used for those 14, and every other entry keeps
its key; the translator identity is kept (`tools/build_xlate.py` `SAME_AS`).

Checked with the pack's own job flags (the old translator's output is in the pack for all of them): each of the eight
foliage programs has one function with `FunctionControl` 2 and passes `spirv-val --target-env vulkan1.3`; the six
other programs in the band and every mesh shader outside it translate to the same bytes as before.

## Warp collectives on subgroups wider than 32 lanes (patch 0044, 2026-11-20)

An RX 570 (GCN 4, AMD's Windows driver 26.5.2 for Polaris) drew the great tree's rooms without walls, a jump
platform, a fire, a spiked log in the water and the door frames, and its log shows that the game issued no mesh draw
there in 5,300 frames, where the RTX 5070 issues 48 a frame: what the game draws there follows from what its GPU
culling wrote.

GCN runs compute 64 lanes wide and cannot be pinned to 32 (`subgroup size control 64-64`), so compute is translated for
64 lanes there ("translator subgroup vs/fs/cs 64/64/64"); every other GPU the game has met pins compute to 32 (RDNA,
Intel) or has 32 lanes (NVIDIA). Ryujinx's own warp instructions (VOTE, SHFL, the lane masks) emulate a 32-lane warp in
a 64-lane subgroup by the lane id's bit 5. The Ampere instructions the Switch 2 front end added do not: REDUX, MATCH and
the per-lane HMMA, LDSM and MOVM read other lanes with shuffles by lane number 0..31, and REDUX and MATCH take the
active lanes as ballot component 0. A shuffle's index is a lane of the subgroup, so the second warp of each subgroup
reduced and matched the first warp's values. In a build's 51,147 stored inputs REDUX is in 159 compute kernels, MATCH
in 45, REDUX in 20 task and 64 mesh shaders, neither in a vertex or fragment shader.

The patch gives those instructions in a compute shader the lanes of their own warp: the shuffle index is the lane id
with its low five bits cleared plus the lane number (`AWarpFirst`, `AWarpLane`), and the active mask is `EmitBallot`'s,
the warp's 32 bits (`AWarpActive`). The game's task and mesh workgroups are 32 threads, one warp to a subgroup, so those
stages stay as they were (and an RDNA card's mesh pipelines are not built again for nothing). With 32 lanes, or in
another stage, the patch emits nothing: those translations stay byte for byte the same, and the translator identity is
kept (`tools/build_xlate.py SAME_AS`). `gfx_shader.c build_code_hash` ends the hash of a compute program with those
instructions with a tag where compute is translated for more than 32 lanes (`xlate_lanes`, `nvn_ctl.h
nvn2_ctl_warp_lanes`), so only a GCN card's kernels of that kind are translated anew.

Checked on the RTX 5070: the same 70 s (the forest, then the great tree) with the translators before and after the
patch, each into an empty shader cache: 992 SPIR-V modules in both, all byte for byte equal, among them 21 compute
kernels with REDUX, 11 with HMMA, 10 with LDSM, one with MOVM and 10 task or mesh programs with REDUX (the files that
differ are the run's own: provisional keys in `res.json`, the answers of stored inputs, the use masks).

## The 2.0 translator set (2026-11-23)

Upstream's version 2.0 was allowed to translate every player's shaders again, so the translator changes that had waited for it
were gathered: patch 0048 below. It did not need it: it changes the 12 mesh programs with per-primitive
outputs and their emulation kernels, which `gfx_shader.c build_code_hash` keys apart (tag `TKPP`). Translated
with the shipped translator and with this one: 31,598 shaders of a corpus of 42,851 (all the harness can form; the rest,
mostly fragment shaders that need the game's answers, translate with neither), all byte for byte the same; the 1,085
mesh programs of a build as the runtime asks for them, with and without whole index vectors, 1,073 the same and the 12
above different; their emulation kernels the same, the 10 of those 12 with triangles now translated; the 71 kernels with
HMMA, without `coop_matrix` and with NV's, the same. So the set keeps the shipped identity (`tools/build_xlate.py
SAME_AS`), and a player's prepared shader cache and driver caches stay: the update runs no shader step
(`tools/update_cost.py`), and the game translates the 12 programs anew when it first draws them. Shown in the temple's
block room with a cache folder that held only a pack the shipped translator made: the prewarm translated nothing, and in
play the set's build translated the two per-primitive programs drawn there and the same 19 shaders the shipped build
also had to translate (the pack lacked them), those byte for byte as the shipped translator wrote them.

In this fork the patch is 0048 (upstream's 0046, after this fork's own 0045 and 0046, with upstream's 0045 as 0047) and
the identity is kept only once this fork's translator passes the same comparison: until then `SAME_AS` has no entry
for its sources and an update would translate every shader again.

Left out of the set: the clustered lighting's light records read 16 bytes at a time (the three heaviest passes
on weak GPUs): the same shaders with 16-byte loads took 15.3 / 11.8 / 7.3 ms on the integrated Radeon
against 14.8 / 11.8 / 7.1 without (the round trip through GLSL both times), and capping the loop at two lights a tile
did not move them either: their ~7,700-instruction body is bound by arithmetic, not by the loads.

## Per-primitive outputs after the vertex blocks (patch 0048, 2026-11-23)

Players saw the pictures of the ghosts and the tops of the push blocks in the temple in the woods drawn flat green. Four
of agl's mesh shaders (`AglShader.sharcb`; 12 of a build's 1,085 mesh programs with their variants, ten of triangles and
two of points, three with a task shader) have per-primitive attributes. The most used of them draws 128 vertices and 64
primitives a workgroup, with six vertex components (the position and location 2's x, y) and four per-primitive ones
(locations 0 and 1 x, location 3 x, y). The guest code writes vertex v's
component k at byte (v / 32) x 768 + k x 128 + (v % 32) x 4 of the attribute area and primitive p's component j at
3072 + (p / 32) x 512 + j x 128 + (p % 32) x 4 (the translation of `baf3d07b0d451a84`): every vertex block first,
then every primitive block, from the end of the last vertex block (max_vertices / 32 of them, rounded up). The entry
point read them interleaved, each block's per-primitive words after its vertex words (the layout the nxnative lowering
came with), which agrees only without per-primitive components or with one block: every vertex from 32 on and every
per-primitive word came from other words. MacNativeGames found the same in their port (their issue 2214, "mesh shader
primitive outputs read from the primitive blocks"); this is our own code. `MeshStage.VertexBlockWords`,
`PrimitiveBlockWords` and `PrimitiveAreaWord` give the layout; the primitive bytes keep their place, which lies past the
area for any counts.

The per-primitive attributes also went out decorated `PerPrimitiveEXT`, which the fragment shader's inputs at those
locations must carry too, and nothing gave them that (the fragment translation does not know its mesh shader). They now
go out as plain vertex outputs (`MeshStage.PrimitiveOutputsPerVertex`): each primitive gets vertices of its own (output
vertex n is corner n % k of primitive n / k, k vertices a primitive), which carry its corners' vertex words and its own
per-primitive words; the fragment shaders drawn with these programs declare those inputs flat (NVN2 marks them
constant; checked on the two the frame dumps below show, `a75289082a4d738b` and `fefe386d01dd83d0`), so every point of
a primitive reads its value exactly. 64 triangles take 192 of the 256 vertices every device outputs; a
shader whose primitives would need more keeps `PerPrimitiveEXT` outputs (none in OoT). The mesh emulation
(`MeshStage.AsCompute`) takes these shaders too now, where Program.cs refused them before (their draws were left out on
GPUs without mesh shaders): each triangle gets three vertices of its own in the emulation buffer, as above
(`MeshStage.EmuMeshletVertices`, `EmuLocations`).

Checked: the 1,085 mesh programs of a build's `shader-programs.bin` translated with the shipped translator and with this
one, as the runtime asks (and with `whole_indices`, and as emulation kernels): 1,073 byte for byte the same, the 12 with
per-primitive outputs differ and pass spirv-val; as emulation kernels, the 10 of triangles translate now (the two of
points are still refused).

In the game (the reporting player's save at the block puzzle, 1080p base mode; the patched modules put in place of the
shipped ones with `REKARINA_SPV_FILE`, so that nothing else differs): the picture is painted every frame. A pass renders
its subject into a 1024 x 1024 target, two draws of a task and mesh shader with one per-primitive colour (mesh
`59bd5ef12a84bc6e`) lay brush strokes from it into a second target, and a third pass composes both into the texture
that the block tops and the picture sample. With the shipped translation the strokes' target is one flat green (0, 0.63,
0) in a frame dump of the RTX 5070, and so are the block tops on screen: green minus red on a block's top 99 to 105 of
255 on every shot, against 19 to 23 with the patched modules, whose tops show the strokes. The mesh emulation
(`REKARINA_MESH_EMULATE=1`) left those draws out before (the picture without its strokes) and draws them now, as the
patched mesh path does. The integrated Radeon, whose mesh shaders store whole index vectors, shows the same with
this build and translator against the shipped ones: 81 to 87 against 21 to 31 at 720p.

## Warp-aggregated atomics (patches 0028 and 0030, 2026-11-07)

The NVN2 compiler appends to GPU lists with warp-aggregated atomics, as ptxas does: inside the branch of the lanes that
append, one `VOTEU.ANY UR4, UPT, PT` takes their mask, `UBREV` + `FLO` pick the leader, the leader branches into
`POPC R, UR4` + `ATOMG.ADD` (or `RED`), and a `SHFL.IDX` hands the old count to the other lanes, which add their rank
(`POPC` of the mask under `SR_LTMASK`). Two translator bugs broke it, both by evaluating the vote with fewer lanes
than the hardware does:

- 0028: a VOTE / VOTEU under a guard predicate was wrapped in a branch, so only the guarded lanes voted. Now it runs in
  every active lane and the guard masks only the write.
- 0030: the AST optimizer propagated every single-use or unary expression into its uses, a `Ballot` included. The
  vote's mask went into the FLO, into the leader's POPC and into the rank, each a ballot of its own, and the one in the
  leader's branch counted only the leader: every warp reserved one slot, and its other lanes wrote over the next warps'
  entries. Ballots, votes and shuffles now stay where the shader put them (`AstOptimizer.HasWarpCollective`).

Found on OoT's foliage culling (`FoliageShader.sharcb`, the 169-group stage-1 kernel at caller main+0x563008 and the
stage-2 kernels after it): the cell, card and shadow-caster lists held a fraction of their entries, so tree crowns,
bushes and most shadow casters were missing. Corpus shader `1e10ec44df577182` (one of that archive's culling
variants) had 79 ballots for its 19 votes before 0030 and 20 after. With 0030 the corpus's 4,123 compute, 500 mesh
and 20 task shaders (with `gmem_map`) translate and pass spirv-val as before: one failure, the known
`68bbe98b3bfd9876`.
