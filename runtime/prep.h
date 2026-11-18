// What the game prepares before play, and what play still had to build (runtime/prep.c).
//
// The entry step (boot.c rt_boot, vk_pipeline.c vk_pipeline_prewarm) builds every shader and pipeline the player's key
// files and the release's seeds name before the guest starts; prep_progress_* count its work for the progress picture
// (gfx_present.c gfx_present_prep_frame, ui/prep_screen.cpp). Everything translated or built after the guest started is
// a stall in play: prep_note puts it in a ledger by kind and by map, and prep_report logs one summary line and a table
// per map at exit ("prep: in play ...").
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    PREP_XLATE,      // a shader translated
    PREP_GFX,        // a graphics pipeline built
    PREP_LIB,        // a stage library built for a draw (graphics pipeline library)
    PREP_MESH,       // a mesh pipeline built
    PREP_GEOM,       // a geometry pipeline built
    PREP_CS,         // a compute pipeline built
    PREP_KINDS
};

void prep_play_begin(void);                          // boot.c: the guest starts; from here on a build is "in play"
int prep_in_play(void);                              // 1 after prep_play_begin
// Something was built or translated in play: wait_ms what a draw, dispatch or thread waited for it (0: nobody waited,
// its draws were left out meanwhile), build_ms how long the build took (0: unknown).
void prep_note(int kind, double wait_ms, double build_ms);
void prep_map_open(const char *path);                // romfs.c: a file the game opened; a map file names the map
const char *prep_map_now(void);                      // the current map's name ("-" before the first map)
uint64_t prep_map_frame(void);                       // the frame of the last map change (0: none yet)
void prep_report(void);                              // the ledger into the log (exit paths)

// ---- the entry step's progress (any thread)
void prep_progress_total(int64_t n);                 // more work known
void prep_progress_step(int64_t n);                  // work done
void prep_progress_phase(int phase);                 // PREP_PHASE_*
void prep_progress_build(double ms);                 // one build of the step returned after ms
double prep_progress_longest(void);                  // the longest build of the step so far, in ms
int prep_progress(int64_t *done, int64_t *total, double *seconds);   // the phase (0: no entry step running)
// MESH: the builders are on the mesh and geometry jobs (a few, up to 22 s each cold); PIPELINES: on the rest, at a
// steady rate the picture's remaining time is taken from
enum { PREP_PHASE_NONE, PREP_PHASE_SHADERS, PREP_PHASE_MESH, PREP_PHASE_PIPELINES, PREP_PHASE_DONE };
void prep_progress_begin(void);                      // vk_pipeline_prewarm starts
void prep_progress_end(void);                        // and ends
int prep_screen_active(void);                        // 1 while the progress picture is shown (ui/menu.cpp)
void prep_screen_set(int on);                        // boot.c, around the frames it presents

#ifdef __cplusplus
}
#endif
