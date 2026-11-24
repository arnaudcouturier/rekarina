// Ultrawide HUD anchoring and custom HUD placement for OoT 1.1.1 (recomp/patches-oot-v111.txt, set wide_ui).
//
// On screens wider than 16:9, widescreen.c's wide_hud patch squeezes the 2D projection horizontally so menus and HUD
// keep 16:9 proportions inside [-1920, +1920] x [-1080, +1080] canvas units. Shifting a side HUD pane outward by
// ex = 1920 * (reach / (16/9) - 1) places it at the ultrawide edge without distortion, and scaling a spanning pane's
// drawn width by wx = 1 + ex / 1920 lets it span the full width.
//
// In Custom HUD mode (REKARINA_HUD_LAYOUT=custom), three HUD groups (Hearts & magic, Action & item buttons, Minimap)
// additionally receive a live, bounds-clamped affine scale and offset in canvas space. Custom placement works at 16:9
// and ultrawide alike.
//
// The 2D layout structure offsets and hook sites in OoT 1.1.1 were identified from Hookshot PR #2843 and Longshot
// (GPL-3.0, read only for what the game does); this planner, cache, and hook layer is an original implementation for
// Rekarina's patch-page mechanism.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wide_ui.h"

#define WIDE_CANVAS_HALF_X 1920.0f
#define WIDE_CANVAS_HALF_Y 1080.0f
#define WIDE_GAME_ASPECT   (16.0f / 9.0f)
#define WIDE_DEFAULT_MAX   (12.0f / 5.0f)   // cap edge push at 21:9 (3840x1600) so 32:9 stays comfortable
#define WIDE_SURROUND      (48.0f / 9.0f)   // triple-monitor surround keeps HUD on center display
#define WIDE_EDGE_NEAR     150.0f
#define WIDE_EDGE_TOL      40.0f
#define WIDE_SIDE_MID      1100.0f
#define WIDE_CROSS_SHARE   0.25f
#define WIDE_SIZE_CAP      960.0f

// ------------------------------------------------------------------------------------------------ pure planning logic

float wide_ui_reach_ratio(float aspect_ratio, float max_ratio) {
    if (!(aspect_ratio > WIDE_GAME_ASPECT * 1.001f)) return WIDE_GAME_ASPECT;
    if (aspect_ratio >= WIDE_SURROUND * 0.99f && max_ratio == WIDE_DEFAULT_MAX) return WIDE_GAME_ASPECT;
    if (max_ratio > 0.0f && aspect_ratio > max_ratio)
        return max_ratio > WIDE_GAME_ASPECT ? max_ratio : WIDE_GAME_ASPECT;
    return aspect_ratio;
}

float wide_ui_edge_shift(float aspect_ratio, float max_ratio) {
    float reach = wide_ui_reach_ratio(aspect_ratio, max_ratio);
    if (!(reach > WIDE_GAME_ASPECT * 1.001f)) return 0.0f;
    return WIDE_CANVAS_HALF_X * (reach / WIDE_GAME_ASPECT - 1.0f);
}

static float clamp_axis_offset(float offset_frac, float scale, float lo, float hi, float half_extent) {
    float center = 0.5f * (lo + hi);
    float half_group = 0.5f * (hi - lo) * scale;
    float limit = half_extent * 0.99f - half_group;
    if (limit < 0.0f) limit = 0.0f;
    float target = center + 2.0f * half_extent * offset_frac;
    if (target > limit) target = limit;
    if (target < -limit) target = -limit;
    return target - scale * center;
}

WideHudTransform wide_ui_group_transform(WideHudGroupCfg cfg, float lo_x, float hi_x, float lo_y, float hi_y,
                                         float half_x, float half_y) {
    WideHudTransform id = {1.0f, 0.0f, 0.0f};
    if (cfg.x_pct == 0 && cfg.y_pct == 0 && cfg.scale_pct == 100) return id;
    if (!isfinite(lo_x) || !isfinite(hi_x) || !isfinite(lo_y) || !isfinite(hi_y) || hi_x < lo_x || hi_y < lo_y)
        return id;
    if (cfg.x_pct < -15 || cfg.x_pct > 15 || cfg.y_pct < -15 || cfg.y_pct > 15 ||
        cfg.scale_pct < 75 || cfg.scale_pct > 125)
        return id;
    float s = (float)cfg.scale_pct / 100.0f;
    float fx = (float)cfg.x_pct / 100.0f;
    float fy = (float)cfg.y_pct / 100.0f;
    WideHudTransform t;
    t.scale = s;
    t.tx = clamp_axis_offset(fx, s, lo_x, hi_x, half_x);
    t.ty = clamp_axis_offset(fy, s, lo_y, hi_y, half_y);
    return t;
}

static int pane_is_drawn(uint8_t type) { return type != WIDE_PANE_NULL && type != WIDE_PANE_PART; }

typedef struct {
    const WidePaneInput *in;
    WidePanePlan *out;
    int count;
    float ex;
    float wx;
    int32_t *first_child;
    int32_t *next_sibling;
    float *rest_dx;
    uint8_t *rest_ready;
    float *ext_lo;
    float *ext_hi;
    uint8_t *ext_ready;
} PlanCtx;

static float compute_rest(PlanCtx *ctx, int i, int depth) {
    if (ctx->rest_ready[i] || depth > 64) return ctx->rest_dx[i];
    const WidePaneInput *p = &ctx->in[i];
    int par = p->parent;
    float rest_x = p->ox;
    if (par >= 0 && par < ctx->count && par != i) {
        float par_rest = ctx->in[par].ox - compute_rest(ctx, par, depth + 1);
        if (p->rule == WIDE_RULE_SLIDE) rest_x = par_rest;
        else rest_x = par_rest + p->ox - (isnan(p->parent_ox) ? ctx->in[par].ox : p->parent_ox);
    }
    ctx->rest_dx[i] = p->ox - rest_x;
    ctx->rest_ready[i] = 1;
    return ctx->rest_dx[i];
}

