// TOTK Vulkan adaptation of MacNativeGames games/totk/tools/totk_xlate/Program.cs (MIT, see LICENSE; MNG commit b75be8cf).
// Provenance: that file is based on D2R Mobile dm/gfx/tools/dmsh_xlate/Program.cs. Changes here: SPIR-V for Vulkan is the final
// output (no MSL service, no VertexCounterLowering, no NEAR_IS_MINUS_W rewrite), depth clip control reported as supported,
// non-constant texture offsets reported as unsupported, --disasm dropped.
// Protocol (stdout carries replies only; diagnostics go to stderr):
//   totk_xlate --serve              one request per stdin line: {"job": {...}, "out": "<dir>"}; one reply line per request:
//                                   "ok <key>" or "fail <key> <message>"; a malformed line gets "fail ? <message>" and the loop continues.
//   totk_xlate <jobs.json> <outdir> batch mode: a JSON array of jobs, prints a summary, exit 1 if any failed.
//   job: { "key", "stage": "vs|ps|cs", "code": path (graphics = 0x50-byte SPH + instructions, compute = instructions), "c1": hex,
//          "tex": {slot: type}, "img": {slot: {type, fmt}}, "attr": {loc: float|sint|uint}, "fout": {idx: float|sint|uint},
//          "local": [x, y, z], "shared": bytes, "debug": bool, "reduced": bool, "softfloat64": bool, "zero_locals": bool,
//          "sbuf_align": int, "subgroup": int,
//          Switch 2 (NVN2) only: "arch": "sm86" (Ampere code: graphics = 0x80-byte SPH + 128-bit instructions), "nvn2_cs": [ubos,
//          sbufs, textures, c0 bytes, push bytes, separate textures, separate samplers] (compute), "gmem_map": bool;
//          "stage": "ts" / "ms" (task / mesh, code = SPH + instructions) with "local": [threads, 1, 1] and "mesh": {"payload": bytes,
//          "task": 0|1, "max_vertices", "max_primitives", "primitive_sph": offset or 0, "prim_verts": 3|2|1, "whole_indices": 0|1} }
//   Output: <out>/<key>.spv and <out>/<key>.res.json (bindings, queried slots, notes).
//   docs/oot-xlate.md describes the Switch 2 fields, their sources in the NVN2 control section and what changes in the reply.
using Ryujinx.Common.Memory;
using Ryujinx.Graphics.Shader;
using Ryujinx.Graphics.Shader.Translation;
using System;
using System.Collections.Generic;
using System.Linq;
using System.IO;
using System.Runtime.InteropServices;
using System.Text.Json;
using System.Text.Json.Nodes;

static class SwshXlate
{
    const int TexBase = 0x168, SampBase = 0x568;   // NVN 分离贴图 / 采样器句柄在 c[2] 的字节基址（每槽 8 字节；dm/gfx/tools/maxwell_refs.py 全量实测） | byte base of NVN separate texture / sampler handles in c[2] (8 bytes per slot; full measurement in dm/gfx/tools/maxwell_refs.py)
    // NVN's other c[2] handle tables (runtime/gfx_shader.c handle_slot; seedds/nxnative dm/gfx/tools/dmsh_xlate/Program.cs Slot, MIT):
    // combined texture handles (nvnCommandBufferBindTexture) at 0x20 + 8 r, 32 slots, and image handles at 0x120 + 8 r. Ampere jobs
    // only, where NVN2 compute shaders reach their textures and images there (InstEmitAmpere.AMapC0) and graphics shaders bind
    // combined textures; a Maxwell job keeps the separate table alone, so TotK's translations do not change.
    const int CombinedBase = 0x20, CombinedSlots = 32, ImageBase = 0x120, ImageSlots = 9;

