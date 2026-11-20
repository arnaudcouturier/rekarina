// The game's movies (AV1, 1920x1080, 60 fps) go through agl's NDVE network before they are shown: a U-Net of eleven
// compute passes on the decoded picture (passes 1-11: 960x540x16 features down to 240x135x64 and back), pass 12, which
// adds what the U-Net computed to the picture, and pass 13, the upscaler to the console's 2560x1440 (docs/oot-xlate.md,
// "NN noise"). Its matrix multiplies are HMMA.16816, which run on the tensor cores where the device has
// VK_NV_cooperative_matrix (gfx_shader_coop_matrix) and on the translator's shared-memory stand-in elsewhere. On an
// RTX 5070 the title movie took 4.4 ms of GPU a frame with the tensor cores and 9.0 with the stand-in, 7.4 of them in
// the U-Net; a player's GTX 1650 Super showed every movie at 17 fps (56 ms of GPU a frame), and since a movie's event
// script counts the game's updates as 60 Hz frames, at under a third of its speed. (A body of subgroup shuffles for
// the stand-in gave the same bits and was 4.7 times slower, 43 ms a frame: not used.)
//
// The whole network runs as long as the card holds the movie's rate. While a movie plays the GPU time of its frames is
// measured (vk_movienet_frame: the submission parts' timestamps, vk_movienet_timing turns them on meanwhile); when the
// average over MEASURE frames does not fit the movie's frame time (MOVIE_FPS, FIT of it, for the rest of the frame), the
// U-Net is left out of the movies from then on, for the rest of the session (never outside a movie): its dispatches are
// skipped (vk_movienet_skip, vk_exec_draw.c op_dispatch) and pass 12 runs a pipeline whose HMMA helper hands its
// accumulator through (vk_pipeline_compute_passthrough, vk_movienet_module), so it adds only its bias, under 4e-6 in the
// game's data, and writes the decoded picture itself; the upscaler runs as before. The title movie then took 1.5 ms of
// GPU a frame with the stand-in. The picture: full-size shots of the same movie frames with and without the U-Net differ
// by 0.6-1.1 of 255 levels on average (PSNR 43-50 dB, at most 34 levels in a few pixels), a cut made only where the movie
// would otherwise run slow. REKARINA_MOVIE_UNET: auto (the default, as above), 1 the whole network always, 0 never the
// U-Net in a movie.
// The measurement took the first 180 frames of the first movie, ten seconds at the 17 fps of a GTX 1650 Super, and the
// movie's sound ran on meanwhile (a player: every movie started slow for 8-9 s and stayed out of step with its sound).
// So an early look decides at once where the first frames take more than twice the movie's frame time (EARLY_*:
// about a second on that card), and the verdict is kept per GPU and driver in movie-network.txt beside rekarina.exe:
// the next start leaves the U-Net out from the first movie frame on.
// The passes are known by their programs' input ids (game 1.1.1); another version's are not touched.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "gfx.h"
#include "host.h"
#include "vk.h"
#include "platform/platform.h"

static const uint64_t UNET[] = {   // passes 1-11 (2 and 11, 4 and 9, 6 and 7 share a program)
    0x09f71b09f69e2bcfull, 0xd4fff465ffd5f5a2ull, 0x16a4de9c319833a5ull, 0x44fadb5bf93b3263ull,
    0x3830484b2960b8ddull, 0x1d9a3a87bc5b9a90ull, 0x031ea2f64a64c6d1ull, 0xe419cd3c027b7a29ull,
};
static const uint64_t PASS12 = 0x6103f6383911dad2ull;
#define MOVIE_FPS 60.0   // the movies' rate
#define FIT 0.9          // of its frame time, the GPU may take for a movie frame
#define SETTLE 60        // frames of a movie not measured (its first pipelines, the decoder starting)
#define MEASURE 120      // frames averaged
#define EARLY_SETTLE 8   // the early look: frames not measured,
#define EARLY 12         // frames averaged,
#define EARLY_OVER 2.0   // and how far over the movie's frame time they must be to decide at once

static uint64_t prog_id(const GfxShader *s) {   // the first 8 bytes of the input id, as the logs name programs
    const uint8_t *iid = gfx_shader_input_id(s);
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = v << 8 | iid[i];
    return v;
}

static int mode = -1;        // 2 auto, 1 the whole network always, 0 never the U-Net
static volatile int cut;     // auto: the U-Net is left out from now on

