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
// The step's parts, for its time left: the recorded pipelines (prep_progress_total/step count here), then the prepared
// variants' stage libraries the driver does not have yet (vk_pipeline.c lb_list_load), and beside both the textures the
// texture cache does not hold yet (tex_prepare.c tex_prepare_entry_start).
enum { PREP_PART_PIPES, PREP_PART_LIBS, PREP_PART_TEX, PREP_PARTS };
void prep_part_total(int part, int64_t n, double unit_s);   // unit_s: seconds an item takes before its pace is measured
void prep_part_step(int part, int64_t n);
void prep_part_work(int part, int64_t total, double unit_s);   // the part's pace taken on this work, not its count
void prep_part_step_work(int part, int64_t n, int64_t work);
double prep_progress_left(void);                     // seconds the step has left (-1: not known yet)
void prep_part(int part, int64_t *done, int64_t *total);   // a part's count
// This exe in another mode (--scan-textures) as a child, its output line by line to on_line; 0 when it
// did not start. Returns when the child has ended.
int prep_child(const char *mode, void (*on_line)(const char *line, void *ctx), void *ctx);
void prep_child_paths(void);   // the game's exefs, romfs and profile paths into the environment its children read
void prep_progress_begin(void);                      // vk_pipeline_prewarm starts
void prep_progress_end(void);                        // and ends
int prep_screen_active(void);                        // 1 while the progress picture is shown (ui/menu.cpp)
void prep_screen_set(int on);                        // boot.c, around the frames it presents

#ifdef __cplusplus
}
#endif
