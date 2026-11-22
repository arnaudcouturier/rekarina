#version 450
// build_shaders: keep functions
//
// Copyright (c) 2025 Carlos Lopez Jr.
// Copyright (C) 2026 Carlos Lopez
// SPDX-License-Identifier: MIT
//
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
// documentation files (the "Software"), to deal in the Software without restriction, including without limitation the
// rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit
// persons to whom the Software is furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all copies or substantial portions of the
// Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
// WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
// COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
//
// RenoDX HDR tone mapper library (ported from HLSL/Slang in github.com/clshortfuse/renodx: tonemap/psychov/test35.hlsl,
// tonemap/reno_drt.hlsl, tonemap/daniele.hlsl, draw.hlsl, colorgrade.hlsl, colorcorrect.hlsl, color/oklab.hlsl,
// color/srgb.hlsl and the OoT scene mapper). runtime/vk_hdr.c (tonemap_splice) links nx_tonemap into the game's
// translated ACES tonemap fragment shaders in memory and replaces the three clamped output channel stores.
//
// Parameters (vk_hdr.c vk_hdr_support, support_buffer from byte 1440, VK_HDR_PARAMS_AT):
//   p0: peak nits, game nits (paper white), HUD nits, tone mapper (-1: vanilla SDR passthrough, 0: vanilla, 3: RenoDRT,
//       9: PsychoV35)
//   p1: exposure [1], highlights [1], shadows [1], contrast [1]
//   p2: saturation [1], highlight saturation [1], blowout [0], flare [0]
//   p3: hue correction [1], grade strength [1], gamma correction (0 off, 1: 2.2, 2: 2.4) [1], tone slope [1]
//   p4: hue clip emulation [0.5], mid grey (0 render, 1 vanilla) [1], match tone slope (0 or 1) [1], unused
//   p5: bloom scale [1], local exposure scale in stops [1], two unused. The splicer reads p5 in the game's shader.
// main only keeps the entry point alive for glslc; tonemap_splice drops main and splices the helper functions.
layout(location = 0) in vec4 a0;
layout(location = 1) in vec4 a1;
layout(location = 2) in vec4 a2;
layout(location = 3) in vec4 a3;
layout(location = 4) in vec4 a4;
layout(location = 5) in vec4 a5;
layout(location = 6) in vec4 a6;
layout(location = 0) out vec4 o;

// Row-major matrix-vector multiply (matrices below are written row by row).
vec3 mulm(mat3 m, vec3 v) { return v * m; }

const mat3 BT709_TO_AP1 = mat3(0.6130974024, 0.3395231462, 0.0473794514,
                               0.0701937225, 0.9163538791, 0.0134523985,
                               0.0206155929, 0.1095697729, 0.8698146342);
const mat3 AP1_TO_BT709 = mat3(1.7050509927, -0.6217921207, -0.0832588720,
                               -0.1302564175, 1.1408047366, -0.0105483191,
                               -0.0240033568, -0.1289689761, 1.1529723329);
const mat3 BT709_TO_OKLABLMS = mat3(0.4122214708, 0.5363325363, 0.0514459929,
                                    0.2119034982, 0.6806995451, 0.1073969566,
                                    0.0883024619, 0.2817188376, 0.6299787005);
const mat3 OKLABLMS_TO_OKLAB = mat3(0.2104542553, 0.7936177850, -0.0040720468,
                                    1.9779984951, -2.4285922050, 0.4505937099,
                                    0.0259040371, 0.7827717662, -0.8086757660);
const mat3 OKLAB_TO_OKLABLMS = mat3(1.0, 0.3963377774, 0.2158037573,
                                    1.0, -0.1055613458, -0.0638541728,
                                    1.0, -0.0894841775, -1.2914855480);
const mat3 OKLABLMS_TO_BT709 = mat3(4.0767416621, -3.3077115913, 0.2309699292,
                                    -1.2684380046, 2.6097574011, -0.3413193965,
                                    -0.0041960863, -0.7034186147, 1.7076147010);

float y709(vec3 c) { return dot(c, vec3(0.2126390059, 0.7151686788, 0.0721923154)); }

float divide_safe(float a, float b, float fallback) { return b == 0.0 ? fallback : a / b; }

vec3 copy_sign(vec3 mag, vec3 s) {
    vec3 a = abs(mag);
    return mix(a, -a, lessThan(s, vec3(0.0)));
}