    sealed class Accessor : IGpuAccessor
    {
        readonly byte[] _code; readonly JsonObject _job; int _binding = 1; public readonly List<string> Notes = new(); public readonly SortedSet<int> TexQueried = new(), ImgQueried = new();
        readonly byte[] _c1;
        public Accessor(byte[] code, JsonObject job, MeshStage mesh = null) { _code = code; _job = job; _c1 = job["c1"] is JsonNode h ? Convert.FromHexString((string)h) : Array.Empty<byte>(); Ampere = (string)job["arch"] == "sm86"; Mesh = mesh; }
        // An NVN2 task or mesh shader ("stage" "ts" / "ms"): translated as a compute kernel whose stage memory is its shared memory, in a
        // VK_EXT_mesh_shader entry point (patch 0024; MakeMeshStage).
        public readonly MeshStage Mesh;
        public MeshStage QueryMeshStage() => Mesh;
        public int[] QueryAmpereStageMemory() => Mesh?.Offsets;
        // "arch": "sm86": Ampere code as NVN2 (Switch 2) ships it, translated by the Ampere frontend (patch 0017, TranslationFlags.Ampere).
        public readonly bool Ampere;
        // c1 常数段（着色器包控制段前 0x100 字节）：间接跳转 BRX 的跳转表在这里（LDC c[1]）；不提供时翻译器解不出跳转目标、控制流出错（粒子着色器死循环）
        // c1 constant segment (the 0x100 bytes before the control segment in the shader package): the jump table of indirect jump BRX lives here (LDC c[1]); without it the translator cannot resolve jump targets and control flow goes wrong (particle shaders loop forever)
        public uint ConstantBuffer1Read(int offset) { if (offset >= 0 && offset + 4 <= _c1.Length) return BitConverter.ToUInt32(_c1, offset); Notes.Add($"c1 读越界 +{offset:X}（c1 长 {_c1.Length}）"); return 0; }
        // 全部资源放进第 0 组，各占不同绑定号（spirv-cross 出成一个参数缓冲）；常量段号 index 的缓冲由翻译器给 binding = index+1 的惯例不依赖
        // All resources go into set 0, each with a distinct binding number (spirv-cross emits them as one argument buffer); the translator's convention of giving the buffer of constant segment number index binding = index+1 is not relied on
        public SetBindingPair CreateConstantBufferBinding(int index) => new(0, _binding++);
        public SetBindingPair CreateImageBinding(int count, bool isBuffer) { int b = _binding; _binding += count; return new(0, b); }
        public SetBindingPair CreateStorageBufferBinding(int index) => new(0, _binding++);
        public SetBindingPair CreateTextureBinding(int count, bool isBuffer) { int b = _binding; _binding += count; return new(0, b); }
        // Ampere: an address past the code is no code; the Ampere decoder ends a BRX jump table on it (seedds/nxnative 9507c22, MIT).
        public ReadOnlySpan<ulong> GetCode(ulong address, int minimumSize) => Ampere && address >= (ulong)_code.Length ? default : MemoryMarshal.Cast<byte, ulong>(new ReadOnlySpan<byte>(_code)[(int)address..]);
        // Ampere: the translator's own messages (a global access traced to no storage buffer, a bindless access it could not resolve) go to the notes.
        public void Log(string message) { if (Ampere) Notes.Add("translator: " + message); }
        public int QuerySamplerArrayLengthFromPool() => 32;
        public int QueryTextureArrayLengthFromBuffer(int slot) => 32;
        public int QueryTextureArrayLengthFromPool() => 32;

