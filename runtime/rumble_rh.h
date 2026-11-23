// RenHDrum: the controller haptics library (renhdrum.dll beside the game; tools/build_renhdrum.py builds it) that plays
// the game's HD rumble on a DualSense's voice coils, an Xbox One or Series pad's four motors or a Switch Pro
// Controller's HD rumble, and on a DualSense its extras (trigger kickback, the speaker's taps). input.c asks for it while
// the Vibration setting is Auto; without the library, or for a pad it does not play, the pad keeps input.c's own
// rumble. rumble_rh.c says what each backend does.
#ifndef RUMBLE_RH_H
#define RUMBLE_RH_H

#include <stdint.h>

typedef struct SDL_Gamepad SDL_Gamepad;

enum { RH_NONE, RH_DUALSENSE_USB, RH_DUALSENSE_BT, RH_XBOX, RH_SWITCH };

// 1 when renhdrum.dll is beside the game and usable (its version and every function this file binds). The first call
// loads it and logs the outcome, or why not. Any thread.
int rh_ready(void);
// The backend that plays this gamepad, opened now (RH_NONE: none; the pad keeps its own rumble), percent the Vibration
// strength. The game's last values go to it at once. Gamepad thread; one gamepad at a time (rh_detach first).
int rh_attach(SDL_Gamepad *pad, int percent);
// Closes the backend (its sound stream, its report thread, its state). pad: the gamepad when it is still connected, so
// a DualSense's triggers and speaker are given back first; NULL when it went away. Gamepad thread. The caller stops
// the pad's motors.
void rh_detach(SDL_Gamepad *pad);
// The backend in use (RH_NONE when none). Any thread.
int rh_kind(void);
const char *rh_kind_name(int kind);
// One side (0 left, 1 right) of the game's vibration as the game sent it: amplitudes 0..1, frequencies in Hz. Any thread.
void rh_feed(int side, float amp_low, float freq_low, float amp_high, float freq_high);
// Every gamepad-thread poll while a backend is open: sides the game stopped feeding go silent, the Xbox motors and the
// Switch packets RenHDrum has ready are sent. percent: the Vibration strength (0..300); triggers: an Xbox pad's trigger
// motors may run (REKARINA_RUMBLE_TRIGGERS). Returns 0 when the backend failed: the caller closes it (rh_detach) and the
// pad gets its own rumble.
int rh_tick(SDL_Gamepad *pad, uint64_t now_ns, int percent, int triggers);
// A DualSense's extras (REKARINA_DUALSENSE_EXTRAS), every gamepad-thread poll while RenHDrum plays a DualSense: on, the
// triggers kick back with the vibration and the speaker taps; off, the voice coils alone (and the triggers released).
// depth: each trigger's travel 0..255 (left, right); other: another button is held (RenHDrum mutes a kick that starts
// while the player acts). Nothing for other backends.
void rh_extras(SDL_Gamepad *pad, uint64_t now_ns, int on, const int depth[2], int other);

#endif