static void compute_extents(PlanCtx *ctx, int i, int depth) {
    if (ctx->ext_ready[i]) return;
    const WidePaneInput *p = &ctx->in[i];
    float r = ctx->rest_dx[i];
    float lo = p->ox - r;
    float hi = p->ox - r;
    if (pane_is_drawn(p->type) && (p->x1 - p->x0) < WIDE_SIZE_CAP) {
        float x0 = p->x0 - r, x1 = p->x1 - r;
        if (x0 < lo) lo = x0;
        if (x1 > hi) hi = x1;
    }
    if (depth < 48) {
        for (int c = ctx->first_child[i]; c >= 0; c = ctx->next_sibling[c]) {
            compute_extents(ctx, c, depth + 1);
            if (ctx->ext_lo[c] < lo) lo = ctx->ext_lo[c];
            if (ctx->ext_hi[c] > hi) hi = ctx->ext_hi[c];
        }
    }
    ctx->ext_lo[i] = lo;
    ctx->ext_hi[i] = hi;
    ctx->ext_ready[i] = 1;
}

static void plan_visit(PlanCtx *ctx, int node, int depth);

static void plan_widen(PlanCtx *ctx, int node, int depth) {
    const WidePaneInput *p = &ctx->in[node];
    float mid = 0.5f * (p->x0 + p->x1) - ctx->rest_dx[node];
    if (pane_is_drawn(p->type) && mid * mid < 1.0f) ctx->out[node].wide = ctx->wx;
    plan_visit(ctx, node, depth + 1);
}

static void plan_visit(PlanCtx *ctx, int node, int depth) {
    if (depth > 48) return;
    const float E = WIDE_CANVAS_HALF_X;
    int side_nodes[2][64];
    int side_n[2] = {0, 0};
    float side_lo[2] = {INFINITY, INFINITY};
    float side_hi[2] = {-INFINITY, -INFINITY};

    for (int c = ctx->first_child[node]; c >= 0; c = ctx->next_sibling[c]) {
        const WidePaneInput *p = &ctx->in[c];
        if (!ctx->out[c].in_hud) continue;
        if (p->is_map && !ctx->out[c].as_menu && (p->is_minimap || p->rule == WIDE_RULE_MAP_ORIGIN)) continue;

        float r = ctx->rest_dx[c];
        float ox = p->ox - r;
        float x0 = p->x0 - r;
        float x1 = p->x1 - r;

        if (p->is_world || p->rule == WIDE_RULE_KEEP || p->rule == WIDE_RULE_ANCHOR_LEFT ||
            p->rule == WIDE_RULE_ANCHOR_RIGHT)
            continue;
        if (p->rule == WIDE_RULE_MAP_ORIGIN && fabsf(ox) < 1.0f) continue;
        if (p->rule == WIDE_RULE_DESCEND) {
            plan_visit(ctx, c, depth + 1);
            continue;
        }
        if (p->rule == WIDE_RULE_WIDEN) {
            plan_widen(ctx, c, depth);
            continue;
        }
        if (p->type == WIDE_PANE_PART && fabsf(ox) < 1.0f) {
            plan_visit(ctx, c, depth + 1);
            continue;
        }

        compute_extents(ctx, c, 0);
        float lo = ctx->ext_lo[c];
        float hi = ctx->ext_hi[c];
        if (pane_is_drawn(p->type)) {
            if (x0 <= -E + WIDE_EDGE_TOL && x1 >= E - WIDE_EDGE_TOL) {
                plan_widen(ctx, c, depth);
                continue;
            }
            if (ctx->first_child[c] < 0) {
                if (x0 < lo) lo = x0;
                if (x1 > hi) hi = x1;
            }
        }
        if (lo >= E || hi <= -E || ((hi - lo) > 2.5f * E && fabsf(ox) > 1.0f)) continue;
        if (lo < -E - WIDE_EDGE_NEAR) lo = -E - WIDE_EDGE_NEAR;
        if (hi > E + WIDE_EDGE_NEAR) hi = E + WIDE_EDGE_NEAR;

        if (lo <= -E + WIDE_EDGE_TOL && hi >= E - WIDE_EDGE_TOL) {
            plan_visit(ctx, c, depth + 1);
            continue;
        }
        if (lo < 0.0f && hi > 0.0f && fminf(-lo, hi) > WIDE_CROSS_SHARE * (hi - lo)) {
            int near_r = hi >= E - WIDE_EDGE_NEAR;
            int near_l = lo <= -E + WIDE_EDGE_NEAR;
            if (near_r != near_l && (ctx->first_child[c] < 0 || (hi - lo) < E))
                ctx->out[c].dx = near_r ? ctx->ex : -ctx->ex;
            else
                plan_visit(ctx, c, depth + 1);
            continue;
        }
        if (lo == 0.0f || hi == 0.0f ||
            (lo < 0.0f && hi > 0.0f &&
             (fabsf(0.5f * (lo + hi)) < WIDE_EDGE_NEAR || (ctx->first_child[c] >= 0 && fabsf(ox) < 1.0f)))) {
            plan_visit(ctx, c, depth + 1);
            continue;
        }

        int s = (lo + hi >= 0.0f) ? 1 : 0;
        if (side_n[s] < 64) side_nodes[s][side_n[s]++] = c;
        if (lo < side_lo[s]) side_lo[s] = lo;
        if (hi > side_hi[s]) side_hi[s] = hi;
    }

    for (int s = 0; s < 2; s++) {
        if (!side_n[s]) continue;
        float outer = s ? side_hi[s] : -side_lo[s];
        float mid = fabsf(0.5f * (side_lo[s] + side_hi[s]));
        int anchor = outer >= E - WIDE_EDGE_NEAR || mid >= WIDE_SIDE_MID;
        for (int k = 0; k < side_n[s]; k++) {
            int idx = side_nodes[s][k];
            if (anchor) ctx->out[idx].dx = s ? ctx->ex : -ctx->ex;
            else plan_visit(ctx, idx, depth + 1);
        }
    }
}