// movie-network.txt beside rekarina.exe: a line "cut <vendor>:<device>:<driver>" for each GPU and driver on which the
// movies did not fit with the whole network.
static int verdict_path(char *out, size_t n) {
    char exe[PLAT_PATH_MAX * 2];
    int len = plat_exe_path(exe, sizeof exe);
    char *sep = len > 0 && (size_t)len < sizeof exe ? plat_last_sep(exe) : NULL;
    if (!sep) return 0;
    *sep = 0;
    return snprintf(out, n, "%s/movie-network.txt", exe) < (int)n;
}
static void gpu_key(char *out, size_t n) {
    const VkPhysicalDeviceProperties *p = &vk_ctx()->props;
    snprintf(out, n, "cut %04x:%04x:%08x", p->vendorID, p->deviceID, p->driverVersion);
}
static int verdict_read(void) {
    char path[PLAT_PATH_MAX * 2 + 32], key[64], ln[128];
    if (!vk_ctx() || !verdict_path(path, sizeof path)) return 0;
    gpu_key(key, sizeof key);
    FILE *f = fopen(path, "r");
    int found = 0;
    while (f && !found && fgets(ln, sizeof ln, f)) found = !strncmp(ln, key, strlen(key));
    if (f) fclose(f);
    return found;
}
static void verdict_write(void) {
    char path[PLAT_PATH_MAX * 2 + 32], key[64];
    if (!verdict_path(path, sizeof path)) return;
    gpu_key(key, sizeof key);
    FILE *f = fopen(path, "a");
    if (f) fprintf(f, "%s\n", key), fclose(f);
}

static int read_mode(void) {
    if (mode < 0) {
        const char *e = cfg_get("REKARINA_MOVIE_UNET");
        mode = e && *e && strcmp(e, "auto") ? atoi(e) != 0 : 2;
        if (mode == 2 && verdict_read()) {
            __atomic_store_n(&cut, 1, __ATOMIC_RELAXED);
            rt_log("vk: the movies' network runs without its U-Net on this GPU (movie-network.txt: an earlier start "
                   "measured its movie frames over their time)");
        }
    }
    return mode;
}

int codec_movie_live(void);   // ns2_codec.c
int vk_movienet_on(void) {   // the U-Net is left out now: only ever while a movie plays
    int m = read_mode();
    return (m == 0 || (m == 2 && __atomic_load_n(&cut, __ATOMIC_RELAXED))) && codec_movie_live();
}
int vk_movienet_timing(void) { return read_mode() == 2 && !__atomic_load_n(&cut, __ATOMIC_RELAXED) && codec_movie_live(); }

// At each present (vk_backend.c): the measurement of the movie playing, and the decision.
void vk_movienet_frame(void) {
    static int live, frames;
    static uint64_t gpu0, told_fit;
    if (read_mode() != 2 || __atomic_load_n(&cut, __ATOMIC_RELAXED)) return;
    if (!codec_movie_live()) {
        live = 0;
        return;
    }
    static uint64_t gpu_e;
    uint64_t gpu = perf_gpu_busy_total();
    if (!live) live = 1, frames = 0;
    if (++frames == EARLY_SETTLE) gpu_e = gpu;
    if (frames == SETTLE) gpu0 = gpu;
    double fit = 1000.0 / MOVIE_FPS * FIT;
    int test = 0;
    {   // DEBUG-MOVIEFIT: REKARINA_MOVIE_FIT_MS=<ms> stands in for a slower card's budget (a test of the cut on a fast one)
        const char *e = cfg_env("REKARINA_MOVIE_FIT_MS");
        if (e && atof(e) > 0) fit = atof(e), test = 1;
    }
    double ms = 0;
    int early = frames == EARLY_SETTLE + EARLY && gpu != gpu_e;
    if (early) {
        ms = (double)(gpu - gpu_e) / 1e6 / EARLY;
        if (ms <= fit * EARLY_OVER) return;
    } else {
        if (frames < SETTLE + MEASURE) return;
        ms = (double)(gpu - gpu0) / 1e6 / MEASURE;
        if (gpu == gpu0) {   // no timestamps on this device: nothing to decide from
            frames = -1000000;
            return;
        }
    }
    if (ms > fit) {
        __atomic_store_n(&cut, 1, __ATOMIC_RELAXED);
        if (!test) verdict_write();
        rt_log("vk: a movie frame took %.1f ms of GPU (%s), over the %.1f ms a %.0f fps movie leaves it: the movies' "
               "network runs without its U-Net from now on%s (REKARINA_MOVIE_UNET)", ms,
               early ? "its first frames" : "measured over 120 frames", fit, MOVIE_FPS, test ? "" : ", here and at the next starts");
    } else if (told_fit++ < 3) {
        rt_log("vk: a movie frame took %.1f ms of GPU: the movies' network runs whole", ms);
    }
    frames = -1000000;   // decided for this movie
}