vec3 cbrt(vec3 x) { return copy_sign(pow(abs(x), vec3(1.0 / 3.0)), x); }

vec3 oklab_from_bt709(vec3 c) { return mulm(OKLABLMS_TO_OKLAB, cbrt(mulm(BT709_TO_OKLABLMS, c))); }

vec3 bt709_from_oklab(vec3 lab) {
    vec3 lms = mulm(OKLAB_TO_OKLABLMS, lab);
    return mulm(OKLABLMS_TO_BT709, lms * lms * lms);
}

vec3 clamp_ap1(vec3 c) { return mulm(AP1_TO_BT709, max(mulm(BT709_TO_AP1, c), vec3(0.0))); }

vec3 hue_oklab(vec3 incorrect, vec3 correct) {
    vec3 il = oklab_from_bt709(incorrect), cl = oklab_from_bt709(correct);
    float pre = length(il.yz);
    vec2 ab = cl.yz;
    ab *= divide_safe(pre, length(ab), 1.0);
    il.yz = ab;
    return clamp_ap1(bt709_from_oklab(il));
}

float srgb_decode(float c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }

float gamma_to_srgb(float c, float g) { return srgb_decode(pow(c, 1.0 / g)); }

// Target peak before the present's gamma correction (0 none, 1: 2.2, 2: 2.4) so highlights still land on display peak.
float peak_before_gamma(float peak, float gamma) {
    return gamma == 0.0 ? peak : gamma_to_srgb(peak, gamma == 1.0 ? 2.2 : 2.4);
}

float smoother(float t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); }

// ---- RenoDRT luminance tone mapper

float highlights(float x, float h, float mid) {
    if (h == 1.0) return x;
    float t = 0.0;
    if (x > mid) t = clamp(log2(x / mid) / log2(1.0 / mid), 0.0, 1.0);
    t = smoother(t);
    if (h > 1.0) return mix(x, mid * pow(x / mid, h), t);
    float b = mid * pow(x / mid, 2.0 - h);
    return divide_safe(x * x, mix(x, b, t), x);
}

float shadows(float x, float s, float mid) {
    if (s == 1.0) return x;
    float ratio = max(divide_safe(x, mid, 0.0), 0.0);
    float base_term = x * mid;
    float base_scale = divide_safe(base_term, ratio, 0.0);
    float shadow_floor = mid / 16.0;
    float t = 1.0;
    if (x > shadow_floor) t = clamp(log2(x / mid) / log2(shadow_floor / mid), 0.0, 1.0);
    t = smoother(t);
    if (s > 1.0) {
        float raised = x * (1.0 + divide_safe(base_term, pow(ratio, s), 0.0));
        return x + (raised - x * (1.0 + base_scale)) * t;
    }
    float lowered = x * (1.0 - divide_safe(base_term, pow(ratio, 2.0 - s), 0.0));
    return x + (lowered - x * (1.0 - base_scale)) * t;
}

float curve(float x, float n, float g, float c_d, float t_1) {
    const float n_r = 100.0, c = 0.18, w_g = 0.0, r_hit_min = 128.0, r_hit_max = 256.0;
    float m_0 = n / n_r;
    float m_1 = 0.5 * (m_0 + sqrt(m_0 * (m_0 + 4.0 * t_1)));
    float r_hit = r_hit_min + (r_hit_max - r_hit_min) * (log(m_0) / log(10000.0 / 100.0));
    float u = pow((r_hit / m_1) / ((r_hit / m_1) + 1.0), g);
    float m = m_1 / u;
    float w_i = log(n / 100.0) / log(2.0);
    float c_t = (c_d / n_r) * (1.0 + w_i * w_g);
    float g_ip = 0.5 * (c_t + sqrt(c_t * (c_t + 4.0 * t_1)));
    float g_ipp2 = -m_1 * pow(g_ip / m, 1.0 / g) / (pow(g_ip / m, 1.0 / g) - 1.0);
    float w_2 = c / g_ipp2;
    float s_2 = w_2 * m_1;
    float u_2 = pow((r_hit / m_1) / ((r_hit / m_1) + w_2), g);
    float m_2 = m_1 / u_2;
    float ts = pow(max(0.0, x) / (x + s_2), g) * m_2;
    return max(0.0, (ts * ts) / (ts + t_1));
}

struct Drt {
    float nits_peak, exposure, highlights, shadows, contrast, saturation, dechroma, flare, hue, blowout;
    float mid_gray_nits;
    bool clamp_zero;
};

