// Unit checks for the ultrawide HUD pane planner and custom group transform (runtime/wide_ui.c).
// Build: clang -O2 -Wall -Wextra -DWIDE_UI_STANDALONE_TEST -Iruntime tests/wide_ui_check.c runtime/wide_ui.c -lm -o wide_ui_check.exe
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "wide_ui.h"

static int g_fails;

static void check_near(const char *label, float got, float want, float tol) {
    if (!(fabsf(got - want) <= tol)) {
        fprintf(stderr, "FAIL %s: got %.4f, want %.4f (tol %.4f)\n", label, (double)got, (double)want, (double)tol);
        g_fails++;
    }
}

static void check_eq_int(const char *label, int got, int want) {
    if (got != want) {
        fprintf(stderr, "FAIL %s: got %d, want %d\n", label, got, want);
        g_fails++;
    }
}

static void test_edge_shift(void) {
    const float def_max = 12.0f / 5.0f;
    // 16:9 stays 0
    check_near("16:9 shift", wide_ui_edge_shift(16.0f / 9.0f, def_max), 0.0f, 0.01f);
    // 21:9 (3440x1440): reach = 3440/1440 = 2.38889 -> ex = 1920 * (2.38889 / 1.77778 - 1) = 660.0
    check_near("3440x1440 shift", wide_ui_edge_shift(3440.0f / 1440.0f, def_max), 660.0f, 0.1f);
    // 32:9 capped at 12:5 by default -> ex = 1920 * (2.4 / (16/9) - 1) = 672.0
    check_near("32:9 capped shift", wide_ui_edge_shift(32.0f / 9.0f, def_max), 672.0f, 0.1f);
    // 32:9 uncapped (max_ratio = 0) -> full double width -> ex = 1920.0
    check_near("32:9 uncapped shift", wide_ui_edge_shift(32.0f / 9.0f, 0.0f), 1920.0f, 0.1f);
    // 48:9 triple-monitor surround with default cap keeps HUD on center 16:9
    check_near("48:9 surround center", wide_ui_edge_shift(48.0f / 9.0f, def_max), 0.0f, 0.01f);
}

static void test_custom_transform(void) {
    // Identity when default (0, 0, 100%)
    WideHudTransform id = wide_ui_group_transform((WideHudGroupCfg){0, 0, 100}, -1800, -1200, 700, 1000, 2580, 1080);
    check_near("id scale", id.scale, 1.0f, 1e-5f);
    check_near("id tx", id.tx, 0.0f, 1e-5f);
    check_near("id ty", id.ty, 0.0f, 1e-5f);

    // Move right by +10% of 21:9 half-width (half_x = 2580 -> full width 5160 -> +516 units)
    WideHudTransform mv = wide_ui_group_transform((WideHudGroupCfg){10, -5, 100}, -2400, -1800, 700, 1000, 2580, 1080);
    check_near("mv scale", mv.scale, 1.0f, 1e-5f);
    check_near("mv tx", mv.tx, 516.0f, 0.5f);
    check_near("mv ty", mv.ty, -108.0f, 0.5f);

    // Pushing far off the left edge must clamp inside -0.99 * half_x
    WideHudTransform cl = wide_ui_group_transform((WideHudGroupCfg){-15, 15, 125}, -2400, -1800, 700, 1000, 2580, 1080);
    float new_lo_x = -2400.0f * cl.scale + cl.tx;
    float new_hi_y = 1000.0f * cl.scale + cl.ty;
    check_near("clamped left edge", new_lo_x, -2580.0f * 0.99f, 1.0f);
    check_near("clamped top edge", new_hi_y, 1080.0f * 0.99f, 1.0f);
}

