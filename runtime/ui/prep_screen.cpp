// The entry step's progress picture (prep.h): what the window shows while the game prepares its shaders and
// pipelines before play (boot.c prep_wait, gfx_present.c gfx_present_prep_frame). Drawn by Dear ImGui in the menu's
// font through ui/menu.cpp (menu_overlay_frame while prep_screen_active), in the look of the game's own Settings screen
// as the menu and the setup use it (design/rekarina-design-board.html): the brown band across the top, one khaki row
// with the step's name and an inset value box whose amber segment grows with the count, and the description box under
// it. Flat fills; the only gradients are the band's and the amber segment's two tones, as on the board.
#include <stdio.h>

#include "imgui.h"
#include "ui/prep_screen.h"

extern "C" {
#include "prep.h"
#include "ui_text.h"
}

static ImU32 rgb(unsigned v, int a = 255) { return IM_COL32(v >> 16 & 255, v >> 8 & 255, v & 255, a); }

void prep_screen_draw(float w, float h, float scale) {
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    ImFont *font = ImGui::GetFont();
    const float fs = ImGui::GetFontSize(), em = 26.0f * scale;
    int64_t done = 0, total = 0;
    double secs = 0;
    int phase = prep_progress(&done, &total, &secs);
    // the remaining time from the count's rate over the last RATE_S seconds of the steady part (PREP_PHASE_PIPELINES):
    // the mesh jobs come first and are few and long, and a rate taken over them would say ten times too much
    enum { RATE_N = 64 };
    const double RATE_S = 10.0;
    static double st[RATE_N];
    static int64_t sd[RATE_N];
    static int sn, sh;
    char left[96] = "";
    if (phase == PREP_PHASE_PIPELINES) {
        if (!sn || secs - st[(sh + RATE_N - 1) % RATE_N] >= 0.5) {
            st[sh] = secs, sd[sh] = done, sh = (sh + 1) % RATE_N;
            if (sn < RATE_N) sn++;
        }
        int o = (sh + RATE_N - sn) % RATE_N;   // the oldest sample within RATE_S
        while (sn > 1 && secs - st[o] > RATE_S) o = (o + 1) % RATE_N, sn--;
        double dt = secs - st[o];
        int64_t dd = done - sd[o];
        if (dt >= 5.0 && dd > 0) {
            double rest = (double)(total - done) * dt / (double)dd;
            if (rest >= 60) snprintf(left, sizeof left, menu_text(UI_PREP_LEFT_MIN), (int)(rest / 60 + 0.5));
            else if (rest >= 5) snprintf(left, sizeof left, "%s", menu_text(UI_PREP_LEFT_LT1));
        }
    }
    float frac = total > 0 ? (float)done / (float)total : 0.0f;
    if (frac > 1) frac = 1;

    // the band: the port's name at the left, as the setup's window has it
    float band = h * 0.072f;
    dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(w, band), rgb(0x6d583a), rgb(0x6d583a), rgb(0x57452f),
                                rgb(0x57452f));
    dl->AddRectFilled(ImVec2(0, band), ImVec2(w, band + 0.16f * em), rgb(0x1a110a));
    dl->AddText(font, fs * 1.08f, ImVec2(1.6f * em, (band - fs * 1.08f) * 0.5f), rgb(0xf6efd6), "Rekarina");

    // the row: name at the left, the value box (36 % of the row) with the amber segment and the count
    float rw = w * 0.62f < 46 * em ? w * 0.62f : 46 * em, rh = 2.45f * em;
    float x0 = (w - rw) * 0.5f, y0 = h * 0.5f - rh;
    float bw = 0.09f * em;
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + rw, y0 + rh), rgb(0x8c7648));
    dl->AddRectFilled(ImVec2(x0 + bw, y0 + bw), ImVec2(x0 + rw - bw, y0 + rh - bw), rgb(0x6f5a35));
    const char *title = menu_text(UI_PREP_TITLE);
    dl->AddText(font, fs * 1.08f, ImVec2(x0 + 1.1f * em, y0 + (rh - fs * 1.08f) * 0.5f), rgb(0xf6efd6), title);
    float vx0 = x0 + rw * 0.64f, vy0 = y0 + bw, vx1 = x0 + rw - bw, vy1 = y0 + rh - bw;
    dl->AddRectFilled(ImVec2(vx0, vy0), ImVec2(vx1, vy1), rgb(0x1e1a12));
    float pad = 0.22f * em, ix0 = vx0 + pad, ix1 = vx1 - pad;
    if (frac > 0)
        dl->AddRectFilledMultiColor(ImVec2(ix0, vy0 + pad), ImVec2(ix0 + (ix1 - ix0) * frac, vy1 - pad), rgb(0xd9951f),
                                    rgb(0xd9951f), rgb(0xc4821b), rgb(0xc4821b));
    char a[24], b[24], count[96];
    snprintf(a, sizeof a, "%lld", (long long)done);
    snprintf(b, sizeof b, "%lld", (long long)total);
    snprintf(count, sizeof count, menu_text(UI_PREP_COUNT), a, b);
    ImVec2 cs = font->CalcTextSizeA(fs, 1e9f, 0, count);   // on the segment, at its left, as the setup's running step
    dl->AddText(font, fs, ImVec2(ix0 + 0.9f * em, (vy0 + vy1 - cs.y) * 0.5f), rgb(0xf6efd6), count);

    // the description box: why the wait, how often; the seconds so far at the right
    float dy0 = y0 + rh + 0.8f * em, dpad = 1.2f * em;
    char sec[32] = "";
    if (secs >= 1) snprintf(sec, sizeof sec, menu_text(UI_PREP_SECONDS), (int)secs);
    ImVec2 ss = font->CalcTextSizeA(fs, 1e9f, 0, sec), ls = font->CalcTextSizeA(fs, 1e9f, 0, left);
    if (ls.x > ss.x) ss.x = ls.x;
    float wrap = rw - 2 * dpad - ss.x - 1.5f * em;
    const char *note = menu_text(UI_PREP_NOTE);
    ImVec2 ns = font->CalcTextSizeA(fs, 1e9f, wrap, note);
    float dh = (ns.y > 2.4f * fs ? ns.y : 2.4f * fs) + 1.6f * em;   // room for the seconds and the time left
    dl->AddRectFilled(ImVec2(x0, dy0), ImVec2(x0 + rw, dy0 + dh), rgb(0x312318));
    dl->AddText(font, fs, ImVec2(x0 + dpad, dy0 + 0.8f * em), rgb(0xf6efd6), note, NULL, wrap);
    ImVec2 s1 = font->CalcTextSizeA(fs, 1e9f, 0, sec);
    dl->AddText(font, fs, ImVec2(x0 + rw - dpad - s1.x, dy0 + 0.8f * em), rgb(0xffd27a), sec);
    if (left[0]) dl->AddText(font, fs, ImVec2(x0 + rw - dpad - ls.x, dy0 + 0.8f * em + 1.4f * fs), rgb(0xffd27a), left);
}