vec3 drt(vec3 bt709, Drt d) {
    const float mid = 0.18;
    float peak = d.nits_peak / 100.0;
    float y_original = y709(bt709);
    float y = y_original * d.exposure;
    y = highlights(y, d.highlights, mid);
    y = shadows(y, d.shadows, mid);
    float y_new = clamp(curve(y, d.nits_peak, d.contrast, d.mid_gray_nits, d.flare), 0.0, peak);
    vec3 color = bt709 * (y_original > 0.0 ? y_new / y_original : 0.0);
    if (d.dechroma != 0.0 || d.saturation != 1.0 || d.hue != 0.0 || d.blowout != 0.0) {
        vec3 lab = oklab_from_bt709(color);
        if (d.hue != 0.0) {
            vec3 old_lab = oklab_from_bt709(bt709);
            float pre = length(lab.yz);
            lab.yz = mix(lab.yz, old_lab.yz, d.hue);
            lab.yz *= divide_safe(pre, length(lab.yz), 1.0);
        }
        if (d.dechroma != 0.0) lab.yz *= mix(1.0, 0.0, clamp(pow(y_original / 100.0, 1.0 - d.dechroma), 0.0, 1.0));
        if (d.blowout != 0.0) {
            float percent_max = clamp(y_original * 100.0 / 10000.0, 0.0, 1.0);
            float change = pow(1.0 - percent_max, 100.0 * abs(d.blowout));
            if (d.blowout < 0.0) change = 2.0 - change;
            lab.yz *= change;
        }
        lab.yz *= d.saturation;
        color = bt709_from_oklab(lab);
    }
    if (d.clamp_zero) color = max(color, vec3(0.0));
    return min(color, vec3(peak));
}

vec3 neutral_sdr(vec3 c) {
    Drt d = Drt(100.0, 1.0, 1.0, 1.0, 1.0, 1.0, 0.0, 0.0, 0.0, 0.0, 18.0, true);
    return drt(c, d);
}

vec3 upgrade(vec3 untonemapped, vec3 neutral, vec3 graded, float strength) {
    float yu = y709(untonemapped), yn = y709(neutral), yg = y709(graded);
    float ratio;
    if (yu < yn) {
        ratio = yu / yn;
    } else {
        float y_new = yg + max(0.0, yu - yn);
        ratio = yg > 0.0 ? y_new / yg : 0.0;
    }
    vec3 scaled = hue_oklab(graded * ratio, graded);
    return mix(untonemapped, scaled, strength);
}