static void test_hud_tree_and_map_page(void) {
    const float ex = 660.0f;
    const float wx = 1.0f + ex / 1920.0f;

    // Tree:
    // 0: PlayerStatus_00 root (HUD)
    // 1:   N_ScaleL_00 (DESCEND container at 0)
    // 2:     Pa_HeartGauge_00 (ANCHOR_LEFT)
    // 3:   Pa_SelectItem_00 (right-side button cluster near +1700)
    // 4:   Scissor clipping a right-side rupee counter (5)
    // 5:     Rupee box at [1600, 1850]
    // 6: Location_00 root (HUD)
    // 7:   Full-width backdrop [-1920, +1920] (widens to wx)
    // 8: MapMenu_00 root #1 (live minimap: N_MapIcon_00 at +1536)
    // 9:   N_InOut_00 (minimap group)
    // 10:  N_MapIcon_00 (MAP_ORIGIN at +1536)
    // 11: MapMenu_00 root #2 (pause map page: N_MapIcon_00 at 0 -> as_menu=1, must NOT move!)
    // 12:  N_InOut_00 (minimap group inside pause map page)
    // 13:  N_MapIcon_00 (MAP_ORIGIN at 0)
    // 14: PlayerStatus3D_00 root (world-projected HUD: must NOT move!)
    // 15:  World gauge at +1700
    WidePaneInput in[16] = {
        {.parent = -1, .layout_id = 100, .ox = 0, .x0 = 0, .x1 = 0, .parent_ox = NAN, .type = WIDE_PANE_NULL, .is_hud = 1},
        {.parent = 0, .layout_id = 100, .ox = 0, .x0 = -1800, .x1 = -1200, .parent_ox = 0, .type = WIDE_PANE_NULL, .rule = WIDE_RULE_DESCEND, .is_hud = 1},
        {.parent = 1, .layout_id = 100, .ox = -1500, .x0 = -1800, .x1 = -1200, .parent_ox = 0, .type = 1, .rule = WIDE_RULE_ANCHOR_LEFT, .is_hud = 1},
        {.parent = 0, .layout_id = 100, .ox = 1650, .x0 = 1400, .x1 = 1860, .parent_ox = 0, .type = 1, .is_hud = 1},
        {.parent = 0, .layout_id = 100, .ox = 0, .x0 = -1920, .x1 = 1920, .parent_ox = 0, .type = WIDE_PANE_SCISSOR, .is_hud = 1},
        {.parent = 4, .layout_id = 100, .ox = 1720, .x0 = 1600, .x1 = 1850, .parent_ox = 0, .type = 1, .is_hud = 1},

        {.parent = -1, .layout_id = 200, .ox = 0, .x0 = 0, .x1 = 0, .parent_ox = NAN, .type = WIDE_PANE_NULL, .is_hud = 1},
        {.parent = 6, .layout_id = 200, .ox = 0, .x0 = -1920, .x1 = 1920, .parent_ox = 0, .type = 1, .is_hud = 1},

        {.parent = -1, .layout_id = 300, .ox = 0, .x0 = 0, .x1 = 0, .parent_ox = NAN, .type = WIDE_PANE_NULL, .is_hud = 1, .is_map = 1},
        {.parent = 8, .layout_id = 300, .ox = 1536, .x0 = 1200, .x1 = 1860, .parent_ox = 0, .type = WIDE_PANE_NULL, .is_hud = 1, .is_map = 1, .is_minimap = 1},
        {.parent = 8, .layout_id = 300, .ox = 1536, .x0 = 1500, .x1 = 1572, .parent_ox = 0, .type = 1, .rule = WIDE_RULE_MAP_ORIGIN, .is_hud = 1, .is_map = 1},

        {.parent = -1, .layout_id = 400, .ox = 0, .x0 = 0, .x1 = 0, .parent_ox = NAN, .type = WIDE_PANE_NULL, .is_hud = 1, .is_map = 1},
        {.parent = 11, .layout_id = 400, .ox = 1536, .x0 = 1200, .x1 = 1860, .parent_ox = 0, .type = WIDE_PANE_NULL, .is_hud = 1, .is_map = 1, .is_minimap = 1},
        {.parent = 11, .layout_id = 400, .ox = 0, .x0 = -50, .x1 = 50, .parent_ox = 0, .type = 1, .rule = WIDE_RULE_MAP_ORIGIN, .is_hud = 1, .is_map = 1},

        {.parent = -1, .layout_id = 500, .ox = 0, .x0 = 0, .x1 = 0, .parent_ox = NAN, .type = WIDE_PANE_NULL, .is_hud = 1, .is_world = 1},
        {.parent = 14, .layout_id = 500, .ox = 1700, .x0 = 1600, .x1 = 1800, .parent_ox = 0, .type = 1, .is_hud = 1, .is_world = 1},
    };
    WidePanePlan out[16];
    wide_ui_plan_panes(in, out, 16, ex, wx);

    check_near("N_ScaleL_00 stays at 0", out[1].dx, 0.0f, 0.01f);
    check_near("Pa_HeartGauge_00 moves left", out[2].dx, -ex, 0.01f);
    check_near("Pa_SelectItem_00 moves right", out[3].dx, ex, 0.01f);
    check_near("Scissor moves right with child", out[4].dx, ex, 0.01f);
    check_near("Location backdrop widens", out[7].wide, wx, 0.001f);

    check_eq_int("live minimap as_menu=0", out[8].as_menu, 0);
    check_near("live minimap N_InOut_00 right", out[9].dx, ex, 0.01f);
    check_near("live minimap N_MapIcon_00 right", out[10].dx, ex, 0.01f);

    check_eq_int("pause map page as_menu=1", out[11].as_menu, 1);
    check_eq_int("pause map page in_hud=0", out[12].in_hud, 0);
    check_near("pause map page N_InOut_00 stays", out[12].dx, 0.0f, 0.01f);
    check_near("pause map page N_MapIcon_00 stays", out[13].dx, 0.0f, 0.01f);

    check_near("world-projected HUD stays", out[15].dx, 0.0f, 0.01f);

    // Center mode (ex = 0): nothing moves or widens
    wide_ui_plan_panes(in, out, 16, 0.0f, 1.0f);
    for (int i = 0; i < 16; i++) {
        check_near("center mode dx", out[i].dx, 0.0f, 0.01f);
        check_near("center mode wide", out[i].wide, 1.0f, 0.001f);
    }
}

int main(void) {
    test_edge_shift();
    test_custom_transform();
    test_hud_tree_and_map_page();
    if (g_fails) {
        fprintf(stderr, "wide_ui_check: %d failure(s)\n", g_fails);
        return 1;
    }
    printf("wide_ui_check: all checks passed\n");
    return 0;
}