void wide_ui_plan_panes(const WidePaneInput *in, WidePanePlan *out, int count, float ex, float wx) {
    if (count <= 0) return;
    int32_t *first_child = (int32_t *)malloc((size_t)count * sizeof *first_child);
    int32_t *next_sibling = (int32_t *)malloc((size_t)count * sizeof *next_sibling);
    float *rest_dx = (float *)calloc((size_t)count, sizeof *rest_dx);
    uint8_t *rest_ready = (uint8_t *)calloc((size_t)count, sizeof *rest_ready);
    float *ext_lo = (float *)calloc((size_t)count, sizeof *ext_lo);
    float *ext_hi = (float *)calloc((size_t)count, sizeof *ext_hi);
    uint8_t *ext_ready = (uint8_t *)calloc((size_t)count, sizeof *ext_ready);
    float *sc_move = (float *)calloc((size_t)count, sizeof *sc_move);
    uint8_t *sc_mixed = (uint8_t *)calloc((size_t)count, sizeof *sc_mixed);

    if (!first_child || !next_sibling || !rest_dx || !rest_ready || !ext_lo || !ext_hi || !ext_ready || !sc_move ||
        !sc_mixed) {
        free(first_child); free(next_sibling); free(rest_dx); free(rest_ready);
        free(ext_lo); free(ext_hi); free(ext_ready); free(sc_move); free(sc_mixed);
        for (int i = 0; i < count; i++) out[i] = (WidePanePlan){0.0f, 1.0f, 0, 0};
        return;
    }

    for (int i = 0; i < count; i++) {
        first_child[i] = -1;
        next_sibling[i] = -1;
        out[i] = (WidePanePlan){0.0f, 1.0f, 0, 0};
    }
    for (int i = 0; i < count; i++) {
        int par = in[i].parent;
        if (par >= 0 && par < count && par != i) {
            next_sibling[i] = first_child[par];
            first_child[par] = i;
        }
    }

    PlanCtx ctx = {in, out, count, ex, wx, first_child, next_sibling, rest_dx, rest_ready, ext_lo, ext_hi, ext_ready};
    for (int i = 0; i < count; i++) compute_rest(&ctx, i, 0);

    // Detect MapMenu_00 layouts currently displaying the pause-menu map page (N_MapIcon_00 sits at canvas origin).
    uint64_t map_tab_layouts[16];
    int map_tab_n = 0;
    for (int i = 0; i < count; i++) {
        if (in[i].rule != WIDE_RULE_MAP_ORIGIN) continue;
        if (fabsf(in[i].ox - rest_dx[i]) >= 1.0f) continue;
        int seen = 0;
        for (int k = 0; k < map_tab_n; k++)
            if (map_tab_layouts[k] == in[i].layout_id) { seen = 1; break; }
        if (!seen && map_tab_n < 16) map_tab_layouts[map_tab_n++] = in[i].layout_id;
    }

    for (int i = 0; i < count; i++) {
        int top = i;
        for (int d = 0; d < 64 && in[top].parent >= 0 && in[top].parent < count && in[top].parent != top; d++)
            top = in[top].parent;
        int tab = 0;
        if (in[top].is_map) {
            for (int k = 0; k < map_tab_n; k++)
                if (map_tab_layouts[k] == in[top].layout_id) { tab = 1; break; }
        }
        out[top].as_menu = (uint8_t)tab;
        out[i].as_menu = (uint8_t)tab;
        out[i].in_hud = (uint8_t)(in[top].is_hud && !tab);
    }

    if (ex != 0.0f) {
        // Walk each active HUD root tree.
        for (int i = 0; i < count; i++) {
            if (in[i].parent >= 0 || !out[i].in_hud || in[i].is_world) continue;
            plan_visit(&ctx, i, 0);
        }

        // Explicit named edge anchors (unless an ancestor already shifted).
        for (int i = 0; i < count; i++) {
            if (!out[i].in_hud) continue;
            if (in[i].rule != WIDE_RULE_ANCHOR_LEFT && in[i].rule != WIDE_RULE_ANCHOR_RIGHT) continue;
            int anc_moved = 0;
            for (int a = in[i].parent, d = 0; a >= 0 && a < count && d < 64 && !anc_moved; a = in[a].parent, d++)
                anc_moved = (out[a].dx != 0.0f);
            if (!anc_moved) out[i].dx = (in[i].rule == WIDE_RULE_ANCHOR_LEFT) ? -ex : ex;
        }

        // Live minimap siblings share one consistent right-edge shift so map, scissor, icons, and keys stay aligned.
        for (int i = 0; i < count; i++) {
            if (!out[i].in_hud || !in[i].is_map || out[i].as_menu) continue;
            if (in[i].is_minimap || in[i].rule == WIDE_RULE_MAP_ORIGIN) out[i].dx = ex;
        }

        // Move enclosing scissor panes with their contents when every moved descendant inside them shifts the same way.
        for (int i = 0; i < count; i++) {
            if (out[i].dx == 0.0f) continue;
            for (int a = in[i].parent, d = 0; a >= 0 && a < count && d < 64; a = in[a].parent, d++) {
                if (out[a].dx != 0.0f) break;
                if (in[a].type != WIDE_PANE_SCISSOR) continue;
                if (sc_move[a] == 0.0f) sc_move[a] = out[i].dx;
                else if (sc_move[a] != out[i].dx) sc_mixed[a] = 1;
                break;
            }
        }
        for (int i = 0; i < count; i++)
            if (sc_move[i] != 0.0f && !sc_mixed[i] && out[i].in_hud) out[i].dx = sc_move[i];
    }

    free(first_child); free(next_sibling); free(rest_dx); free(rest_ready);
    free(ext_lo); free(ext_hi); free(ext_ready); free(sc_move); free(sc_mixed);
}

// ------------------------------------------------------------------------------------------------ guest runtime hooks

#ifndef WIDE_UI_STANDALONE_TEST

#include "cfg.h"
#include "host.h"
#include "platform/platform.h"

#if __has_include("patches.h")
#include "layout.h"
#include "patches.h"
#endif

#if defined(GAME_OOT) && defined(RT_PATCH_PAGE)

#define WIDE_UI_FORCE_OFF  224u   // patch page +0xe0: 0 or 0x40 (bit 6 forces pane transform rebuild)
#define HOST_WIDE_UI_SIZE  (RT_PATCH_PAGE + 0xcc0)
#define HOST_WIDE_UI_PLACE (RT_PATCH_PAGE + 0xcd0)
#define HOST_WIDE_UI_PAR1  (RT_PATCH_PAGE + 0xce0)
#define HOST_WIDE_UI_PAR0  (RT_PATCH_PAGE + 0xcf0)

