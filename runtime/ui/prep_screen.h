// The entry step's progress picture (ui/prep_screen.cpp), laid out inside an ImGui frame of ui/menu.cpp.
#pragma once

// w, h: the swapchain image in pixels; scale: the menu's scale for that height (26 px of font per 1080 lines).
void prep_screen_draw(float w, float h, float scale);