        string Get(string table, int slot) => _job[table] is JsonObject t && t[slot.ToString()] is JsonNode n ? (n is JsonObject o ? (string)o["type"] : (string)n) : null;
        int TexSlot(int handle, int cbufSlot, bool image = false)
        {
            (int off0, _, _) = TextureHandle.UnpackOffsets(handle);
            int bytes = off0 * 4;
            if (cbufSlot >= 0 && (cbufSlot & 0xffff) != 2) return -1;
            if (bytes >= TexBase && bytes < SampBase && (bytes - TexBase) % 8 == 0) return (bytes - TexBase) / 8;
            if (!Ampere || bytes % 8 != 0) return -1;
            if (bytes >= CombinedBase && bytes < CombinedBase + 8 * CombinedSlots) return (bytes - CombinedBase) / 8;
            if (image && bytes >= ImageBase && bytes < ImageBase + 8 * ImageSlots) return (bytes - ImageBase) / 8;
            return -1;
        }
        public SamplerType QuerySamplerType(int handle, int cbufSlot = -1)
        {
            int slot = TexSlot(handle, cbufSlot);
            if (slot >= 0) TexQueried.Add(slot);
            string t = slot >= 0 ? Get("tex", slot) ?? Get("img", slot) : null;
            if (slot < 0 && Ampere && TexSlot(handle, cbufSlot, image: true) is int islot && islot >= 0)   // a size query on an image handle
            {
                ImgQueried.Add(slot = islot);
                t = Get("img", islot);
            }
            if (t == null) { Notes.Add($"贴图句柄 {handle:X}（常量段 {cbufSlot}，槽 {slot}）无 PC 类型，按 2D"); return SamplerType.Texture2D; }
            if (t.EndsWith("_depth")) t = t.Substring(0, t.Length - 6);   // depth-format texture: same sampler type here; the host binds a depth view
            return t switch
            {
                "1d" => SamplerType.Texture1D, "1darray" => SamplerType.Texture1D | SamplerType.Array,
                "buffer" => SamplerType.TextureBuffer, "2d" => SamplerType.Texture2D, "2darray" => SamplerType.Texture2D | SamplerType.Array,
                "2dms" => SamplerType.Texture2D | SamplerType.Multisample, "2dmsarray" => SamplerType.Texture2D | SamplerType.Multisample | SamplerType.Array,
                "3d" => SamplerType.Texture3D, "cube" => SamplerType.TextureCube, "cubearray" => SamplerType.TextureCube | SamplerType.Array,
                _ => SamplerType.Texture2D,
            };
        }
        public TextureFormat QueryTextureFormat(int handle, int cbufSlot = -1)
        {
            int slot = TexSlot(handle, cbufSlot, image: true);
            if (slot >= 0) ImgQueried.Add(slot);
            string f = _job["img"] is JsonObject t && slot >= 0 && t[slot.ToString()] is JsonObject o ? (string)o["fmt"] : null;
            if (f == null) { Notes.Add($"图像句柄 {handle:X} 槽 {slot} 无 PC 格式，按 R8G8B8A8Unorm"); return TextureFormat.R8G8B8A8Unorm; }
            return Enum.TryParse(f, true, out TextureFormat tf) ? tf : TextureFormat.R8G8B8A8Unorm;
        }
        // Ampere: the "img" fmt the host answered for an image slot, Unknown without one. A formatted surface access to an image
        // viewed with an integer format (an NVN R32UI view on an R11G11B10F texture) declares the image with it (patch 0027).
        public TextureFormat QueryAnsweredImageFormat(int handle, int cbufSlot = -1)
        {
            int slot = Ampere ? TexSlot(handle, cbufSlot, image: true) : -1;
            if (slot < 0) return TextureFormat.Unknown;
            ImgQueried.Add(slot);
            string f = _job["img"] is JsonObject t && t[slot.ToString()] is JsonObject o ? (string)o["fmt"] : null;
            return f != null && Enum.TryParse(f, true, out TextureFormat tf) ? tf : TextureFormat.Unknown;
        }
        int Local(int i) => _job["local"] is JsonArray a && a.Count > i ? (int)a[i] : 1;
        public int QueryComputeLocalSizeX() => Local(0);
        public int QueryComputeLocalSizeY() => Local(1);
        public int QueryComputeLocalSizeZ() => Local(2);
        public int QueryComputeSharedMemorySize() => Mesh != null ? Mesh.Size : _job["shared"] is JsonNode n ? (int)n : 0;   // 配对 PC 的 groupshared 声明之和 | sum of the paired PC shader's groupshared declarations
        static AttributeType Attr(string s) => s switch { "sint" => AttributeType.Sint, "uint" => AttributeType.Uint, _ => AttributeType.Float };
        public GpuGraphicsState QueryGraphicsState()
        {
            Array32<AttributeType> attr = default; Array8<AttributeType> fout = default;
            if (_job["attr"] is JsonObject a) foreach (var kv in a) attr[int.Parse(kv.Key)] = Attr((string)kv.Value);
            if (_job["fout"] is JsonObject f) foreach (var kv in f) fout[int.Parse(kv.Key)] = Attr((string)kv.Value);
            // Vulkan: VK_EXT_depth_clip_control handles NEAR_IS_MINUS_W, so the shader is never rewritten ("depth_minus_w" in a job is ignored).
            const bool depthMinusW = false;
            // "early_fragment_tests": the fragment shader's NVN control byte +0x718 (the driver's early-Z-force, Ryujinx's
            // EarlyZForce); the SPIR-V then declares EarlyFragmentTests (MacNativeGames ae8f4f4).
            bool earlyZ = _job["early_fragment_tests"] is JsonValue ez && ez.TryGetValue<bool>(out bool eb) && eb;
            return new GpuGraphicsState(earlyZ, InputTopology.Triangles, false, TessPatchType.Triangles, TessSpacing.EqualSpacing,
                false, false, false, depthMinusW, false, 1f, AlphaTestOp.Always, 0f, attr, false, fout, false,
                false /* 不做 Y 取反 | no Y flip */, true /* 原点左上 | origin upper-left */, false);
        }
        public bool QueryHostSupportsDepthClipControl() => true;
        // Vulkan: the ConstOffset / ConstOffsets image operands Ryujinx emits must be constants. A TLD4.PTP whose offsets
        // come from registers (agl_technique_pfx compute b940aa86, dfe7662f) otherwise becomes an OpConstantComposite over
        // computed ids, which spirv-val rejects; false makes TexturePass emulate only non-constant offsets.
        public bool QueryHostSupportsNonConstantTextureOffset() => false;
        // "softfloat64": true lowers FP64 operations to FP32 (Ryujinx's DoubleToFloat pass), for a host without shaderFloat64.
        public bool QueryHostSupportsShaderFloat64() => _job["softfloat64"] is not JsonValue v || !(v.TryGetValue<bool>(out bool b) && b);
        public bool QueryHostSupportsTextureShadowLod() => true;
        // Sub-texel precision bits of a host whose gathers round differently from Maxwell (AMD, Intel): Ryujinx then
        // biases gather coordinates so the same texels are picked (TexturePass.InsertCoordGatherBias). 0: no bias.
        public int QueryHostGatherBiasPrecision() => _job["gather_bias"] is JsonNode n ? (int)n : 0;
        // 作业 reduced = true：放宽精度——翻译器只给强制精确的运算打 NoContraction（缺省全部打：保持 Maxwell 分开舍入的逐位结果，代价是 MSL 里大量禁优化的 spvFMul 等）
        // Job reduced = true: relaxed precision - the translator marks NoContraction only on operations that must be exact (the default marks all: keeps the bit-exact results of Maxwell's separate rounding, at the cost of many optimization-disabled spvFMul etc. in the MSL)
        public bool QueryHostReducedPrecision() => _job["reduced"] is JsonNode n && (bool)n;
        // Host answers for GPUs other than the RX 9070 (runtime/vk_caps.c); absent from the job they keep Ryujinx's defaults.
        // "sbuf_align": n: the host binds each storage buffer at its guest address rounded down to n (minStorageBufferOffsetAlignment
        // above 4), so storage offsets count from there (Ryujinx's unaligned path; the multi-target path uses n too).
        public bool QueryHasUnalignedStorageBuffer() => _job["sbuf_align"] is JsonNode;
        public int QueryHostStorageBufferOffsetAlignment() => _job["sbuf_align"] is JsonNode n ? (int)n : 16;
        // "subgroup": n: lanes per subgroup when the host cannot pin 32 (AMD GCN/Vega run 64): ballots and votes span n lanes.
        public int QueryHostSubgroupSize() => _job["subgroup"] is JsonNode n ? (int)n : 32;
        // "mvprev": true: the vertex shader also writes last frame's position, computed from slots bank + 15 in place of
        // the user banks c3-c16 (patch 0015, runtime/vk_mvprev.c); res.json's "mvprev" names the output location (-1:
        // none) and "mvprev_banks" the banks the position depends on (bit n: cn). With Ampere code a position whose
        // banks' last-frame slots the shader reads itself (c9's is c[24], NVN2's push constants) gets none (patch 0045).
        public bool QueryMotionPrevious() => _job["mvprev"] is JsonValue v && v.TryGetValue<bool>(out bool b) && b;
        // "zero_locals": true: function-local variables start at 0 (patch 0016, runtime/vk_caps.c decides per driver).
        public bool QueryZeroLocals() => _job["zero_locals"] is JsonValue v && v.TryGetValue<bool>(out bool b) && b;
        // "float_atomics": true: the device enabled shaderBufferFloat32AtomicAdd (runtime/vk_device.c), so float add reductions are one atomic (patch 0031).
        public bool QueryHostSupportsFloatAtomicAdd() => _job["float_atomics"] is JsonValue v && v.TryGetValue<bool>(out bool b) && b;
        // "coop_matrix": true: the device runs 16 x 8 x 16 f16 cooperative matrices (VK_NV_cooperative_matrix, runtime/vk_device.c),
        // so HMMA.16816 with an f16 accumulator uses the tensor cores (patch 0033).
        public bool QueryHostSupportsCooperativeMatrix() => _job["coop_matrix"] is JsonValue v && v.TryGetValue<bool>(out bool b) && b;
        // "gmem_map": true: the host binds the memory pool table and its write log (patches 0017, 0020, 0021; docs/oot-xlate.md), so a global memory
        // access traced to no storage buffer goes through it; absent, such an access is dropped (a load reads 0) as in Ryujinx.
        public bool QueryGlobalMemoryMap() => _job["gmem_map"] is JsonValue v && v.TryGetValue<bool>(out bool b) && b;
        // Switch 2 (NVN2, Ampere code; from MacNativeGames games/totk/tools/totk_xlate/Program.cs, after seedds/nxnative d3995ca4 /
        // a7bd05f4, MIT): an Ampere jump table ends at the end of the c1 data the host knows at the latest; a compute shader's own
        // driver constant layout in c[0] (uniform buffer entries, storage buffer entries, texture handles, its size, the push
        // constant bytes) comes with the job as nvn2_cs, read from its control section.
        public int QueryConstantBuffer1Size() => _c1.Length;
        public int[] QueryAmpereComputeC0Layout() => Ampere && _job["nvn2_cs"] is JsonArray l ? l.Select(v => (int)v).ToArray() : null;
        public int MotionPreviousLocation = -1, MotionPreviousBanks;
        public void ReportMotionPrevious(int location, int banks) => (MotionPreviousLocation, MotionPreviousBanks) = (location, banks);
    }