// OoT 1.1.1 2D layout & pane struct offsets.
enum {
    LYT_RES        = 0xf8,
    LYT_PANES      = 0x100,
    LYT_COUNT      = 0x13c,
    RES_LYT_NAME   = 0x18,
    RES_PANE_NAMES = 0x20,
    PANE_STRIDE    = 0xb0,
    PANE_LAYOUT    = 0x18,
    PANE_XX        = 0x28,   // 3x4 affine matrix: col0 (+0x28..+0x30), col1 (+0x34..+0x3c), col2 (+0x40..+0x48), trans (+0x4c..+0x54)
    PANE_YY        = 0x38,
    PANE_X         = 0x4c,
    PANE_Y         = 0x50,
    PANE_W         = 0x58,
    PANE_H         = 0x5c,
    PANE_FLAGS     = 0xa8,
};

enum { PANE_CAP = 1 << 15 };   // 32768 open-addressing slots for tracked HUD panes

typedef struct {
    uint64_t key;       // guest pane pointer (0 = empty)
    uint64_t parent;    // guest parent pane pointer (0 = root)
    uint64_t layout;    // guest layout pointer
    uint64_t lname;     // guest layout name string pointer
    uint64_t pname;     // guest pane name string pointer
    float ox, x0, x1;
    float oy, y0, y1;
    float parent_ox;
    float plan_dx;
    float plan_wide;
    float moved_x;      // cumulative horizontal shift applied to this pane + ancestors
    float pushed_x;     // non-zero if this pane or an ancestor already took an edge push
    WideHudTransform placed;   // cumulative custom affine transform inherited by children
    uint8_t type;
    uint8_t rule;
    uint8_t is_hud;
    uint8_t is_world;
    uint8_t is_map;
    uint8_t is_minimap;
    uint8_t is_action;
    uint8_t custom_group;   // 0 = none, else WIDE_HUD_GROUP_* + 1
    uint8_t in_hud;
    uint8_t as_menu;
    uint8_t tree_ok;        // 1 if children should also be tracked
    uint8_t dead;
} PaneNode;

static PaneNode g_nodes[PANE_CAP];
static uint64_t g_used_bits[PANE_CAP / 64];
static PlatMutex g_lock = PLAT_MUTEX_INIT;

static int g_active;        // atomic: 1 when either edge shift or custom placement is active
static int g_custom;        // atomic: 1 when custom placement is active
static float g_ex;          // current horizontal edge shift in canvas units
static float g_wx = 1.0f;   // current spanning width multiplier
static float g_half_x = WIDE_CANVAS_HALF_X;
static float g_half_y = WIDE_CANVAS_HALF_Y;
static int g_force_frames;
static int g_log_budget;

static WideHudGroupCfg g_group_cfg[WIDE_HUD_GROUP_COUNT];
static WideHudTransform g_group_xf[WIDE_HUD_GROUP_COUNT];

static _Thread_local uint64_t tls_parent;

static uint64_t g_u64(uint64_t at) { uint64_t v = 0; if (at >= 0x10000 && at < RT_ADDRESS_LIMIT) memcpy(&v, (const void *)(uintptr_t)at, 8); return v; }
static uint32_t g_u32(uint64_t at) { uint32_t v = 0; if (at >= 0x10000 && at < RT_ADDRESS_LIMIT) memcpy(&v, (const void *)(uintptr_t)at, 4); return v; }
static float g_f32(uint64_t at) { uint32_t u = g_u32(at); float f; memcpy(&f, &u, 4); return f; }
static void p_f32(uint64_t at, float f) { if (at >= 0x10000 && at < RT_ADDRESS_LIMIT) memcpy((void *)(uintptr_t)at, &f, 4); }

static int g_str_eq(uint64_t at, const char *s) {
    if (at < 0x10000 || at >= RT_ADDRESS_LIMIT) return 0;
    const char *p = (const char *)(uintptr_t)at;
    for (int i = 0; i < 64; i++) {
        if (p[i] != s[i]) return 0;
        if (!p[i]) return 1;
    }
    return 0;
}

static int g_str_in(uint64_t at, const char *const *list, int n) {
    for (int i = 0; i < n; i++)
        if (g_str_eq(at, list[i])) return 1;
    return 0;
}

static uint32_t pane_hash(uint64_t key) { return (uint32_t)((key >> 4) * 0x9E3779B1u) & (PANE_CAP - 1); }

static PaneNode *pane_find(uint64_t key) {
    for (uint32_t i = pane_hash(key), n = 0; n < 64; i = (i + 1) & (PANE_CAP - 1), n++) {
        uint64_t k = g_nodes[i].key;
        if (k == key) return &g_nodes[i];
        if (!k) return NULL;
    }
    return NULL;
}

static void pane_clear_body(PaneNode *n) {
    uint64_t k = n->key;
    memset(n, 0, sizeof *n);
    n->key = k;
    n->plan_wide = 1.0f;
    n->placed = (WideHudTransform){1.0f, 0.0f, 0.0f};
    n->parent_ox = NAN;
}

static PaneNode *pane_take(uint64_t key) {
    PaneNode *ex = pane_find(key);
    if (ex) {
        if (ex->dead) pane_clear_body(ex);
        return ex;
    }
    for (uint32_t i = pane_hash(key), n = 0; n < 64; i = (i + 1) & (PANE_CAP - 1), n++) {
        if (!g_nodes[i].key || g_nodes[i].dead) {
            g_nodes[i].key = key;
            pane_clear_body(&g_nodes[i]);
            g_used_bits[i / 64] |= 1ull << (i % 64);
            return &g_nodes[i];
        }
    }
    return NULL;
}

static void pane_drop(PaneNode *n) {
    n->plan_dx = 0.0f;
    n->plan_wide = 1.0f;
    n->layout = 0;
    n->dead = 1;
}

static int pane_alive(const PaneNode *n) {
    if (!n->key || !n->layout || n->dead) return 0;
    if (!plat_mem_readable(n->key + PANE_LAYOUT, 8) || g_u64(n->key + PANE_LAYOUT) != n->layout) return 0;
    if (!plat_mem_readable(n->layout + LYT_PANES, 8) || !plat_mem_readable(n->layout + LYT_COUNT, 4)) return 0;
    uint64_t base = g_u64(n->layout + LYT_PANES);
    uint32_t cnt = g_u32(n->layout + LYT_COUNT) & 0xffffu;
    return base && n->key >= base && (n->key - base) % PANE_STRIDE == 0 && (n->key - base) / PANE_STRIDE < cnt;
}

