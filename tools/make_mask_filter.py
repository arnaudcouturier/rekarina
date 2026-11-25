"""Writes runtime/mask_filter_spv.h: the SPIR-V of the shadow mask's temporal filter (vk_maskfilter.c), assembled here
like tools/make_dlaa_mask.py (no GLSL compiler on the machine this was written on).

The shader, as GLSL:

    #version 450
    layout(local_size_x = 8, local_size_y = 8) in;
    layout(binding = 0) uniform sampler2D cur;                   // the frame's shadow mask after its last pass
    layout(binding = 1) uniform sampler2D hist;                  // last frame's filtered mask (linear, clamped)
    layout(binding = 2) uniform sampler2D depth;                 // the scene's depth (nearest)
    layout(binding = 3, rgba8) uniform writeonly image2D outp;   // this frame's filtered mask: the next frame's history
    layout(push_constant) uniform P {
        vec4 m0, m1, m2, m3;   // rows of the reprojection: this frame's clip space to last frame's
        vec4 p;                // size xy, the history's weight z, w 1 when the history holds last frame's mask
    } pc;
    void main() {
        ivec2 c = ivec2(gl_GlobalInvocationID.xy);
        if (c.x >= int(pc.p.x) || c.y >= int(pc.p.y)) return;
        vec4 now = texelFetch(cur, c, 0);
        vec2 lo = now.gb, hi = now.gb;
        ivec2 top = ivec2(pc.p.xy) - 1;
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                vec2 v = texelFetch(cur, clamp(c + ivec2(dx, dy), ivec2(0), top), 0).gb;
                lo = min(lo, v), hi = max(hi, v);
            }
        float z = texelFetch(depth, c, 0).r;
        vec2 win = vec2(c) + 0.5;
        vec4 ndc = vec4(win.x / pc.p.x * 2.0 - 1.0, 1.0 - win.y / pc.p.y * 2.0, z * 2.0 - 1.0, 1.0);
        vec4 q = vec4(dot(pc.m0, ndc), dot(pc.m1, ndc), dot(pc.m2, ndc), dot(pc.m3, ndc));
        vec2 pn = q.xy / q.w;
        vec2 uv = vec2(pn.x * 0.5 + 0.5, 0.5 - pn.y * 0.5);
        bool ok = pc.p.w > 0.5 && q.w > 1e-6 && uv.x >= 0.0 && uv.x <= 1.0 && uv.y >= 0.0 && uv.y <= 1.0;
        vec2 h = clamp(textureLod(hist, uv, 0.0).gb, lo, hi);
        vec2 g = mix(now.gb, h, ok ? pc.p.z : 0.0);
        imageStore(outp, c, vec4(now.r, g, now.a));
    }

The mask (RGBA8 at the scene's size, written MIN-blended by the grass's, the characters' and the sun's passes) holds the
hard shadows in green and the grass's soft shadow in blue; red and alpha pass through. Each pixel's surface is taken
to last frame's picture by the camera's motion (the depth and the reprojection), last frame's filtered value there is
held within the range of this frame's 3x3 neighbourhood (so a shadow that moves, or a surface that comes into view,
takes this frame's value: no trail) and mixed in by the weight. The depth is ndc z * 0.5 + 0.5 and window y grows
downwards from ndc y = +1, as runtime/shaders/dlaa_mv.comp takes them.

    python tools/make_mask_filter.py
"""
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / 'runtime' / 'mask_filter_spv.h'

SECTIONS = ('cap', 'ext', 'mem', 'entry', 'exec', 'dec', 'types', 'func')


class Asm:
    def __init__(self):
        self.next = 1
        self.sections = {k: [] for k in SECTIONS}

    def id(self):
        self.next += 1
        return self.next - 1

    def op(self, sec, code, *words):
        self.sections[sec].append([(len(words) + 1) << 16 | code, *words])

    def words(self):
        out = [0x07230203, 0x00010000, 0, self.next, 0]
        for sec in SECTIONS:
            for ins in self.sections[sec]:
                out.extend(ins)
        return out