    // c[0] byte offset where the runtime puts a task or mesh draw's first workgroup (runtime/vk_exec.h VK_C0_MESH_FIRST), which the
    // shader adds to its own workgroup number.
    const int MeshFirstWorkgroupOffset = 0x9F0;
    // Ampere opcodes of shared memory accesses (LDS, STS, ATOMS): a task or mesh shader's stage memory is its shared memory, so one
    // that has shared memory of its own is refused, as seedds/nxnative dm/gfx/tools/dmsh_meshstage.py own_shared does.
    static readonly int[] SharedMemoryOpcodes = { 0x984, 0x388, 0x988, 0x38c, 0x98c, 0xf8c };

    // The MeshStage of a task or mesh job and the code the translator reads (the instructions after the 0x80-byte SPH). The stage
    // memory layout is seedds/nxnative dm/gfx/tools/dmsh_meshstage.py info / memory (MIT): word 0 the workgroup's number, the input
    // (a task's payload) at 0x10, the outputs after it at a 16-byte boundary (a task's payload, or a mesh shader's attribute blocks of
    // 32 vertices, a word per component of the SPH's OMAP), then a mesh shader's primitive bytes (count at +3, three index bytes a
    // triangle from +4).
    static MeshStage MakeMeshStage(JsonObject job, string stage, byte[] code, out byte[] instructions)
    {
        if (job["mesh"] is not JsonObject m) throw new ArgumentException($"stage {stage} without \"mesh\"");
        int Field(string name) => m[name] is JsonNode n ? (int)n : 0;
        int payload = Field("payload"), maxV = Field("max_vertices"), maxP = Field("max_primitives"), primSph = Field("primitive_sph");
        // "prim_verts": vertices of a primitive (control +0x5f): 3 triangles (also when absent), 2 lines, 1 points;
        // "whole_indices": 1: a primitive's index vector is stored by one thread (patch 0041; runtime/vk_device.c asks for it off NVIDIA)
        int primVerts = m["prim_verts"] is JsonNode pv ? (int)pv : 3;
        if (primVerts < 1 || primVerts > 3) throw new NotSupportedException($"mesh shader with {primVerts} vertices a primitive");
        // "as_compute": n: the stage as a compute kernel for a GPU without VK_EXT_mesh_shader (MeshStage.AsCompute): a task shader, or a
        // mesh shader of triangles without per-primitive attributes (with a task or without); n is the runtime's revision of the layout
        bool asCompute = Field("as_compute") != 0;
        if (asCompute && stage == "ms" && (primVerts != 3 || primSph != 0))
            throw new NotSupportedException("mesh shader as compute: only triangles without per-primitive attributes");
        int threads = job["local"] is JsonArray l && l.Count > 0 ? (int)l[0] : 1;
        bool task = stage == "ts";
        if (code.Length < 0x90 || (code.Length & 15) != 0) throw new ArgumentException($"{stage} code of {code.Length} bytes: no SPH and instructions");
        if (threads < 1 || threads > 128) throw new NotSupportedException($"{stage} with {threads} threads");
        instructions = code[0x80..];
        for (int o = 0; o + 16 <= instructions.Length; o += 16)
            if (Array.IndexOf(SharedMemoryOpcodes, BitConverter.ToUInt16(instructions, o) & 0xfff) >= 0)
                throw new NotSupportedException($"{stage} with shared memory of its own (opcode at 0x{o:x})");
        int output = 0x10 + ((payload + 15) & ~15);
        if (task)
        {
            return new MeshStage { IsTask = true, Threads = threads, PayloadWords = (payload + 3) >> 2, VertexComponents = Array.Empty<int>(),
                Offsets = new[] { 0, 0x10, output, output + payload }, Size = output + ((payload + 3) & ~3), FirstWorkgroupOffset = MeshFirstWorkgroupOffset,
                AsCompute = asCompute };
        }
        // The SPH's OMAP: the system values of byte 0x35 (bits 0..2 primitive id, layer and viewport index, which take a block and go
        // nowhere; bit 3 the point size), the position's components (bits 4..7), then generic attribute n's (bytes 0x36 on, a nibble
        // each). Seven of OoT's 854 mesh shaders have the point size: the grass that draws its blades' tips as points into a
        // shadow map. Read without it, every component sat one block late: the size became x and w came from the index bytes.
        var comps = new List<int>();
        for (int c = 0; c < 3; c++) if ((code[0x35] >> c & 1) != 0) comps.Add(MeshStage.Unused);
        if ((code[0x35] >> 3 & 1) != 0) comps.Add(MeshStage.PointSize);
        for (int c = 0; c < 4; c++) if ((code[0x35] >> (4 + c) & 1) != 0) comps.Add(-1 - c);
        for (int n = 0; n < 32; n++) for (int c = 0; c < 4; c++) if ((code[0x36 + n / 2] >> (4 * (n % 2) + c) & 1) != 0) comps.Add(4 * n + c);
        if (!comps.Any(c => c < 0 && c > MeshStage.PointSize)) throw new NotSupportedException("mesh shader whose SPH enables no position component");
        // Per-primitive attributes: a second SPH at code area + primitive_sph (control +0x64 when +0x68 bit 7), past the instructions the code
        // file holds. From "primitive_sph_hex" (its 0x80 bytes), else from "c1" with "c1_offset" (control +0x40), else, as in all four of
        // OoT's (agl's AglShader.sharcb), at the start of the c1 segment when that starts with an SPH (type 1, version 4).
        var primComps = new List<int>();
        if (primSph != 0)
        {
            byte[] c1 = job["c1"] is JsonNode h ? Convert.FromHexString((string)h) : Array.Empty<byte>();
            int c1Offset = m["c1_offset"] is JsonNode co ? (int)co : -1;
            byte[] sph = m["primitive_sph_hex"] is JsonNode ph ? Convert.FromHexString((string)ph)
                : c1Offset >= 0 && primSph >= c1Offset && primSph - c1Offset + 0x80 <= c1.Length ? c1[(primSph - c1Offset)..(primSph - c1Offset + 0x80)]
                : c1Offset < 0 && c1.Length >= 0x80 && (BitConverter.ToUInt32(c1, 0) & 0x3ff) == 0x81 ? c1[..0x80] : null;
            if (sph == null || sph.Length < 0x80) throw new NotSupportedException($"mesh shader whose per-primitive SPH (+0x{primSph:x}) is not in the job");
            for (int n = 0; n < 32; n++) for (int c = 0; c < 4; c++) if ((sph[0x36 + n / 2] >> (4 * (n % 2) + c) & 1) != 0) primComps.Add(4 * n + c);
            if (primComps.Any(pc => comps.Any(vc => vc >= 0 && vc >> 2 == pc >> 2))) throw new NotSupportedException("mesh shader with a location both per vertex and per primitive");
        }
        if (maxV < 1 || maxV > 256 || maxP < 1 || maxP > 256) throw new NotSupportedException($"mesh shader with {maxV} vertices and {maxP} primitives");
        int blocks = (Math.Max(Math.Max(maxV, maxP), 1) + MeshStage.Block - 1) / MeshStage.Block;
        int prim = output + blocks * Math.Max(comps.Count + primComps.Count, 1) * MeshStage.Block * 4;
        return new MeshStage { IsTask = false, HasTask = Field("task") != 0, Threads = threads, PayloadWords = (payload + 3) >> 2, MaxVertices = maxV,
            MaxPrimitives = maxP, VertexComponents = comps.ToArray(), PrimitiveComponents = primComps.ToArray(), Offsets = new[] { 0, 0x10, output, prim },
            Size = prim + ((4 + primVerts * maxP + 3) & ~3), FirstWorkgroupOffset = MeshFirstWorkgroupOffset, PrimitiveVertices = primVerts, WholeIndices = Field("whole_indices") != 0,
            AsCompute = asCompute };
    }