static const char *const HUD_ROOT_NAMES[] = {
    "PlayerStatus_00", "Location_00", "MapMenu_00", "Talk_00", "MessageLog_01",
    "DoCommand_00", "PlayerStatus3D_00", "Pointer_00", "BalloonMessage_00",
};
static const char *const WORLD_ROOT_NAMES[] = {
    "PlayerStatus3D_00", "Pointer_00", "BalloonMessage_00",
};

typedef struct { const char *layout, *pane; uint8_t rule; } NamedRule;
static const NamedRule NAMED_RULES[] = {
    {"PlayerStatus_00", "N_ScaleL_00",      WIDE_RULE_DESCEND},
    {"PlayerStatus_00", "N_ScaleL_01",      WIDE_RULE_DESCEND},
    {"PlayerStatus_00", "Pa_HeartGauge_00", WIDE_RULE_ANCHOR_LEFT},
    {"PlayerStatus_00", "Pa_MpGauge_00",    WIDE_RULE_ANCHOR_LEFT},
    {"PlayerStatus_00", "Pa_AgonyStone_00", WIDE_RULE_ANCHOR_LEFT},
    {"MapMenu_00",      "N_MenuIn_00",      WIDE_RULE_SLIDE},
    {"MapMenu_00",      "N_InChallenge_00", WIDE_RULE_SLIDE},
    {"MapMenu_00",      "N_MapIcon_00",     WIDE_RULE_MAP_ORIGIN},
    {"MapMenu_00",      "Pa_MapIcon_00",    WIDE_RULE_KEEP},
    {"MapMenu_00",      "Pa_WorldMap_00",   WIDE_RULE_KEEP},
    {"Location_00",     "P_DecoFrameLT_00",   WIDE_RULE_WIDEN},
    {"Location_00",     "P_DecoFrameLTSh_00", WIDE_RULE_WIDEN},
    {"Location_00",     "P_DecoFrameRT_00",   WIDE_RULE_WIDEN},
    {"Location_00",     "P_DecoFrameRTSh_00", WIDE_RULE_WIDEN},
    {"Location_00",     "P_DecoFrameLB_00",   WIDE_RULE_WIDEN},
    {"Location_00",     "P_DecoFrameLBSh_00", WIDE_RULE_WIDEN},
    {"Location_00",     "P_DecoFrameRB_00",   WIDE_RULE_WIDEN},
    {"Location_00",     "P_DecoFrameRBSh_00", WIDE_RULE_WIDEN},
};

static void classify_pane(PaneNode *n, uint64_t pane, uint64_t layout) {
    uint64_t res = layout ? g_u64(layout + LYT_RES) : 0;
    uint64_t lname = res ? g_u64(res + RES_LYT_NAME) : 0;
    uint64_t names = res ? g_u64(res + RES_PANE_NAMES) : 0;
    uint64_t base = layout ? g_u64(layout + LYT_PANES) : 0;
    uint64_t pname = (names && base && pane >= base) ? g_u64(names + ((pane - base) / PANE_STRIDE) * 8) : 0;

    n->lname = lname;
    n->pname = pname;
    n->rule = WIDE_RULE_NONE;
    n->is_hud = (uint8_t)g_str_in(lname, HUD_ROOT_NAMES, (int)(sizeof HUD_ROOT_NAMES / sizeof HUD_ROOT_NAMES[0]));
    n->is_world = (uint8_t)g_str_in(lname, WORLD_ROOT_NAMES, (int)(sizeof WORLD_ROOT_NAMES / sizeof WORLD_ROOT_NAMES[0]));
    n->is_map = (uint8_t)g_str_eq(lname, "MapMenu_00");
    n->is_action = (uint8_t)g_str_eq(lname, "DoCommand_00");
    n->is_minimap = (uint8_t)(n->is_map && (g_str_eq(pname, "N_InOut_00") || g_str_eq(pname, "Pa_KeySmall_00")));
    n->custom_group = 0;

    if (g_str_eq(lname, "PlayerStatus_00")) {
        if (g_str_eq(pname, "Pa_HeartGauge_00") || g_str_eq(pname, "Pa_MpGauge_00"))
            n->custom_group = WIDE_HUD_GROUP_HEALTH + 1;
        else if (g_str_eq(pname, "Pa_SelectItem_00"))
            n->custom_group = WIDE_HUD_GROUP_ITEMS + 1;
    } else if (n->is_action && g_str_eq(pname, "N_PosType_00")) {
        n->custom_group = WIDE_HUD_GROUP_ACTION + 1;
    } else if (n->is_minimap) {
        n->custom_group = WIDE_HUD_GROUP_MINIMAP + 1;
    }

    for (size_t i = 0; pname && i < sizeof NAMED_RULES / sizeof NAMED_RULES[0]; i++) {
        if (g_str_eq(lname, NAMED_RULES[i].layout) && g_str_eq(pname, NAMED_RULES[i].pane)) {
            n->rule = NAMED_RULES[i].rule;
            break;
        }
    }
}

static void write_force_mask(uint32_t mask) {
    uint8_t *page = (uint8_t *)rt_patch_page();
    if (!page) return;
    __atomic_store_n((uint32_t *)(page + WIDE_UI_FORCE_OFF), mask, __ATOMIC_RELEASE);
}

static int read_hud_mode(void) {
    const char *s = cfg_value("REKARINA_HUD_LAYOUT");
    if (!strcmp(s, "center")) return WIDE_HUD_CENTER;
    if (!strcmp(s, "custom")) return WIDE_HUD_CUSTOM;
    return WIDE_HUD_EDGES;
}

static int group_is_default(WideHudGroupCfg c) { return c.x_pct == 0 && c.y_pct == 0 && c.scale_pct == 100; }

// ---- guest callbacks (registered on patch page +0xcc0..+0xcf0)

static void cb_parent_none(Cpu *c) { (void)c; tls_parent = 0; }
static void cb_parent_set(Cpu *c) { tls_parent = c->x[0]; }