def string(s):
    b = s.encode() + b'\0'
    b += b'\0' * (-len(b) % 4)
    return list(struct.unpack('<%dI' % (len(b) // 4), b))


def f32(x):
    return struct.unpack('<I', struct.pack('<f', x))[0]


OpCapability, OpExtInstImport, OpExtInst, OpMemoryModel, OpEntryPoint, OpExecutionMode = 17, 11, 12, 14, 15, 16
OpDecorate, OpMemberDecorate = 71, 72
OpTypeVoid, OpTypeBool, OpTypeInt, OpTypeFloat, OpTypeVector, OpTypeImage, OpTypeSampledImage = 19, 20, 21, 22, 23, 25, 27
OpTypeStruct, OpTypePointer, OpTypeFunction = 30, 32, 33
OpConstant, OpConstantComposite, OpVariable, OpFunction, OpFunctionEnd, OpLabel, OpReturn = 43, 44, 59, 54, 56, 248, 253
OpLoad, OpAccessChain, OpVectorShuffle, OpCompositeExtract, OpCompositeConstruct = 61, 65, 79, 81, 80
OpImage, OpImageSampleExplicitLod, OpImageFetch, OpImageWrite = 100, 88, 95, 99
OpConvertSToF, OpConvertFToS, OpBitcast = 111, 110, 124
OpIAdd, OpISub, OpFAdd, OpFSub, OpFMul, OpFDiv, OpDot = 128, 130, 129, 131, 133, 136, 148
OpFOrdLessThan, OpFOrdGreaterThan, OpFOrdLessThanEqual, OpFOrdGreaterThanEqual, OpSLessThan = 184, 186, 188, 190, 177
OpLogicalOr, OpLogicalAnd, OpSelect = 166, 167, 169
OpSelectionMerge, OpBranch, OpBranchConditional = 247, 249, 250
FMin, FMax, SClamp, FClamp, FMix = 37, 40, 45, 43, 46   # GLSL.std.450
UniformConstant, Input, PushConstant = 0, 1, 9
Binding, DescriptorSet, Block, Offset, BuiltIn, NonReadable = 33, 34, 2, 35, 11, 25
GlobalInvocationId = 28
Rgba8 = 4

a = Asm()
a.op('cap', OpCapability, 1)   # Shader
glsl = a.id()
a.op('ext', OpExtInstImport, glsl, *string('GLSL.std.450'))
a.op('mem', OpMemoryModel, 0, 1)   # Logical GLSL450
main = a.id()
t_void, t_fn, t_bool, t_f, t_i, t_u = a.id(), a.id(), a.id(), a.id(), a.id(), a.id()
t_v2, t_v4, t_v2i, t_v3u = a.id(), a.id(), a.id(), a.id()
t_simg_t, t_simg, t_wimg = a.id(), a.id(), a.id()   # sampled image type, its sampler type, storage rgba8
t_psimg, t_pwimg, t_pgid, t_pc, t_ppc, t_ppv4 = a.id(), a.id(), a.id(), a.id(), a.id(), a.id()
v_cur, v_hist, v_depth, v_out, v_gid, v_p = a.id(), a.id(), a.id(), a.id(), a.id(), a.id()
c_i0, c_i1, c_i2, c_i3, c_i4, c_im1 = a.id(), a.id(), a.id(), a.id(), a.id(), a.id()
c_f0, c_f1, c_f2, c_half, c_eps, c_one = a.id(), a.id(), a.id(), a.id(), a.id(), a.id()
c_v2i0 = a.id()

a.op('entry', OpEntryPoint, 5, main, *string('main'), v_gid)   # GLCompute
a.op('exec', OpExecutionMode, main, 17, 8, 8, 1)   # LocalSize
a.op('dec', OpDecorate, v_gid, BuiltIn, GlobalInvocationId)
for var, binding in ((v_cur, 0), (v_hist, 1), (v_depth, 2), (v_out, 3)):
    a.op('dec', OpDecorate, var, DescriptorSet, 0)
    a.op('dec', OpDecorate, var, Binding, binding)
a.op('dec', OpDecorate, v_out, NonReadable)
a.op('dec', OpDecorate, t_pc, Block)
for k in range(5):
    a.op('dec', OpMemberDecorate, t_pc, k, Offset, 16 * k)

T = 'types'
a.op(T, OpTypeVoid, t_void)
a.op(T, OpTypeFunction, t_fn, t_void)
a.op(T, OpTypeBool, t_bool)
a.op(T, OpTypeFloat, t_f, 32)
a.op(T, OpTypeInt, t_i, 32, 1)
a.op(T, OpTypeInt, t_u, 32, 0)
a.op(T, OpTypeVector, t_v2, t_f, 2)
a.op(T, OpTypeVector, t_v4, t_f, 4)
a.op(T, OpTypeVector, t_v2i, t_i, 2)
a.op(T, OpTypeVector, t_v3u, t_u, 3)
a.op(T, OpTypeImage, t_simg_t, t_f, 1, 0, 0, 0, 1, 0)       # 2D, sampled, format unknown
a.op(T, OpTypeSampledImage, t_simg, t_simg_t)
a.op(T, OpTypeImage, t_wimg, t_f, 1, 0, 0, 0, 2, Rgba8)     # 2D, storage, rgba8
a.op(T, OpTypePointer, t_psimg, UniformConstant, t_simg)
a.op(T, OpTypePointer, t_pwimg, UniformConstant, t_wimg)
a.op(T, OpTypePointer, t_pgid, Input, t_v3u)
a.op(T, OpTypeStruct, t_pc, t_v4, t_v4, t_v4, t_v4, t_v4)
a.op(T, OpTypePointer, t_ppc, PushConstant, t_pc)
a.op(T, OpTypePointer, t_ppv4, PushConstant, t_v4)
for c, v in ((c_i0, 0), (c_i1, 1), (c_i2, 2), (c_i3, 3), (c_i4, 4)):
    a.op(T, OpConstant, t_i, c, v)
a.op(T, OpConstant, t_i, c_im1, 0xffffffff)
for c, v in ((c_f0, 0.0), (c_f1, 1.0), (c_f2, 2.0), (c_half, 0.5), (c_eps, 1e-6), (c_one, 1.0)):
    a.op(T, OpConstant, t_f, c, f32(v))
a.op(T, OpConstantComposite, t_v2i, c_v2i0, c_i0, c_i0)
a.op(T, OpVariable, t_psimg, v_cur, UniformConstant)
a.op(T, OpVariable, t_psimg, v_hist, UniformConstant)
a.op(T, OpVariable, t_psimg, v_depth, UniformConstant)
a.op(T, OpVariable, t_pwimg, v_out, UniformConstant)
a.op(T, OpVariable, t_pgid, v_gid, Input)
a.op(T, OpVariable, t_ppc, v_p, PushConstant)

F = 'func'


def ins(code, rtype, *args):
    r = a.id()
    a.op(F, code, rtype, r, *args)
    return r


def ext(rtype, inst, *args):
    return ins(OpExtInst, rtype, glsl, inst, *args)


def ex(rtype, v, k):
    return ins(OpCompositeExtract, rtype, v, k)


l_entry, l_body, l_merge = a.id(), a.id(), a.id()
a.op(F, OpFunction, t_void, main, 0, t_fn)
a.op(F, OpLabel, l_entry)
m = [ins(OpLoad, t_v4, ins(OpAccessChain, t_ppv4, v_p, ci)) for ci in (c_i0, c_i1, c_i2, c_i3)]
p = ins(OpLoad, t_v4, ins(OpAccessChain, t_ppv4, v_p, c_i4))
gid = ins(OpLoad, t_v3u, v_gid)
cx = ins(OpBitcast, t_i, ex(t_u, gid, 0))
cy = ins(OpBitcast, t_i, ex(t_u, gid, 1))
w, h = ex(t_f, p, 0), ex(t_f, p, 1)
wi, hi = ins(OpConvertFToS, t_i, w), ins(OpConvertFToS, t_i, h)
inside = ins(OpLogicalAnd, t_bool, ins(OpSLessThan, t_bool, cx, wi), ins(OpSLessThan, t_bool, cy, hi))
a.op(F, OpSelectionMerge, l_merge, 0)
a.op(F, OpBranchConditional, inside, l_body, l_merge)
a.op(F, OpLabel, l_body)
c = ins(OpCompositeConstruct, t_v2i, cx, cy)
top = ins(OpCompositeConstruct, t_v2i, ins(OpISub, t_i, wi, c_i1), ins(OpISub, t_i, hi, c_i1))
cur_img = ins(OpImage, t_simg_t, ins(OpLoad, t_simg, v_cur))
now = ins(OpImageFetch, t_v4, cur_img, c, 2, c_i0)   # Lod 0
now_gb = ins(OpVectorShuffle, t_v2, now, now, 1, 2)
lo = hi_gb = now_gb
for dy in (-1, 0, 1):
    for dx in (-1, 0, 1):
        if not dx and not dy:
            continue
        off = ins(OpCompositeConstruct, t_v2i, {-1: c_im1, 0: c_i0, 1: c_i1}[dx], {-1: c_im1, 0: c_i0, 1: c_i1}[dy])
        q = ext(t_v2i, SClamp, ins(OpIAdd, t_v2i, c, off), c_v2i0, top)
        v = ins(OpImageFetch, t_v4, cur_img, q, 2, c_i0)
        vgb = ins(OpVectorShuffle, t_v2, v, v, 1, 2)
        lo = ext(t_v2, FMin, lo, vgb)
        hi_gb = ext(t_v2, FMax, hi_gb, vgb)
depth_img = ins(OpImage, t_simg_t, ins(OpLoad, t_simg, v_depth))
z = ex(t_f, ins(OpImageFetch, t_v4, depth_img, c, 2, c_i0), 0)   # Lod 0
wx = ins(OpFAdd, t_f, ins(OpConvertSToF, t_f, cx), c_half)
wy = ins(OpFAdd, t_f, ins(OpConvertSToF, t_f, cy), c_half)
nx = ins(OpFSub, t_f, ins(OpFMul, t_f, ins(OpFDiv, t_f, wx, w), c_f2), c_f1)
ny = ins(OpFSub, t_f, c_f1, ins(OpFMul, t_f, ins(OpFDiv, t_f, wy, h), c_f2))
nz = ins(OpFSub, t_f, ins(OpFMul, t_f, z, c_f2), c_f1)
ndc = ins(OpCompositeConstruct, t_v4, nx, ny, nz, c_f1)
qx, qy, qz, qw = (ins(OpDot, t_f, m[k], ndc) for k in range(4))
px = ins(OpFDiv, t_f, qx, qw)
py = ins(OpFDiv, t_f, qy, qw)
u = ins(OpFAdd, t_f, ins(OpFMul, t_f, px, c_half), c_half)
vv = ins(OpFSub, t_f, c_half, ins(OpFMul, t_f, py, c_half))
ok = ins(OpFOrdGreaterThan, t_bool, ex(t_f, p, 3), c_half)
for cond in (ins(OpFOrdGreaterThan, t_bool, qw, c_eps),
             ins(OpFOrdGreaterThanEqual, t_bool, u, c_f0), ins(OpFOrdLessThanEqual, t_bool, u, c_f1),
             ins(OpFOrdGreaterThanEqual, t_bool, vv, c_f0), ins(OpFOrdLessThanEqual, t_bool, vv, c_f1)):
    ok = ins(OpLogicalAnd, t_bool, ok, cond)
uv = ins(OpCompositeConstruct, t_v2, u, vv)
hs = ins(OpImageSampleExplicitLod, t_v4, ins(OpLoad, t_simg, v_hist), uv, 2, c_f0)   # Lod 0
hgb = ext(t_v2, FClamp, ins(OpVectorShuffle, t_v2, hs, hs, 1, 2), lo, hi_gb)
wgt = ex(t_f, p, 2)
wok = ins(OpSelect, t_f, ok, wgt, c_f0)
g = ext(t_v2, FMix, now_gb, hgb, ins(OpCompositeConstruct, t_v2, wok, wok))
out = ins(OpCompositeConstruct, t_v4, ex(t_f, now, 0), ex(t_f, g, 0), ex(t_f, g, 1), ex(t_f, now, 3))
a.op(F, OpImageWrite, ins(OpLoad, t_wimg, v_out), c, out)
a.op(F, OpBranch, l_merge)
a.op(F, OpLabel, l_merge)
a.op(F, OpReturn)
a.op(F, OpFunctionEnd)

words = a.words()
lines = [', '.join(f'0x{x:08x}' for x in words[i:i + 8]) for i in range(0, len(words), 8)]
text = ('// Generated by tools/make_mask_filter.py: the shadow mask\'s temporal filter (vk_maskfilter.c). Do not edit.\n'
        '#pragma once\n#include <stdint.h>\n'
        'static const uint32_t mask_filter_spv[] = {\n    ' + ',\n    '.join(lines) + '\n};\n')
OUT.write_bytes(text.encode())
print(f'{OUT}: {len(words)} words')