    internal static string Translate(JsonObject job, string outDir)   // also called by the --service mode (Service.cs)
    {
        string key = (string)job["key"];
            try
            {
                string arch = (string)job["arch"], stage = (string)job["stage"];
                if (arch != null && arch != "sm50" && arch != "sm86") throw new ArgumentException($"arch {arch} unknown (sm50: Maxwell, the default; sm86: Ampere)");
                bool meshStage = stage == "ts" || stage == "ms";
                if (arch == "sm86" && stage != "vs" && stage != "ps" && stage != "cs" && !meshStage) throw new ArgumentException($"stage {stage} not supported for sm86 (vs, ps, cs, ts, ms)");
                if (meshStage && arch != "sm86") throw new ArgumentException($"stage {stage} needs arch sm86");
                byte[] code = File.ReadAllBytes((string)job["code"]);
                MeshStage mesh = meshStage ? MakeMeshStage(job, stage, code, out code) : null;
                var acc = new Accessor(code, job, mesh);
                // 作业 debug = false：关 DebugMode（DebugMode 会跳过 AST 优化器，产物满是临时变量；不改语义）
                // Job debug = false: turn off DebugMode (DebugMode skips the AST optimizer and the output is full of temporaries; semantics unchanged)
                var flags = (job["debug"] is JsonNode dbg && !(bool)dbg ? 0 : TranslationFlags.DebugMode) | (stage == "cs" || meshStage ? TranslationFlags.Compute : 0);
                if (arch == "sm86") flags |= TranslationFlags.Ampere;   // NVN2 (Switch 2): a 0x80-byte SPH and 128-bit instructions
                var ctx = Translator.CreateContext(0, acc, new TranslationOptions(TargetLanguage.Spirv, TargetApi.Vulkan, flags));
                ShaderProgram p = ctx.Translate();
                File.WriteAllBytes(Path.Combine(outDir, key + ".spv"), p.BinaryCode);
                var res = new JsonObject
                {
                    ["key"] = key, ["stage"] = stage, ["notes"] = new JsonArray(acc.Notes.ConvertAll(n => (JsonNode)n).ToArray()),
                    ["cbufs"] = new JsonArray(), ["sbufs"] = new JsonArray(), ["textures"] = new JsonArray(), ["images"] = new JsonArray(),
                    ["uses_frag_coord"] = p.Info.UsesFragCoord, ["frag_out_map"] = p.Info.FragmentOutputMap,
                    ["tex_queried"] = new JsonArray(acc.TexQueried.Select(x => (JsonNode)x).ToArray()), ["img_queried"] = new JsonArray(acc.ImgQueried.Select(x => (JsonNode)x).ToArray()),
                };
                if (acc.QueryMotionPrevious()) (res["mvprev"], res["mvprev_banks"]) = (acc.MotionPreviousLocation, acc.MotionPreviousBanks);
                if (mesh != null)   // what the entry point writes (docs/oot-xlate.md)
                    res["mesh"] = new JsonObject
                    {
                        ["threads"] = mesh.Threads, ["payload_words"] = mesh.PayloadWords, ["max_vertices"] = mesh.MaxVertices, ["max_primitives"] = mesh.MaxPrimitives,
                        ["outputs"] = new JsonArray(mesh.VertexComponents.Where(c => c >= 0).Select(c => c >> 2).Distinct().Select(x => (JsonNode)x).ToArray()),
                        ["per_primitive_outputs"] = new JsonArray(mesh.PrimitiveComponents.Select(c => c >> 2).Distinct().Select(x => (JsonNode)x).ToArray()),
                        ["stage_memory"] = mesh.Size,
                    };
                if (mesh != null && mesh.AsCompute && mesh.IsTask)   // a task kernel (MeshStageWrapper.GenerateAsCompute)
                {
                    res["stage"] = "tsc";
                    res["mesh"]["as_compute"] = new JsonObject { ["launches"] = MeshStage.EmuTaskLaunches, ["max_tasks"] = mesh.EmuMaxTasks() };
                }
                else if (mesh != null && mesh.AsCompute)   // the emulation buffer's layout for this shader (MeshStageWrapper.GenerateAsCompute)
                {
                    int[] locs = mesh.VertexComponents.Where(c => c >= 0).Select(c => c >> 2).Distinct().OrderBy(x => x).ToArray();
                    res["stage"] = "msc";
                    res["mesh"]["as_compute"] = new JsonObject { ["vec4s"] = 1 + locs.Length, ["max_meshlets"] = mesh.EmuMaxMeshlets(1 + locs.Length),
                        ["locations"] = new JsonArray(locs.Select(x => (JsonNode)x).ToArray()), ["task"] = mesh.HasTask ? 1 : 0 };
                }
                foreach (var b in p.Info.CBuffers) res["cbufs"].AsArray().Add((JsonNode)new JsonObject { ["binding"] = b.Binding, ["slot"] = b.Slot });
                foreach (var b in p.Info.SBuffers) res["sbufs"].AsArray().Add((JsonNode)new JsonObject { ["binding"] = b.Binding, ["slot"] = b.Slot, ["cb_slot"] = b.SbCbSlot, ["cb_offset"] = b.SbCbOffset, ["flags"] = (int)b.Flags });
                foreach (var (list, name) in new[] { (p.Info.Textures, "textures"), (p.Info.Images, "images") })
                    foreach (var t in list) res[name].AsArray().Add((JsonNode)new JsonObject { ["binding"] = t.Binding, ["type"] = (int)t.Type, ["format"] = t.Format.ToString(), ["cbuf_slot"] = t.CbufSlot, ["handle"] = t.HandleIndex, ["array"] = t.ArrayLength, ["separate"] = t.Separate, ["flags"] = (int)t.Flags });
                File.WriteAllText(Path.Combine(outDir, key + ".res.json"), res.ToJsonString(new JsonSerializerOptions { WriteIndented = true }));
                return null;
            }
            catch (Exception e) { if (Environment.GetEnvironmentVariable("DMSH_STACK") != null) Console.Error.WriteLine(e.StackTrace); return $"{e.GetType().Name}: {e.Message.Split('\n')[0]}"; }
    }