static void cb_placed(Cpu *c) {
    uint64_t pane = c->x[19];
    uint64_t parent = tls_parent;
    tls_parent = 0;
    if (!__atomic_load_n(&g_active, __ATOMIC_ACQUIRE)) return;

    plat_mutex_lock(&g_lock);
    if (!__atomic_load_n(&g_active, __ATOMIC_RELAXED)) { plat_mutex_unlock(&g_lock); return; }

    PaneNode *pn = parent ? pane_find(parent) : NULL;
    if (parent && !(pn && pn->layout && !pn->dead && pn->tree_ok)) {
        PaneNode *old = pane_find(pane);
        if (old && old->layout) pane_drop(old);
        plat_mutex_unlock(&g_lock);
        return;
    }

    PaneNode *n = pane_take(pane);
    if (!n) { plat_mutex_unlock(&g_lock); return; }

    float inh_x = pn ? pn->moved_x : 0.0f;
    float inh_push = pn ? pn->pushed_x : 0.0f;
    int custom_on = __atomic_load_n(&g_custom, __ATOMIC_RELAXED);
    WideHudTransform inh_xf = (custom_on && pn && pn->placed.scale > 0.0f)
                                  ? pn->placed
                                  : (WideHudTransform){1.0f, 0.0f, 0.0f};
    float inv_scale = inh_xf.scale != 1.0f ? 1.0f / inh_xf.scale : 1.0f;

    uint32_t flags = g_u32(pane + PANE_FLAGS);
    float sx = g_f32(pane + PANE_X);
    float sy = g_f32(pane + PANE_Y);
    float raw_x = (sx - inh_xf.tx) * inv_scale - inh_x;
    float raw_y = (sy - inh_xf.ty) * inv_scale;
    float w = g_f32(pane + PANE_W) * fabsf(g_f32(pane + PANE_XX)) * inv_scale;
    float h = g_f32(pane + PANE_H) * fabsf(g_f32(pane + PANE_YY)) * inv_scale;
    uint32_t align_x = ((flags >> 11) & 0x7fu) % 3u;
    uint32_t align_y = (((flags >> 11) & 0x7fu) / 3u) % 3u;

    uint64_t layout = g_u64(pane + PANE_LAYOUT);
    if (n->layout != layout) classify_pane(n, pane, layout);

    n->tree_ok = (uint8_t)(!n->is_world && (parent != 0 || n->is_hud));
    n->parent = parent;
    n->layout = layout;
    n->type = (uint8_t)((flags >> 22) & 0xfu);
    n->ox = raw_x;
    n->parent_ox = pn ? pn->ox : NAN;
    n->x0 = raw_x - (align_x == 1 ? 0.5f * w : align_x == 2 ? w : 0.0f);
    n->x1 = n->x0 + w;
    n->oy = raw_y;
    n->y1 = raw_y + (align_y == 1 ? 0.5f * h : align_y == 2 ? h : 0.0f);
    n->y0 = n->y1 - h;

    float side = (inh_push != 0.0f) ? 0.0f : n->plan_dx;
    n->moved_x = inh_x + side;
    n->pushed_x = inh_push + side;
    if (side != 0.0f) {
        sx += side * inh_xf.scale;
        p_f32(pane + PANE_X, sx);
    }

    n->placed = inh_xf;
    if (custom_on && n->custom_group && !(n->is_map && n->as_menu)) {
        int g = (int)n->custom_group - 1;
        // Apply only once per hierarchy branch (if an ancestor already applied custom transform, inherit it).
        int already = (inh_xf.scale != 1.0f || inh_xf.tx != 0.0f || inh_xf.ty != 0.0f);
        if (!already && g >= 0 && g < WIDE_HUD_GROUP_COUNT) {
            WideHudTransform adj = g_group_xf[g];
            if (adj.scale > 0.0f && (adj.scale != 1.0f || adj.tx != 0.0f || adj.ty != 0.0f)) {
                // Scale X/Y basis rows (+0x28,+0x2c, +0x34,+0x38, +0x40,+0x44); Z row is written by st1 right after.
                static const uint32_t xy_offs[6] = {0x28, 0x2c, 0x34, 0x38, 0x40, 0x44};
                for (int k = 0; k < 6; k++)
                    p_f32(pane + xy_offs[k], g_f32(pane + xy_offs[k]) * adj.scale);
                p_f32(pane + PANE_X, sx * adj.scale + adj.tx);
                p_f32(pane + PANE_Y, sy * adj.scale + adj.ty);
                n->placed = (WideHudTransform){
                    inh_xf.scale * adj.scale,
                    inh_xf.tx * adj.scale + adj.tx,
                    inh_xf.ty * adj.scale + adj.ty,
                };
            }
        }
    }

    if (n->plan_wide != 1.0f && n->plan_wide > 0.0f && n->type == WIDE_PANE_CAPTURE) {
        p_f32(pane + PANE_XX, g_f32(pane + PANE_XX) * n->plan_wide);
        p_f32(pane + PANE_XX + 4, g_f32(pane + PANE_XX + 4) * n->plan_wide);
    }
    plat_mutex_unlock(&g_lock);
}

static void cb_size(Cpu *c) {
    if (!__atomic_load_n(&g_active, __ATOMIC_ACQUIRE)) return;
    uint64_t pane = c->x[0];
    plat_mutex_lock(&g_lock);
    PaneNode *n = pane_find(pane);
    float wscale = (n && !n->dead && n->type != WIDE_PANE_CAPTURE && n->plan_wide > 0.0f) ? n->plan_wide : 1.0f;
    plat_mutex_unlock(&g_lock);
    if (wscale != 1.0f) {
        uint32_t bits = (uint32_t)c->v[0];
        float v;
        memcpy(&v, &bits, 4);
        v *= wscale;
        memcpy(&bits, &v, 4);
        c->v[0] = (c->v[0] & ~(u128)0xffffffffu) | bits;
    }
}

// ---- per-present planner (called on presenting thread via nvnQueuePresentTexture)

static int32_t g_live_slots[PANE_CAP];
static WidePaneInput g_plan_in[PANE_CAP];
static WidePanePlan g_plan_out[PANE_CAP];
static int32_t g_slot_to_dense[PANE_CAP];

