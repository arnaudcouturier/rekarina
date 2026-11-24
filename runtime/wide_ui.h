// Ultrawide HUD anchoring and custom HUD placement for OoT 1.1.1 (runtime/wide_ui.c).
//
// Keeps gameplay HUD elements anchored to the screen's edges on ultrawide displays while preserving 16:9 proportions
// (working on top of widescreen.c's wide_hud horizontal squeeze), and lets the player move or scale the three main HUD
// groups live from the F1 menu (Hearts & magic, Action & item buttons, Minimap).
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// HUD layout mode (REKARINA_HUD_LAYOUT).
enum {
    WIDE_HUD_EDGES  = 0,   // anchor gameplay HUD to the screen's edges on ultrawide
    WIDE_HUD_CENTER = 1,   // keep gameplay HUD in the middle 16:9
    WIDE_HUD_CUSTOM = 2,   // start from screen edges and apply per-group offsets/scales
};

// Custom HUD placement groups (items and action prompts share the "Action and item buttons" sliders).
enum {
    WIDE_HUD_GROUP_HEALTH  = 0,
    WIDE_HUD_GROUP_ITEMS   = 1,
    WIDE_HUD_GROUP_ACTION  = 2,
    WIDE_HUD_GROUP_MINIMAP = 3,
    WIDE_HUD_GROUP_COUNT   = 4,
};

// Pane types reported by the game's 2D layout flags ((flags >> 22) & 0xf).
enum {
    WIDE_PANE_NULL    = 0,
    WIDE_PANE_SCISSOR = 4,
    WIDE_PANE_CAPTURE = 5,
    WIDE_PANE_PART    = 6,
    WIDE_PANE_HOST    = 7,
};

// Per-pane layout rules assigned from layout + pane names.
enum {
    WIDE_RULE_NONE = 0,
    WIDE_RULE_KEEP,          // leave position and size untouched
    WIDE_RULE_DESCEND,       // keep container at its origin, still plan its children
    WIDE_RULE_WIDEN,         // stretch centered pane to full ultrawide width
    WIDE_RULE_ANCHOR_LEFT,   // anchor directly to the left edge
    WIDE_RULE_ANCHOR_RIGHT,  // anchor directly to the right edge
    WIDE_RULE_SLIDE,         // sliding pane: evaluate extents at its parent's rest X
    WIDE_RULE_MAP_ORIGIN,    // MapMenu_00 icon anchor: at x ~= 0 marks the pause-menu map page
};

// Affine 2D transform applied to a custom HUD group in canvas coordinates (3840x2160, origin at center).
typedef struct {
    float scale;
    float tx;
    float ty;
} WideHudTransform;

// User-configured offset (% of picture width/height, -15..15) and scale (%, 75..125) for one group.
typedef struct {
    int x_pct;
    int y_pct;
    int scale_pct;
} WideHudGroupCfg;

// Snapshot of one live pane fed into the pure tree planner (no guest memory pointers needed).
typedef struct {
    int32_t parent;       // index into input array, or -1 for root
    uint64_t layout_id;   // opaque layout instance identity
    float ox, x0, x1;     // unshifted origin X and horizontal bounds in 3840x2160 canvas units
    float parent_ox;      // parent's unshifted origin X when placed (NaN if unknown)
    uint8_t type;         // WIDE_PANE_*
    uint8_t rule;         // WIDE_RULE_*
    uint8_t is_hud;       // root layout belongs to gameplay HUD
    uint8_t is_world;     // world-projected HUD layout (never moved or widened)
    uint8_t is_map;       // MapMenu_00 (shared between gameplay minimap and pause map page)
    uint8_t is_minimap;   // MapMenu_00 live-minimap anchor pane (N_InOut_00 / Pa_KeySmall_00)
} WidePaneInput;

// Planned output for one pane.
typedef struct {
    float dx;         // horizontal shift in canvas units (0 = unchanged)
    float wide;       // width multiplier (1.0f = unchanged)
    uint8_t in_hud;   // 1 if pane belongs to an active gameplay HUD tree
    uint8_t as_menu;  // 1 if MapMenu_00 root is currently showing the pause-menu map page
} WidePanePlan;

// Runtime lifecycle (called from widescreen.c, nvn_api_obj.c, ui/menu.cpp).
int wide_ui_available(void);
void wide_ui_init(void);
void wide_ui_tick(void);

// Pure planning helpers (also exercised directly by tests/wide_ui_check.c).
float wide_ui_reach_ratio(float aspect_ratio, float max_ratio);
float wide_ui_edge_shift(float aspect_ratio, float max_ratio);
WideHudTransform wide_ui_group_transform(WideHudGroupCfg cfg, float lo_x, float hi_x, float lo_y, float hi_y,
                                         float half_x, float half_y);
void wide_ui_plan_panes(const WidePaneInput *in, WidePanePlan *out, int count, float ex, float wx);

#ifdef __cplusplus
}
#endif