    static int Main(string[] args)
    {
        if (args.Length == 1 && args[0] == "--serve")
        {
            // One request per line: {"job": {...}, "out": "<dir>"}; one reply line "ok <key>" / "fail <key> <message>".
            string line;
            while ((line = Console.ReadLine()) != null)
            {
                if (line.Length == 0) continue;
                string key = "?", err;
                try
                {
                    var req = JsonNode.Parse(line).AsObject(); var job = req["job"].AsObject(); string o = (string)req["out"];
                    key = (string)job["key"];
                    Directory.CreateDirectory(o);
                    err = Translate(job, o);
                }
                catch (Exception e) { err = $"{e.GetType().Name}: {e.Message.Split((char)10)[0]}"; }
                Console.WriteLine(err == null ? $"ok {key}" : $"fail {key} {err}");
                Console.Out.Flush();
            }
            return 0;
        }
        var jobs = JsonNode.Parse(File.ReadAllText(args[0])).AsArray(); string outDir = args[1]; Directory.CreateDirectory(outDir);
        int ok = 0, bad = 0;
        foreach (JsonObject job in jobs)
        {
            string err = Translate(job, outDir);
            if (err == null) ok++; else { bad++; Console.Error.WriteLine($"【红】{(string)job["key"]}：{err}"); }
        }
        Console.WriteLine($"翻译 {ok} 成功，{bad} 失败");
        return bad == 0 ? 0 : 1;
    }
}