static void run_planner_locked(int custom_on) {
    int n_live = 0;
    for (int w = 0; w < PANE_CAP / 64; w++) {
        for (uint64_t bits = g_used_bits[w]; bits; bits &= bits - 1) {
            int slot = w * 64 + __builtin_ctzll(bits);
            PaneNode *n = &g_nodes[slot];
            if (!n->key || n->dead) continue;
            if (!pane_alive(n)) {
                pane_drop(n);
                continue;
            }
            g_slot_to_dense[slot] = n_live;
            g_live_slots[n_live++] = slot;
        }
    }

    for (int i = 0; i < n_live; i++) {
        const PaneNode *n = &g_nodes[g_live_slots[i]];
        int32_t par = -1;
        if (n->parent) {
            PaneNode *pn = pane_find(n->parent);
            if (pn && !pn->dead && pn->layout) {
                int pslot = (int)(pn - g_nodes);
                int pidx = g_slot_to_dense[pslot];
                if (pidx >= 0 && pidx < n_live && g_live_slots[pidx] == pslot) par = pidx;
            }
        }
        g_plan_in[i] = (WidePaneInput){
            .parent = par,
            .layout_id = n->layout,
            .ox = n->ox,
            .x0 = n->x0,
            .x1 = n->x1,
            .parent_ox = n->parent_ox,
            .type = n->type,
            .rule = n->rule,
            .is_hud = n->is_hud,
            .is_world = n->is_world,
            .is_map = n->is_map,
            .is_minimap = n->is_minimap,
        };
    }

    wide_ui_plan_panes(g_plan_in, g_plan_out, n_live, g_ex, g_wx);

    int changed = 0;
    if (custom_on) {
        float lo_x[WIDE_HUD_GROUP_COUNT], hi_x[WIDE_HUD_GROUP_COUNT];
        float lo_y[WIDE_HUD_GROUP_COUNT], hi_y[WIDE_HUD_GROUP_COUNT];
        for (int g = 0; g < WIDE_HUD_GROUP_COUNT; g++) {
            lo_x[g] = lo_y[g] = INFINITY;
            hi_x[g] = hi_y[g] = -INFINITY;
        }
        // Accumulate subtree bounds for health, items, and action groups (post edge shift).
        for (int i = 0; i < n_live; i++) {
            const PaneNode *n = &g_nodes[g_live_slots[i]];
            if (!n->custom_group || n->custom_group == WIDE_HUD_GROUP_MINIMAP + 1) continue;
            int g = (int)n->custom_group - 1;
            if (group_is_default(g_group_cfg[g])) continue;
            float dx = 0.0f;
            for (int a = i, d = 0; a >= 0 && d < 64; a = g_plan_in[a].parent, d++) {
                if (g_plan_out[a].dx != 0.0f) dx = g_plan_out[a].dx;
            }
            // Include the pane itself and all its live descendants.
            for (int j = 0; j < n_live; j++) {
                int in_sub = 0;
                for (int a = j, d = 0; a >= 0 && d < 64; a = g_plan_in[a].parent, d++) {
                    if (a == i) { in_sub = 1; break; }
                }
                if (!in_sub) continue;
                const PaneNode *c = &g_nodes[g_live_slots[j]];
                float x0 = c->ox + dx, x1 = c->ox + dx;
                float y0 = c->oy, y1 = c->oy;
                if (pane_is_drawn(c->type) && (c->x1 - c->x0) < WIDE_SIZE_CAP) {
                    if (c->x0 + dx < x0) x0 = c->x0 + dx;
                    if (c->x1 + dx > x1) x1 = c->x1 + dx;
                }
                if (pane_is_drawn(c->type) && (c->y1 - c->y0) < WIDE_SIZE_CAP) {
                    if (c->y0 < y0) y0 = c->y0;
                    if (c->y1 > y1) y1 = c->y1;
                }
                if (x0 < lo_x[g]) lo_x[g] = x0;
                if (x1 > hi_x[g]) hi_x[g] = x1;
                if (y0 < lo_y[g]) lo_y[g] = y0;
                if (y1 > hi_y[g]) hi_y[g] = y1;
            }
        }
        // Live minimap frame sits at [1200+ex, 1860+ex] x [-1040, -360] on the 3840x2160 canvas.
        lo_x[WIDE_HUD_GROUP_MINIMAP] = 1200.0f + g_ex;
        hi_x[WIDE_HUD_GROUP_MINIMAP] = 1860.0f + g_ex;
        lo_y[WIDE_HUD_GROUP_MINIMAP] = -1040.0f;
        hi_y[WIDE_HUD_GROUP_MINIMAP] = -360.0f;

        for (int g = 0; g < WIDE_HUD_GROUP_COUNT; g++) {
            WideHudTransform next = wide_ui_group_transform(g_group_cfg[g], lo_x[g], hi_x[g], lo_y[g], hi_y[g],
                                                            g_half_x, g_half_y);
            if (next.scale != g_group_xf[g].scale || fabsf(next.tx - g_group_xf[g].tx) >= 0.5f ||
                fabsf(next.ty - g_group_xf[g].ty) >= 0.5f) {
                g_group_xf[g] = next;
                changed = 1;
            }
        }
    }

    for (int i = 0; i < n_live; i++) {
        PaneNode *n = &g_nodes[g_live_slots[i]];
        const WidePanePlan *p = &g_plan_out[i];
        if (n->plan_dx == p->dx && n->plan_wide == p->wide && n->in_hud == p->in_hud && n->as_menu == p->as_menu)
            continue;
        if (g_log_budget > 0) {
            g_log_budget--;
            char ln[48] = "", pn[48] = "";
            if (n->lname >= 0x10000 && n->lname < RT_ADDRESS_LIMIT) snprintf(ln, sizeof ln, "%.47s", (const char *)(uintptr_t)n->lname);
            if (n->pname >= 0x10000 && n->pname < RT_ADDRESS_LIMIT) snprintf(pn, sizeof pn, "%.47s", (const char *)(uintptr_t)n->pname);
            rt_log("wide ui: %s:%s dx=%.1f wide=%.3f hud=%u map_menu=%u", ln, pn, p->dx, p->wide, p->in_hud, p->as_menu);
        }
        n->plan_dx = p->dx;
        n->plan_wide = p->wide;
        n->in_hud = p->in_hud;
        n->as_menu = p->as_menu;
        changed = 1;
    }

    if (changed && g_force_frames < 3) g_force_frames = 3;
}