vec3 aces_fitted(vec3 x) {
    return clamp(x * (2.51 * x + 0.03) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

// ---- PsychoV35 observer-response cone mapper (Stockman-CVRL 2-degree LMS)

const mat3 PSY_BT709_TO_LMS = mat3(0.2896057766, 0.6972466442, 0.0763712786,
                                   0.0901837576, 0.7073490257, 0.1122031464,
                                   0.0155287401, 0.0536093073, 0.5097978305);
const mat3 PSY_LMS_TO_BT709 = mat3(4.9676196505, -4.9223795294, 0.3391991812,
                                   -0.6196828788, 2.0517506515, -0.3587439844,
                                   -0.0861520040, -0.0658193830, 1.9889544833);
const vec3 PSY_YF_WEIGHTS = vec3(0.68990272, 0.34832189, 0.0);
const vec3 PSY_D65_WHITE_LMS = vec3(1.0632236993, 0.9097359297, 0.5789358779);

float psy_mean(vec3 v) { return v.x + ((v.y - v.x) + (v.z - v.x)) / 3.0; }

vec3 psy_rgb_residual(vec3 normalized_lms) { return mulm(PSY_LMS_TO_BT709, normalized_lms * PSY_D65_WHITE_LMS); }

void psy_response_logs(float log_q, float k, float n, out float log_u, out float log_deficit) {
    float latent = log(k) - log(1.0 - k) + n * log_q;
    float correction = log(1.0 + exp(-abs(latent)));
    log_u = min(latent, 0.0) - correction;
    float log_tail = -max(latent, 0.0) - correction;
    log_deficit = latent > 20.0 ? log_tail
                                : log(abs(log_u) < 0.01
                                          ? -log_u * (1.0 + log_u * (0.5 + log_u * (1.0 / 6.0 + log_u / 24.0)))
                                          : 1.0 - exp(log_u));
}

// Fits Rec. 709 [0..peak] because the game's scene target is R11G11B10_UFLOAT (non-negative).
vec3 psycho35(vec3 bt709, float peak, float highlights, float shadows, float contrast, float purity_scale, float cone,
              float anchor_in_gray, float anchor_out_gray) {
    vec3 input_lms = mulm(PSY_BT709_TO_LMS, bt709);
    vec3 anchor_in = mulm(PSY_BT709_TO_LMS, vec3(anchor_in_gray));
    vec3 anchor_out = mulm(PSY_BT709_TO_LMS, vec3(anchor_out_gray));
    if (highlights != 1.0 || shadows != 1.0) {
        float input_yf = dot(PSY_YF_WEIGHTS, input_lms);
        float anchor_yf = dot(PSY_YF_WEIGHTS, anchor_in);
        float graded_y = input_yf;
        if (highlights != 1.0) {
            float ratio = graded_y / anchor_yf;
            float blend = graded_y > anchor_yf ? clamp(log2(ratio) / log2(1.0 / anchor_yf), 0.0, 1.0) : 0.0;
            blend = smoother(blend);
            float shaped = pow(ratio, abs(highlights - 1.0));
            graded_y = highlights > 1.0 ? graded_y * mix(1.0, shaped, blend) : graded_y / mix(1.0, shaped, blend);
        }
        if (shadows != 1.0) {
            float ratio = graded_y / anchor_yf;
            float blend = graded_y > anchor_yf * exp2(-4.0) ? clamp(log2(ratio) / -4.0, 0.0, 1.0) : 1.0;
            blend = smoother(blend);
            float target_y = shadows > 1.0
                                 ? graded_y * (1.0 - anchor_yf * anchor_yf) + anchor_yf * anchor_yf * anchor_yf * pow(ratio, 2.0 - shadows)
                                 : graded_y * (1.0 + anchor_yf * anchor_yf) - anchor_yf * anchor_yf * anchor_yf * pow(ratio, shadows);
            graded_y = mix(graded_y, target_y, blend);
        }
        if (input_yf != 0.0) input_lms *= graded_y / input_yf;
    }
    vec3 source_ratio = input_lms / anchor_in;
    if (purity_scale != contrast) {
        float neutral_ratio = dot(PSY_YF_WEIGHTS, input_lms) / dot(PSY_YF_WEIGHTS, anchor_in);
        vec3 purity_direction = source_ratio - neutral_ratio;
        float purity_amount = purity_scale / contrast;
        if (all(greaterThanEqual(source_ratio, vec3(0.0))) && neutral_ratio > 0.0 && purity_amount > 1.0) {
            for (int c = 0; c < 3; ++c)
                if (purity_direction[c] < 0.0) purity_amount = min(purity_amount, neutral_ratio / -purity_direction[c]);
            source_ratio = max(neutral_ratio + purity_amount * purity_direction, 0.0);
        } else {
            source_ratio = neutral_ratio + purity_amount * purity_direction;
        }
    }
    vec3 anchor_power = anchor_out / (PSY_D65_WHITE_LMS * peak);
    vec3 input_exponent = contrast * cone / (1.0 - anchor_power);
    if (all(equal(source_ratio, vec3(0.0)))) return vec3(0.0);
    float drive = max(abs(source_ratio.x), max(abs(source_ratio.y), abs(source_ratio.z)));
    vec3 ratio = source_ratio / drive;
    vec3 log_ratio = log(vec3(ratio.x != 0.0 ? ratio.x : 1.0, ratio.y != 0.0 ? ratio.y : 1.0, ratio.z != 0.0 ? ratio.z : 1.0));
    vec3 response, deficit;
    for (int i = 0; i < 3; ++i) {
        if (ratio[i] == 0.0) {
            response[i] = 0.0;
            deficit[i] = 1.0;
        } else {
            float log_u, log_deficit;
            psy_response_logs(log_ratio[i] + log(drive), anchor_power[i], input_exponent[i], log_u, log_deficit);
            response[i] = exp(log_u);
            deficit[i] = exp(log_deficit);
        }
    }
    float response_mean = psy_mean(response);
    vec3 response_residual = response_mean > 0.5 ? psy_mean(deficit) - deficit : response - response_mean;
    float residual_scale = max(abs(response_residual.x), max(abs(response_residual.y), abs(response_residual.z)));
    vec3 scaled_residual = residual_scale > 0.0 ? response_residual / residual_scale : vec3(0.0);
    float scaled_radius = length(scaled_residual);
    float radius = residual_scale * scaled_radius;
    vec3 direction = scaled_radius > 0.0 ? scaled_residual / scaled_radius : vec3(0.0);
    vec3 source = ratio - psy_mean(ratio);
    float source_radius = length(source);
    if (source_radius > 0.0 && radius > 0.0) {
        vec3 midpoint_direction = source / source_radius + direction;
        float midpoint_length = length(midpoint_direction);
        if (midpoint_length > 0.0) direction = midpoint_direction / midpoint_length;
    }
    float mean = peak * response_mean, headroom = peak * psy_mean(deficit);
    vec3 residual = peak * radius * direction;
    if (all(equal(residual, vec3(0.0)))) return vec3(mean);
    vec3 displacement = psy_rgb_residual(residual);
    float scale = 1.0;
    for (int channel = 0; channel < 3; ++channel) {
        if (displacement[channel] > 0.0) scale = min(scale, headroom / displacement[channel]);
        else if (displacement[channel] < 0.0) scale = min(scale, mean / -displacement[channel]);
    }
    return mean + psy_rgb_residual(residual * scale);
}

vec3 hue_clip(vec3 untonemapped, float amount) {
    vec3 magnitude = abs(untonemapped);
    vec3 reference = aces_fitted(magnitude);
    float low = min(reference.x, min(reference.y, reference.z));
    float range = max(reference.x, max(reference.y, reference.z)) - low;
    bool neutral = range == 0.0;
    float mag_low = min(magnitude.x, min(magnitude.y, magnitude.z));
    float mag_high = max(magnitude.x, max(magnitude.y, magnitude.z));
    vec3 transferred = mix(vec3(mag_low), vec3(mag_high), (reference - low) / (neutral ? 1.0 : range));
    transferred = copy_sign(transferred, untonemapped);
    if (neutral) transferred = untonemapped;
    vec3 bent = mix(untonemapped, transferred, amount);
    float original_y = y709(untonemapped), bent_y = y709(bent);
    if (original_y > 0.0 && bent_y > 0.0) untonemapped = bent * (original_y / bent_y);
    return untonemapped;
}

vec3 scene_tonemap(vec3 untonemapped, float gain, float peak, vec4 p1, vec4 p2, vec4 p3, vec4 p4) {
    if (p1.x == 0.0) return vec3(0.0);
    if (p4.x > 0.0) untonemapped = hue_clip(untonemapped, p4.x);
    // Analytic log-slope x*V'(x)/V(x) at V''(x)=0 (x=0.120305925) of the game's ACES fit.
    float cone = p4.z != 0.0 ? 1.3355037565264822 : 1.0;
    float anchor_out = (p4.y == 1.0 ? aces_fitted(vec3(0.18)).x : 0.18) * gain * p1.x;
    return psycho35(untonemapped, peak, p1.y, p1.z, p1.w, p2.x, cone * p3.w, 0.18, anchor_out);
}

vec3 nx_tonemap(vec3 x, vec4 scene, vec4 p0, vec4 p1, vec4 p2, vec4 p3, vec4 p4) {
    vec3 vanilla = aces_fitted(x);
    if (p0.w < 0.0) return vanilla;   // SDR swapchain: the game's own SDR result
    vec3 color = vanilla;             // tone mapper 0: vanilla curve scaled to picture white
    if (p0.w == 9.0) {
        if (!(scene.a > 0.0)) return vec3(0.0);
        float peak = peak_before_gamma(max(p0.x, p0.y) / p0.y, p3.z);
        color = scene_tonemap(scene.rgb / scene.a, scene.a, peak, p1, p2, p3, p4);
    } else if (p0.w > 0.0) {
        vec3 graded = p3.y == 0.0 ? x : upgrade(x, neutral_sdr(x), vanilla, p3.y);
        float peak = peak_before_gamma(p0.x / p0.y, p3.z);
        Drt d = Drt(peak * 100.0, p1.x, p1.y, p1.z, p1.w, p2.x, p2.z, 0.10 * pow(p2.w, 10.0), p3.x, -(p2.y - 1.0),
                    18.0, false);
        color = drt(graded, d);
    }
    color *= p0.y / p0.z;   // picture white relative to HUD white
    return mix(color, vec3(0.0), isnan(color));
}

void main() { o = vec4(nx_tonemap(a0.xyz, a1, a2, a3, a4, a5, a6), 1.0); }