int vk_movienet_skip(const GfxShader *cs) {
    if (!cs) return 0;
    uint64_t id = prog_id(cs);
    int unet = 0;
    for (size_t i = 0; i < sizeof UNET / sizeof UNET[0]; i++) unet |= id == UNET[i];
    if (!unet && id != PASS12) return 0;
    if (!codec_movie_live()) {   // only ever seen behind movies; whole outside them in any case
        static int told;
        if (!told++) rt_log("vk: the movies' network program %016llx runs while no movie plays", (unsigned long long)id);
        return 0;
    }
    return unet && vk_movienet_on();
}

int vk_movienet_is_pass12(const GfxShader *cs) { return cs && prog_id(cs) == PASS12; }

// Pass 12's module with its HMMA helper (dmsh_hmma16816_f16: ten words in, the second D word out through the eleventh
// parameter, the first returned) reduced to handing C through: no product of the features is added. malloc'd, *out_words
// its size; NULL for any other module.
uint32_t *vk_movienet_module(const GfxShader *cs, const uint32_t *code, uint64_t words, uint64_t *out_words) {
    if (!cs || prog_id(cs) != PASS12 || !code || words < 5 || code[0] != 0x07230203u) return NULL;
    uint64_t fn_at = 0, fn_body = 0, fn_end = 0, first_fn = 0;
    uint32_t fn = 0, ret_t = 0, params[11], np = 0;
    for (uint64_t i = 5; i < words;) {
        uint32_t n = code[i] >> 16, op = code[i] & 0xffff;
        if (!n || i + n > words) return NULL;
        const uint32_t *x = code + i + 1;
        if (op == 5 && n >= 3 && !strncmp((const char *)(x + 1), "dmsh_hmma16816_f16", (n - 2) * 4)) fn = x[0];   // OpName
        if (op == 54 && !first_fn) first_fn = i;
        if (op == 54 && fn && x[1] == fn) fn_at = i, ret_t = x[0];
        if (op == 55 && fn_at && !fn_body) {   // OpFunctionParameter
            if (np < 11) params[np] = x[1];
            np++;
        }
        if (fn_at && !fn_body && op != 54 && op != 55) fn_body = i;
        if (op == 56 && fn_at && !fn_end && i > fn_at) fn_end = i;
        i += n;
    }
    if (!fn_at || !fn_body || !fn_end || np != 11) return NULL;
    uint32_t bound = code[3];
    // the helper's own ids: their names and decorations go with the old body
    uint8_t *dead = calloc(((size_t)bound + 7) / 8, 1);
    uint32_t *o = malloc((size_t)words * 4 + 128);
    if (!dead || !o) {
        free(dead), free(o);
        return NULL;
    }
    for (uint64_t i = fn_body; i < fn_end;) {
        uint32_t n = code[i] >> 16, op = code[i] & 0xffff;
        uint32_t id = op == 248 ? code[i + 1] : n >= 3 ? code[i + 2] : 0;   // OpLabel's id, else the result after the type
        int none = op == 62 || op == 224 || op == 225 || op == 249 || op == 250 || op == 246 || op == 247 || op == 253 || op == 254;
        if (!none && id && id < bound) dead[id >> 3] |= (uint8_t)(1u << (id & 7));
        i += n;
    }
    uint64_t k = 0;
    memcpy(o, code, 5 * 4), k = 5;
    for (uint64_t i = 5; i < fn_body;) {   // everything up to the helper's body, less what named or decorated its ids
        uint32_t n = code[i] >> 16, op = code[i] & 0xffff, id = n >= 2 ? code[i + 1] : 0;
        int on_dead = i < first_fn && (op == 5 || op == 71 || op == 332) && id < bound && dead[id >> 3] >> (id & 7) & 1;
        if (!on_dead) memcpy(o + k, code + i, (size_t)n * 4), k += n;
        i += n;
    }
    free(dead);
    uint32_t label = bound++, c0 = bound++, c1 = bound++;
    o[k++] = 2u << 16 | 248, o[k++] = label;                                 // OpLabel
    o[k++] = 4u << 16 | 61, o[k++] = ret_t, o[k++] = c0, o[k++] = params[8];  // OpLoad C word 0
    o[k++] = 4u << 16 | 61, o[k++] = ret_t, o[k++] = c1, o[k++] = params[9];  // OpLoad C word 1
    o[k++] = 3u << 16 | 62, o[k++] = params[10], o[k++] = c1;                 // OpStore the second D word
    o[k++] = 2u << 16 | 254, o[k++] = c0;                                    // OpReturnValue the first
    memcpy(o + k, code + fn_end, (size_t)(words - fn_end) * 4), k += words - fn_end;
    o[3] = bound;
    *out_words = k;
    return o;
}