int wide_ui_available(void) { return rt_patch_set("wide_ui") >= 0; }

void wide_ui_init(void) {
    int sw = rt_patch_set("wide_ui");
    if (sw < 0) return;
    const char *env = cfg_env("REKARINA_PATCH_WIDE_UI");
    if (env && !strcmp(env, "0")) {
        rt_patch_switch(sw, 0);
        return;
    }
    write_force_mask(0);
    rt_patch_switch(sw, 1);
}

void wide_ui_tick(void) {
    if (!wide_ui_available() || !rt_patch_enabled(rt_patch_set("wide_ui"))) return;

    static int inited;
    static float max_ratio = WIDE_DEFAULT_MAX;
    static int last_mode = -1;
    static int last_any_custom = -1;
    static float last_ratio = -1.0f;
    static WideHudGroupCfg last_cfg[WIDE_HUD_GROUP_COUNT];

    if (!inited) {
        inited = 1;
        const char *e = cfg_env("REKARINA_HUD_MAX_RATIO");
        if (e && *e) max_ratio = (float)atof(e);
        const char *l = cfg_env("REKARINA_WIDE_UI_LOG");
        if (l && *l) g_log_budget = atoi(l);
        for (int g = 0; g < WIDE_HUD_GROUP_COUNT; g++) {
            g_group_xf[g] = (WideHudTransform){1.0f, 0.0f, 0.0f};
            last_cfg[g] = (WideHudGroupCfg){0, 0, 100};
        }
    }

    int mode = read_hud_mode();
    float ratio = wide_aspect_ratio();
    WideHudGroupCfg cfg[WIDE_HUD_GROUP_COUNT];
    cfg[WIDE_HUD_GROUP_HEALTH] = (WideHudGroupCfg){
        cfg_int("REKARINA_HUD_HEALTH_X"), cfg_int("REKARINA_HUD_HEALTH_Y"), cfg_int("REKARINA_HUD_HEALTH_SCALE")
    };
    cfg[WIDE_HUD_GROUP_ITEMS] = (WideHudGroupCfg){
        cfg_int("REKARINA_HUD_BUTTONS_X"), cfg_int("REKARINA_HUD_BUTTONS_Y"), cfg_int("REKARINA_HUD_BUTTONS_SCALE")
    };
    cfg[WIDE_HUD_GROUP_ACTION] = cfg[WIDE_HUD_GROUP_ITEMS];
    cfg[WIDE_HUD_GROUP_MINIMAP] = (WideHudGroupCfg){
        cfg_int("REKARINA_HUD_MINIMAP_X"), cfg_int("REKARINA_HUD_MINIMAP_Y"), cfg_int("REKARINA_HUD_MINIMAP_SCALE")
    };

    int custom_mode = (mode == WIDE_HUD_CUSTOM);
    int any_custom = 0;
    if (custom_mode) {
        for (int g = 0; g < WIDE_HUD_GROUP_COUNT; g++)
            if (!group_is_default(cfg[g])) { any_custom = 1; break; }
    }
    float ex = (mode != WIDE_HUD_CENTER) ? wide_ui_edge_shift(ratio, max_ratio) : 0.0f;
    float wx = 1.0f + ex / WIDE_CANVAS_HALF_X;
    int active = (ex != 0.0f) || any_custom;

    int mode_changed = (mode != last_mode) || (ratio != last_ratio) || (any_custom != last_any_custom);
    int sliders_changed = memcmp(cfg, last_cfg, sizeof cfg) != 0;

    plat_mutex_lock(&g_lock);
    if (mode_changed || sliders_changed) {
        memcpy(last_cfg, cfg, sizeof cfg);
        memcpy(g_group_cfg, cfg, sizeof cfg);
        g_force_frames = 4;
    }
    if (mode_changed) {
        last_mode = mode;
        last_any_custom = any_custom;
        last_ratio = ratio;

        g_ex = ex;
        g_wx = wx;
        g_half_x = (ratio > WIDE_GAME_ASPECT * 1.001f) ? WIDE_CANVAS_HALF_X * (ratio / WIDE_GAME_ASPECT)
                                                       : WIDE_CANVAS_HALF_X;
        g_half_y = WIDE_CANVAS_HALF_Y;

        for (int g = 0; g < WIDE_HUD_GROUP_COUNT; g++)
            g_group_xf[g] = (WideHudTransform){1.0f, 0.0f, 0.0f};
        for (int w = 0; w < PANE_CAP / 64; w++) {
            for (uint64_t bits = g_used_bits[w]; bits; bits &= bits - 1) {
                int slot = w * 64 + __builtin_ctzll(bits);
                g_nodes[slot].plan_dx = 0.0f;
                g_nodes[slot].plan_wide = 1.0f;
                g_nodes[slot].in_hud = 0;
                g_nodes[slot].as_menu = 0;
            }
        }
        __atomic_store_n(&g_custom, any_custom, __ATOMIC_RELEASE);
        __atomic_store_n(&g_active, active, __ATOMIC_RELEASE);
        rt_log("wide ui: mode=%s aspect=%.4f edge_shift=%.1f custom=%d",
               mode == WIDE_HUD_EDGES ? "edges" : mode == WIDE_HUD_CENTER ? "center" : "custom",
               (double)ratio, (double)ex, any_custom);
    }

    if (active) run_planner_locked(any_custom);

    if (g_force_frames > 0) {
        write_force_mask(0x40u);
        g_force_frames--;
    } else {
        write_force_mask(0u);
    }
    plat_mutex_unlock(&g_lock);
}

__attribute__((constructor)) static void wide_ui_register(void) {
    rt_register_host(HOST_WIDE_UI_SIZE, cb_size);
    rt_register_host(HOST_WIDE_UI_PLACE, cb_placed);
    rt_register_host(HOST_WIDE_UI_PAR1, cb_parent_set);
    rt_register_host(HOST_WIDE_UI_PAR0, cb_parent_none);
}

#else

int wide_ui_available(void) { return 0; }
void wide_ui_init(void) {}
void wide_ui_tick(void) {}

#endif
#endif   // WIDE_UI_STANDALONE_TEST
