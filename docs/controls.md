# Controls

Input comes from the first SDL3 gamepad, the keyboard and the mouse, merged into one Switch controller (`runtime/input_win.c`). Buttons are ORed; each stick takes the source with the larger deflection. Keys and mouse buttons count only while the game window is in the foreground. `REKARINA_INPUT=none` disables both (automated runs). `REKARINA_*` settings can also be set in `rekarina.ini` next to `Rekarina.exe` (`input = none`, `motion = 0`, `rumble = 0`); the environment overrides the file. `REKARINA_RUMBLE_STRENGTH` is the rumble strength in percent (0 to 300, default 100; 0 is the same as `rumble = 0`). `REKARINA_RUMBLE_CURVE` is the exponent applied to the amplitude before the strength (0.2 to 1.5, default 0.6; lower makes weak pulses stronger, 1 is linear). `REKARINA_RUMBLE_TRIGGERS=1` also drives the impulse triggers of an Xbox pad.

The game sends HD rumble: for the left and the right side, a low and a high band, each an amplitude at a frequency (the log shows the first value, `hid: first non-zero vibration from the game: left 0.01@95 Hz 0.01@236 Hz`). Most pads have two plain motors: each side's bands are split between the heavy and the light motor by frequency (all heavy at 160 Hz and below, all light at 320 Hz and up), the stronger side drives each motor, and SDL_RumbleGamepad plays the result (`runtime/ov_input.c`, `rumble_update`). A Switch Pro Controller or a Joy-Con that SDL opens itself (SDL_GetGamepadType, and Nintendo's USB ids of the Switch's own controllers, which clones in Switch mode copy) plays both sides as the game sent them, frequencies included: the gamepad thread sends Switch rumble output packets through SDL_SendGamepadEffect (`runtime/input.c`, `hd_send`; the four-byte encoding per side follows the public notes on the Switch controllers' rumble data, `input_hd_rumble_encode`, checked by `--probe-cfg`). The curve and the strength apply to those amplitudes too (100 % and curve 1 leave the game's own), capped at amplitude 1.0. A packet has no duration of its own, so the pad thread sends the neutral packet 250 ms after the last request, as the two-motor path stops when the game stops streaming. A pad whose driver refuses the packet gets the two-motor rumble from then on; the log says `input: "..." does not take HD rumble packets`, and `input: HD rumble: ...` once the first packet went through. The Switch 2 controllers have no gamepad type of their own, but SDL drives them with another driver (its `SDL_JOYSTICK_HIDAPI_SWITCH2` one), not the one this packet is checked against: their USB ids keep them on the two-motor path. Untested on hardware: a Joy-Con pair that SDL combines into one gamepad (its combined driver may refuse the packet, which falls back as above). A program that stands in for the controller (Steam Input, DS4Windows, BetterJoy) shows SDL an Xbox-type pad: no gyro and no HD rumble reach the game then, and the log says `input: an Xbox-type (XInput) gamepad: no gyro reaches the game`.

`REKARINA_VIBRATION` (`vibration`, live; *Vibration*: Auto, Basic, Off) picks how that plays. `auto` (the default) plays the game's bands through RenHDrum ([github.com/trzz1214/RenHDrum](https://github.com/trzz1214/RenHDrum), 0.5.0, MIT or Apache-2.0) where it plays the pad, and as described above where it does not or when its library is missing; `basic` gives every pad its two motors (a Switch pad too); `off` sends nothing (the game still has its vibration devices). RenHDrum is `renhdrum.dll` beside the game, built from its source with a pinned Rust by `tools/build_renhdrum.py` and carried by the release in `bin/` (the setup's components step puts it beside the game); `runtime/rumble_rh.c` loads it by its full path and binds the functions it uses, and a missing file, a missing function or another 0.x version leaves every pad on its own rumble with the reason in the log (`vibration: RenHDrum: ...`). Its backends, each fed every value the game sends (`ov_input.c` `play`, no curve; the strength applies): a DualSense on its cable plays through its voice coils, the third and fourth channel of its own four-channel sound card (an SDL audio stream on that card, 48 kHz, which RenHDrum's DSP fills; tried again every 3 s, five times, as Windows may list the card after the controller). Windows' volume for that card scales the stream like any other (shared mode): at 0 % or muted the coils get silence, and Windows sets that card's volume and may make the pad the default output when it is plugged in. The log reads it once the card is open, without changing it (`vibration: the DualSense's volume in the system is N %`, `plat_audio_out_volume`); a DualSense over Bluetooth gets RenHDrum's haptics report 0x32 every 10.67 ms through SDL's hidapi beside SDL's own reports, while something plays and for 2 s after; an Xbox One or Series pad gets RenHDrum's four motor values (grips through SDL_RumbleGamepad, triggers through SDL_RumbleGamepadTriggers only with `rumble_triggers = 1`); a Switch Pro Controller or a Joy-Con pair gets RenHDrum's HD rumble packets (its Switch 2 mode) instead of `hd_send`'s. A DualSense's extras (`REKARINA_DUALSENSE_EXTRAS`, `dualsense_extras`, live, default 1; *DualSense extras*, greyed out while the pad is not a DualSense): RenHDrum's trigger model turns each trigger's travel and the vibration into a trigger effect (the kickback, muted when it would start while another button is held) and the DSP's speaker channel plays small taps (60 %, RenHDrum's documented level). On the cable the trigger effects and the block that opens or gives back the speaker go to the pad as DS5 effects blocks through SDL_SendGamepadEffect, and the speaker is the sound card's second channel; over Bluetooth every period is report 0x36 instead of 0x32 (the same haptics, both triggers' effects and the speaker as Opus). Off is the voice coils alone, as 0x32; turning them off, and closing, releases both triggers first (`rumble_rh.c` `rh_extras`). The log names the backend (`vibration: RenHDrum plays "...": ...`). RenHDrum also reads its own settings file (`%APPDATA%\RenHDrum\renhdrum-settings.txt`) and its companion program's shared memory when they exist. A test gamepad (`REKARINA_TEST_PAD`, with `,<lt>,<rt>` after its kind for triggers held at that travel of 255) takes rumble, trigger rumble, effect packets and DS5 effects blocks and logs what arrived (`input: test gamepad got ...`), which checks the Xbox and Switch paths without the pads; `REKARINA_RH_TEST=usb` or `bt` gives a DualSense-type test pad that path with a sink in place of the sound card or the Bluetooth link, which logs the level of the four channels or the reports it would have written (`vibration test: ...`). How it feels needs the pad itself.

`REKARINA_PAD_LAYOUT` (`pad_layout`, the *Button layout* row on the Controls tab, live) picks which face button is Switch A:

- `nintendo`: by position, as on a Switch (the table below).
- `xbox`: south A, east B, west X, north Y, as an Xbox pad labels them; on a PlayStation pad cross is A, circle B, square X, triangle Y.
- `auto` (default): `nintendo` for a pad whose east button SDL reports labelled A (Switch Pro Controller, Nintendo-layout pads), `xbox` for the others.

The log names the layout in effect (`input: gamepad layout ...`); the menu shows it under *Automatic*. Which controller's buttons the game's prompts show is `REKARINA_PROMPTS` (see Xbox button prompts below). 0.1.9's `pad_layout = xbox_prompts` is read as `pad_layout = nintendo` with `prompts = xbox`, and the menu writes it that way.

| Switch | Gamepad (by position, Nintendo layout) | Keyboard and mouse | Action in the world |
|---|---|---|---|
| Left stick | left stick | W A S D | move |
| Right stick | right stick | mouse; arrow keys as a second source | camera |
| Y | west | left mouse button | attack |
| ZL | left trigger (over 50%) | right mouse button, Z | shield, lock on |
| ZR | right trigger (over 50%) | F, X | bow |
| X | north | Space | jump |
| B | south | Left Shift | run, cancel |
| A | east | E | interact, pick up |
| L | left shoulder | Q | abilities |
| R | right shoulder | R | throw weapon |
| Left stick click | left stick click | Left Ctrl | crouch |
| Right stick click | right stick click | C | first-person view |
| D-pad left, up, right, down | D-pad | 1 2 3 4 | quick-select |
| Plus | Start | Tab | menu, inventory |
| Minus | Back | M | map |
| Right stick shaken left and right | shake the right stick | G | detach a fused part from what Ultrahand holds |

Esc is unbound in the game; it opens the settings menu and releases the mouse cursor (see Mouse camera). Z and X duplicate ZL and ZR so menu pages turn with two neighbouring keys; the menu tabs (L, R) are Q and R. The mouse wheel is unbound. The bindings are the tables of `input_map` in `runtime/input_map.c`: the keys (platform-neutral key ids) and one gamepad table per layout (gamepad inputs by position); `runtime/input_win.c` translates Win32 virtual keys and SDL buttons.

## Xbox button prompts

`REKARINA_PROMPTS` (`prompts`, the *Button prompts* row on the Controls tab, read at start) picks the controller whose buttons the game's prompts show: `nintendo` (default) keeps the game's own, `xbox` shows an Xbox controller's, `pc` the keyboard and mouse (PC button prompts, below). `auto` follows the device in use (Automatic button prompts, below; OoT's default).

- L, R, ZL, ZR, Plus and Minus become LB, RB, LT, RT, Menu and View, and the d-pad prompts a cross with an arrow for each direction meant. The letters on the keys are cut out in the game's own text font.
- A, B, X and Y name the label of the button the pad layout presses, so each prompt names the button to press. `pad_layout = nintendo` plays an Xbox pad by position: the right button, labelled B, does Switch A (interact, confirm), and the bottom one, labelled A, does Switch B (run, cancel), so the prompts swap A with B and X with Y. `xbox` and `auto` put Switch A on the button labelled A, and the letters stay.

| Switch | `nintendo` layout | `xbox` or `auto` layout |
|---|---|---|
| A, B, X, Y | B, A, Y, X | A, B, X, Y |
| L, R | LB, RB | LB, RB |
| ZL, ZR | LT, RT | LT, RT |
| Plus, Minus | Menu, View | Menu, View |
| D-pad | cross | cross |

The action prompt (`DoCommand_00`) shows its button in a diamond whose shape gives the face buttons' positions as on a Switch, with the other three letters around it. With `nintendo` its letters are swapped with the rest and every one names the button in that place. With `xbox` and `auto` the letters are right but the diamond keeps the Switch positions: A shows on its right, while the button labelled A is at the bottom. The sticks keep the Switch icons, and so do prompts drawn as fixed images (some tutorial animations).

The prompts are part of the game's fonts, textures and messages, so they follow the layout and not the bindings. A button remapped with a `bind_` line or on the Controls tab keeps the prompt the game draws for it, which then names the wrong button. Use the Xbox prompts with the default bindings.

The game reads its UI archive and fonts once at boot, so the prompts change at the next start, also after `pad_layout` changes between `nintendo` and the others (the menu says *prompts change after restart*); the buttons change at once. Under that note the menu offers *Restart now*: after a confirmation it writes `rekarina.ini`, closes the game as *Quit game* does and starts the game again with the same command line, which waits until the old process is gone before it opens the log and the saves (`runtime/gfx_present.c`, `gfx_window_closed`; `plat_restart` and `plat_restart_wait` in `runtime/platform/win32.c` and `linux.c`). Progress since the last save is lost, as with *Quit game*; while the game writes a save, the confirmation waits. With `prompts = xbox` the game loads `romfs-patches-xbox` (`pad_layout = nintendo`) or `romfs-patches-xbox-label` (`xbox`, `auto`) next to `romfs-index.bin` in place of `romfs-patches` (`runtime/romfs.c`, `overlay_init`; `runtime/input_map.c`, `input_prompts_folder`): the same 60 fps patches, with the prompts drawn from the game's own files by `tools/make_romfs_patches.py --prompts xbox` or `--prompts xbox-label` (`tools/button_icons.py`, after MacNativeGames' `make_button_icons.py`), which the setup runs next to the plain one:

- every prompt but the button guide in play is text in the button font, `scft/External.bfotf` in each `Font/Font*.Nin_NX_NVN.bfarc.zs`: glyphs U+E040 to U+E043 (A B X Y), U+E044 to U+E047 (L R ZL ZR), U+E04B and U+E04C (Plus, Minus), U+E049 and U+E060 to U+E065 (the d-pad). fontTools writes the font again, so the archives grow and the folder also carries the resource size table (`System/Resource/ResourceSizeTable.Product.<n>...rsizetable.zs`) with their entries raised by as much; without that the game stops at start.
- the button guide in play draws L and ZL from the images `Nt_KeyTexL_00` and `Nt_KeyTexZL_00` of the Common archive's `timg/__Combined.bntx`, painted with the same icons. The action prompt draws its button from `Nt_KeyTexA/B/X/Y_00`, whose image data is swapped for `nintendo`, and the diamond's letters are text the game takes from `LayoutMsg/DoCommand_00.msbt` of each language's `Mals` archive (and the layout's own text panes behind it), swapped the same way.

The Switch prompts stay, with a note under *Button prompts* and a `RomFS overlay: ...` line in the log, when the folder is missing (an install made by an older setup, or one where the setup could not prepare it and showed a warning; running the setup again on the install folder writes it), when it fails its check, with a game resolution of another shape than 16:9 (its own UI layout wins; 16:10 and 21:9), and when the `romfs_mods` folder replaces one of its files: `romfs-patches` then loads in its place, where two overlays replacing one file would stop the game. In a source build, `.venv-py314\Scripts\python tools\make_romfs_patches.py --version <ver> --prompts xbox` (and `--prompts xbox-label`) writes `build\<ver>\romfs-patches-xbox` (`-label`); it needs fontTools from `tools/requirements-py314.txt`. `--xbox-prompts` is the older name of `--prompts xbox`.

## PC button prompts

`prompts = pc` (*PC* on the *Button prompts* row) shows the key or mouse button the default keyboard bindings press each Switch button with, whatever the pad layout. The game loads `romfs-patches-pc` in place of `romfs-patches`, written by `tools/make_romfs_patches.py --prompts pc` (`button_icons.py`, `pc_icons`), which the setup runs next to the Xbox ones:

| Switch | Prompt |
|---|---|
| A, B, X, Y | E, Shift, Space, left click |
| L, R, ZL, ZR | Q, R, right click, F |
| Plus, Minus | Tab, M |
| D-pad | 1 (left), 2 (up), 3 (right), 4 (down), as a cross or the keys of the directions meant |
| Left stick, its press | W A S D (or the keys of the directions meant), Ctrl |
| Right stick, its press | the mouse, with an arrow for each direction meant, C |

- The glyphs keep the width of the game's own, so the right-aligned guides do not run into their text. A key is a rounded square with its label cut out in the game's own text fonts; a mouse button is a mouse with that button filled and the others hollow.
- The button guide in play draws every button from images of the Common archive (`Nt_KeyTexA/B/X/Y/L/ZL_00`, `Nt_KeyTexLSt_00` and `_01`, the stick tilted and pressed): all of them are painted with the same icons. The letters of the action prompt's diamond are one character of the Normal font each: E, an arrow for Shift, a bar for Space, L and R for the mouse buttons.
- Like the Xbox prompts they follow the defaults and not the bindings. `--binds <rekarina.ini>` draws the first key of each `bind_` line of that file instead, for a folder made by hand.
- At a game resolution of another shape than 16:9 the game loads `romfs-patches-pc-<w>x<h>` in place of `romfs-patches-<w>x<h>` when it is there: the same prompts with that shape's UI layout, `make_romfs_patches.py --prompts pc --aspect <w>x<h> --out <install>\romfs-patches-pc-<w>x<h>`. The setup does not write those, so without one the Switch prompts stay (*off at a game resolution not 16:9*).

## Automatic button prompts

`prompts = auto` (*Automatic* on the *Button prompts* row; OoT's default, and what its setup writes into a new `rekarina.ini`) shows the buttons of the device the player pressed something on last and changes them while the game runs: every OoT prompt folder's font holds every set, and `runtime/romfs.c` (`romfs_prompts_live`) writes the chosen set's table over the game's (`prompts-live.bin`). `runtime/input.c` picks the set (`input_prompts_wanted`, `prompt_track`):

- Only a new press counts. On the pad: a button going down, or a stick pushed past 35 % after the dead zone within 0.3 s of resting under 20 % (it counts again once back under 20 %), so a small deliberate nudge counts while a drifting or resting stick, or one creeping past the line slowly, never decides. On the keyboard: a key or mouse button the game binds, or Esc, going down while the window is in front and no text entry is open; keys already held when the window comes to the front (the Tab of an Alt+Tab) do not count. Mouse motion does not count, as no prompt shows the camera: a Steam Deck trackpad set up as a mouse keeps the pad's prompts.
- The other device takes over once its press has stayed the newest for 0.4 s, so a key brushed while playing on the pad does not flip the prompts; while the pad is in use and still held (a button down, or a stick that counted and has not come back), a key press does not count at all, so a key brushed while walking with the stick never does. Before any press the pad's set shows while one is connected, else the keyboard's; at start, when SDL has not listed a pad yet, the game gives it up to 1.5 s before it picks the overlay (`input: start: a gamepad is there after ... ms`), so a pad connected before the start is not taken for its absence. The log says `input: device in use: ...`; a pad that went away leaves the keyboard's set.
- The pad's set comes from the labels SDL reports (`input: gamepad type ..., vendor ... product ...: ... labels`):

| Device | Labels | `pad_layout = auto` | Prompts |
|---|---|---|---|
| Xbox and XInput pads, Steam Deck, ROG Ally, Legion Go, pads SDL does not know | A at the bottom | `xbox` | Xbox as labelled (`romfs-patches-xbox-label`) |
| DualShock 4, DualSense, DualSense Edge | cross at the bottom | `xbox` | PlayStation, cross for A (`-playstation-label`) |
| Switch Pro Controller, Joy-Con pair | A on the right | `nintendo` | the game's own |
| keyboard and mouse | | | PC, with the keys bound now (`-pc`) |

A Switch pad keeps the console's buttons: by position its right button, labelled A, confirms as on the console, and the game's own prompts are right. With `pad_layout` set by hand each face button's prompt still names the label of the button that presses it: an Xbox or PlayStation pad under `nintendo` gets the swapped sets (`-xbox`, `-playstation`), a Switch pad under `xbox` the swapped Xbox letters (its bottom button, labelled B, does A). An install without `romfs-patches-pc` keeps the pad's set for the keyboard. Without `prompts-live.bin` (TotK, an older OoT folder) `auto` is the Xbox set picked at start. A fixed choice on the row overrides all of this. Only the first gamepad SDL lists is read: on a Steam Deck with a second pad connected, the Deck's own controls may be the one.

The action hint at the bottom of the screen draws its four face buttons from images of the game's common layout archive, not from the font, and the game loads them once. A change of set also writes the new set's four images over the shown ones in memory (`romfs.c` `hint_apply`, from `prompts-hint.bin`, which `make_oot_prompts.py` `write_hint` makes beside `prompts-live.bin`): the log says `the action hint's images changed from ... to ...`. The first change searches the game's memory near the font table (about half a second); later ones reuse the places found. Without that file the hint keeps the start's images until the next start.

The song screen (the instrument's notes and its song book) draws its six note buttons from six more images of that archive, `OcarinaButtonA/B/X/Y/L/R_00` (ASTC 4x4, a white disc or key with a black letter). Every set's folder carries them as its prompts name those buttons (`make_oot_prompts.py` `ocarina_textures`): the sets by position trade A with B and X with Y, the PlayStation sets draw the shapes, the keyboard set the default keys and the mouse, and every set but the Switch's names the shoulders (LB RB, L1 R1, Q R). A drawing is new ASTC blocks in a box of the image that holds the whole letter and is opaque in the game's file, so the shape's edge stays the game's (`button_icons.py` `bntx_redraw_box`). A change of set in the running game writes them as the hint's (`prompts-ocarina.bin`; the log says `the song screen's images changed from ... to ...`). Without that file (prompt files made before it) the images stay those of the set the game started with, and prompt folders made before the song images show the game's letters in every set; at the first change of set the log says `no prompts-ocarina.bin next to the game` (`no prompts-hint.bin` for the hint).

The button in the hint's pill is text, not one of those images: a glyph of the button font in a text the layout sets once. It keeps the glyph it was first drawn with (the keyboard's key after a pad was switched on) until the next start. Open: the new table does not reach it, and marking the text changed the way the game's own setter does (tried 2026-11-19, with the game's functions, on every text of the interface) did not change it either; where the game keeps the drawn glyph is not found yet.

The hint is a diamond of the four face buttons where the console has them: A on the right, B at the bottom, X at the top, Y on the left. Under the layout by label (`xbox`) the pad's bottom button does A, so `romfs-patches-hint-label` holds the hint's layout with A at the bottom, B on the right, X on the left and Y at the top (`tools/oot_hint_layout.py`), and `romfs.c` serves it at start (`the action hint shows the buttons where a pad mapped by label has them`). The game reads the layout once, so the choice is the start's: `pad_layout` when it is set, and under `auto` the layout of the pad used last (`hint-pad.txt` in the game folder; by label when there is none, which is also what the keyboard's hint shows). A pad of the other layout opened in the running game logs `its buttons move to this pad's places at the next start`.

The settings menu follows the same device: the row's note says *in use: PlayStation*, the picture above its description shows that set, and the hints along the bottom name the pad's own buttons for the layout in effect (B or circle to go back, LB or L1 for the tabs) while the pad is in use, Q, E, Del and Esc while the keyboard is.

## Window keys

| Key | Gamepad | Action |
|---|---|---|
| F1 or Esc | Minus and Plus together, or the Guide button | open or close the settings menu; the game gets no input while it is open |
| F10 | L3 + R3 (both stick clicks) | performance overlay: off, frame rate, full panel |
| F11 | | borderless fullscreen on or off, saved to `rekarina.ini` |

The Guide button (Xbox button, PS button) opens and closes the menu as well; `REKARINA_MENU_GUIDE=0` (live, the *Guide button opens this menu* row on the Controls tab) turns that off. On Windows the Xbox Game Bar may take the Guide button before the game sees it; turn its controller shortcut off in Windows Settings.

The Minus+Plus combo never reaches the game. In the menu the arrow keys, the D-pad or the left stick move, Enter or A selects, Q and E or L and R switch tabs, Esc or B goes back (Switch button names, as in the table above).

## Remapping

The *Controls* tab of the settings menu lists, under the button layout, every Switch button, the eight stick directions the keys drive (movement and camera) and the shake key, each with two key cells and a controller cell. Select a cell, press A or Enter, then press the new key, mouse button or controller button; Esc or five seconds without a press cancel, and a controller button cancels a key cell. Delete, Backspace or X clears the selected cell. A key another action has moves to the new one (the description names the action it left); a controller button swaps with the one it replaces. A remapped controller button is physical: from then on the face buttons (A B X Y) stay where they are in both layouts, until the buttons are put back as the layout in use has them or *Reset all buttons* is chosen. F1, F8, F10, F11 and Esc cannot be bound (the menu, the flight recorder and the window keep them). Punctuation keys are named by their place on a US keyboard; the menu shows the character the keyboard layout in use puts there.

The tab writes `rekarina.ini` lines, one per Switch button or stick direction: `bind_<button> = <entry>, <entry>, ...`. The button is `A B X Y L R ZL ZR PLUS MINUS LEFT UP RIGHT DOWN LCLICK RCLICK`, or a stick direction `LSTICK_LEFT LSTICK_UP LSTICK_RIGHT LSTICK_DOWN` (movement, W A S D by default) and `RSTICK_...` (camera, the arrow keys), or `SHAKE` (keys only, G by default). An entry is `pad:<name>` or `key:<name>`:

- pad names: `south east west north l r zl zr start back lstick rstick dpad_left dpad_up dpad_right dpad_down lpaddle1 rpaddle1 lpaddle2 rpaddle2` (`zl`, `zr` are the triggers; the paddles are the back buttons of an Xbox Elite, DualSense Edge or Steam Controller, upper pair 1, lower pair 2, and no Switch button has them by default)
- key names: `a` to `z`, `0` to `9`, `f2` to `f7`, `f9`, `f12`, `num0` to `num9`, `space tab enter backspace lshift rshift lctrl rctrl lalt capslock insert delete home end pageup pagedown up down left right`, `num_add num_sub num_mul num_div num_decimal`, `minus equals lbracket rbracket semicolon apostrophe comma period slash backslash grave`, `mouse_l mouse_r mouse_m mouse_x1 mouse_x2`
- `key:default` and `pad:default` keep that part's defaults (`pad:default` is each layout's own button).
- `none` leaves the button unbound.

A line replaces every default of that button, pad and keyboard, so list the keys you still want or use `key:default`. Pad names are physical positions, so a line means the same in both layouts and replaces the layout's own button for that Switch button. Stick directions take keys only: the controller's sticks and the mouse camera always work. Buttons without a line keep the table above. Names are case-insensitive. An invalid entry logs one `input: bind ...` line and is skipped; a line without a valid entry changes nothing. The `REKARINA_BIND_<button>` environment variables work like any other setting. A changed line applies at once (`runtime/input_win.c` builds the tables again at its next poll).

A pad that reports its face buttons swapped (A/B and X/Y, as some GuliKit models do) is fixed with:

```
bind_A = pad:south, key:E
bind_B = pad:east, key:lshift
bind_X = pad:west, key:space
bind_Y = pad:north, key:mouse_l
```

## Dash on its own input

OoT dashes while its A button is held as the hero moves; a press of A does the action at hand (talk, open, check). The pause menu's controller guide names A "Actions" and "Dash (Hold)" (X is "Jump"); E is A on the keyboard. That is the game's own design, not a binding: no default binding puts two Switch buttons on one key or pad button. `REKARINA_STICK_DASH=1` (`stick_dash`, off by default, live; *Dash on the left stick's press* on the Controls tab) gives the dash an input of its own: a press of the gamepad's left stick while the stick is tilted past half way holds that button for the game until the stick comes back under a quarter, and `REKARINA_DASH_KEY` (`dash_key`, `v` by default, any key name of the `bind_` lines; *Dash key*) held with a direction does the same on the keyboard (`runtime/input.c`, `dash_apply`). The press of the stick that starts a dash does not reach the game as its own button, and a hold lasts at least 0.4 s, so a quick tap of the key is not taken for a press of A. The game's button keeps working as before, also during such a dash: its press and its release let go of the held button for two frames first, so the game sees them as its own. Until 1.2.2 both held B instead, which does not dash (measured 2026-11-19: a run at 4.1-4.5 m/s with B held, 5.0-5.6 m/s with the stick's press or the key on A, no change in height). In an input script `LCLICK` is the stick's press and `DASH` the key.

## Mouse camera

`mouse_mode = direct` (the default) turns the camera by the raw mouse motion on game 1.4.3, through hooks in the game's camera code (`runtime/mouse_direct.c`, the `mouse_direct` sites of `recomp/patches-v143.txt`). None of them changes a step of the gamepad alone. The free-orbit camera receives raw-count
angular increments at its final spherical-angle output: 0.08 degrees per count on both axes at sensitivity 1, with
no time multiplier, curve or carry into later frames. The controller's persistent pitch/yaw move with the output.
Mouse input is removed from its stick path first, so the camera's later vertical signed-square curve and velocity
filters cannot process it a second time. Other camera modes receive input after the radial dead zone, unit-circle
clamp and stick response exponent; Ultrahand has a separate hook after its manipulation input's power curves.
The game's camera inversion and gamepad response remain in effect. `mouse_sens` scales raw counts; its scale
differs from the stick-emulation mode. Start at 1.
The Controls tab switches between *Direct*, *Stick emulation* (`mouse_mode = stick`) and *Off* (`mouse_mode = off`):
off leaves the camera to the pad and never captures the cursor, as `mouse_sens = 0` does (`runtime/input.c`
mouse_sens_get reads 0), so the pointer stays free for the menus and its buttons do nothing in play (they count only
while the cursor is captured); asked for by a player who plays on a pad.
The orbit camera keeps its desired pitch at the final angular output, including stationary frames, so a later game
filter cannot recenter it. The persistent filter state accounts for the camera's current vertical offset instead of
accumulating corrections. A gamepad vertical input or the game's lock-on/override releases that mouse target.
While L is held, mouse counts move the ability wheel's virtual cursor, which stays where it was when the mouse stops.
The horizontal quick lists (D-pad up/left/right: materials, shields and weapons) use separate 32 ms right-stick steps,
with at least 32 ms of neutral between them. At sensitivity 1, 32 horizontal counts request a step; a large flick
cannot queue continued scrolling after the mouse stops. Vertical movement is ignored in these lists. Switching or
releasing a selector resets its cursor/step state without sending the last movement to the camera. Selector input is
not consumed by the direct camera or Ultrahand hooks.
The mouse never stands in for a stick shake: to detach a fused part, press G (`bind_SHAKE`). A press swings the right
stick left and right for 0.8 s, holding the key keeps it swinging. The arrow keys or the gamepad's right stick shaken by
hand work too. A mouse gesture for it would also fire while a held object is turned back and forth.
A step of the gamepad alone, with no mouse motion and no pitch the mouse holds, reaches the camera exactly as without
the hooks.
The mapping below is still used when the camera-input hook is unavailable or the game disables camera input.
Games 1.1.0 and 1.2.1 have no hooks and keep stick emulation whatever `mouse_mode` says.

OoT has a direct mode of its own (since 2026-11-15; `recomp/patches-oot-v111.txt` set `mouse_direct`,
`runtime/mouse_direct.c` `oot_camera_input`): the camera's stick update calls the host, which adds the counts to the
camera's own yaw and pitch, 0.08 degrees a count times `mouse_sens` (the vertical counts after `mouse_sens_y` and
`mouse_invert_y`), the game's camera inversion options applied as to the stick. A 180-degree flick turns 180 degrees in
the frame it reaches the game, where the stick tops out at about 107 degrees a second. The pitch stays where the mouse
leaves it, as with the stick, and keeps the game's own limits. *Stick emulation* in the Controls tab (`mouse_mode =
stick`) brings the mapping below back.

Some views are another camera's, whose angle the direct path cannot write: the houses, the water and the lock-on (ZL)
camera. Each of them turns by the stick input the update stores, times a rate of its own (measured: the house camera
138 degrees a second per unit, the lock-on camera about 60, the swimming camera about 107 like the land camera), and
none of them holds that input to a full stick (the house camera turned 249 degrees a second at 1.8). There the counts
become that input (`oot_inject`): the angle the mouse asks for over the update's time and the camera's rate, without
the stick's curve or its top speed, the rate learnt from the view within about half a second. Until 1.2.3 such a view
got the mouse through the stick emulation, calibrated for the land camera and capped at a full stick, behind the
stick's curve: off by the camera's rate (slow with ZL, fast in a house) and cut on a fast move in all of them. A
camera that does not answer to it is left to the stick emulation. `REKARINA_MOUSE_INJECT=0` leaves all of them to the
stick emulation as before; a number fixes the rate instead of learning it.

In stick-emulation mode, mouse motion (raw input) becomes right-stick deflection. Each presented frame takes the counts moved since the last frame, divides by the frame time and sets the deflection to `counts per second * REKARINA_MOUSE_SENS / 3000` of full, held for that frame, so the turn rate does not depend on the frame rate. This is not mouselook: the game's stick curve and dead zone still apply, so very slow movements do nothing and a fast flick saturates. The mouse deflection competes with the gamepad and arrow keys by the larger-deflection rule.

Both modes take the mouse's raw input (`WM_INPUT` counts on Windows, SDL's relative motion elsewhere): the Windows pointer speed and "Enhance pointer precision" (its acceleration) never reach the camera, in a window or full screen. The counts are taken only while the cursor is captured. `REKARINA_MOUSE_RAW=0` (`mouse_raw`, the *Raw mouse input* row on Windows, live) takes the hidden cursor's own motion instead, pointer speed and acceleration included, for a player who wants the desktop's feel (on a touchpad, for instance): each sample reads how far the cursor moved from the middle of the client area and puts it back there (`runtime/input_win.c`, `cursor_take`). The SDL back end ignores the key.

`REKARINA_MOUSE_SENS_Y` (`mouse_sens_y`, 0.1 to 3, default 1, live; *Mouse vertical speed*) multiplies the vertical counts, so up and down can turn slower or faster than left and right; the fraction a scaled count leaves is carried into the next sample. `REKARINA_MOUSE_INVERT_Y=1` (`mouse_invert_y`, live; *Invert mouse vertical*) turns the vertical counts over. Both apply to the camera only (the stick emulation, and in direct mode the camera and Ultrahand hooks), not to the selectors' cursor, and the game's own camera options apply on top (`runtime/input.c`, `mouse_take_camera`).

`REKARINA_MOUSE_SENS` (0 to 10, default 1, in steps of 0.1 on the Controls tab; higher turns faster, 0 turns the mouse camera and the cursor capture off). While the window is the foreground window the cursor is hidden and confined to the client area. It is released on focus loss, minimise, while F11 changes the window, during a size or move loop and while a text entry is open. Esc releases it by hand: the cursor shows, the mouse camera and mouse buttons stop, and the window can be moved, minimised or closed. A mouse click inside the client area captures it again; that click does not reach the game. Activating the window by a click on its title bar or border does not capture, so it can be dragged or closed right away; Alt+Tab or a click in the client area captures as before. `REKARINA_INPUT=none` disables the capture too.

`REKARINA_STICK_DEADZONE` (0 to 50, default 8) is the gamepad stick dead zone in percent of full deflection, the same for both sticks. It is round: a stick inside the circle reads centred, and the rest of its travel is scaled to start at 0 at the edge, so full deflection still reaches full. Raise it for a stick that drifts. The game's own dead zone applies after it. `REKARINA_LSTICK_DEADZONE` and `REKARINA_RSTICK_DEADZONE` (the *Left/Right stick dead zone* rows under *Analog sticks* on the Controls tab) set one stick each and fall back to `REKARINA_STICK_DEADZONE` while unset, so a worn stick can get a larger zone than the other. With a gamepad connected each row shows its stick's deflection now, before the dead zone (*stick now*): let go of the stick and set the row a few percent above the value it rests at.

`REKARINA_STICK_SENS` (`stick_sens`, default 1; *Stick camera speed* on the Controls tab) multiplies the game's own right-stick camera speed, top speed included, at whatever camera sensitivity the game's options are set to. Keys bound to the right stick follow it. A mouse that reaches the game as the right stick is divided back by it, so `mouse_sens` alone sets the mouse's speed (`runtime/input.c` `mouse_camera`; on OoT by its square root, since the camera squares the stick first). TotK (0.1 to 3, read at start) gets a copy of its camera pack with the turn-rate table multiplied (`runtime/camera_speed.c` `cam_pack_build`). OoT (0.5 to 4, live) has no such file: its camera turns by the curved stick times a rate that four small functions return, and patch set `stick_sens` (`REKARINA_STICK_SENS_PATCH`, on by default; `recomp/patches-oot-v111.txt`) multiplies each rate as it is loaded (`runtime/camera_speed.c` `oot_camera_rate`). Other cameras that ask the same functions for their rate turn faster too; a camera with a rate of its own keeps the game's speed.

The stick direction bits (`StickLLeft` to `StickRDown`) are derived from the sticks at 50% deflection. A gamepad plugged in later is picked up within one second; with none connected the log says `input: no gamepad`.

`REKARINA_CURSOR_HIDE` (`cursor_hide`, 0, 1, 3, 5 or 10 seconds, default 3, live; *Hide an idle cursor* on the Controls tab): a cursor that is not captured (the menu is open, Esc released it, the mouse camera is off) and has not moved over the client area for that long is hidden until it moves again, so a gamepad player does not keep an arrow on the picture. 0 never hides it. Windows: a 250 ms window timer and `WM_SETCURSOR` (`runtime/input_win.c`); SDL: `SDL_HideCursor` from the window thread's idle tick (`runtime/input_sdl.c`).

## Several controllers, and controllers that send nothing

Every gamepad SDL lists is opened, and the one pressed last is the one the game reads: its labels, the automatic prompts, rumble, HD rumble and gyro go with it (`REKARINA_PAD_FOLLOW`, `pad_follow`, default 1, live; *Use the controller pressed last*). A press is a button, a trigger past half way or a stick past half way, so a resting or drifting pad never takes over. A receiver with no pad, Steam's virtual controller beside the real one, or a second driver's copy of the same controller can then no longer hold the game on a device that sends nothing. `pad_follow = 0` reads only the first gamepad found, as before. A pad that went away hands over to another one still connected. The pads are rescanned once a second; one SDL cannot open is not tried again (an open holds SDL's joystick lock long enough to stall the gamepad thread) (`runtime/input.c`, `scan_pads`, `pad_follow_press`).

`REKARINA_PAD_DRIVER` (`pad_driver`, `auto` or `compat`, read at start; *Controller driver*): `compat` turns SDL's own drivers off (HIDAPI, and on Windows Raw Input and Windows.Gaming.Input), so a controller is read through the system's XInput or DirectInput (on Linux, the kernel's evdev driver). Some controllers are listed by SDL's drivers but send nothing through them (some 8BitDo pads, an Xbox One S pad over Bluetooth); the system's path reads them. Gyro, HD rumble and the PlayStation and Switch labels need `auto`. The menu's controller picture (select *Controller driver* or any controller row) lights each button and draws the sticks live, so a player can see whether the pad sends anything at all.

`REKARINA_KBM_OFF` (`kbm_off`, default 0, live; *Keyboard and mouse off with a controller*): while a gamepad is connected the game takes nothing from the keyboard and the mouse, for a pad player whose keyboard gets leaned on. The settings menu (Esc, F1), F11 and the screenshot key still work (`runtime/input.c`, `kbm_off`).

## Screenshots

`REKARINA_SCREENSHOT_KEY` (`screenshot_key`: `f12` by default, `f9`, `f7`, `f6`, `f5` or `none`; *Screenshot key* on the Game tab) saves the next presented frame as `screenshots\rekarina-<date>-<time>.png` beside the game. By default the picture is the game alone: the settings menu and the overlays are left out of that one frame. `REKARINA_SCREENSHOT_OVERLAY=1` (*Screenshots with the menu and overlays*) keeps them (`runtime/gfx_present.c`, `screenshot_path`).

## Mouse in the game's menus

On game 1.4.3, while one of the game's menus has its focus box (the frame the d-pad moves: the title, the save list, the inventory and its System tab, Options, shops, dialogue choices), the cursor is released from the mouse camera and shown in the middle of the window. The item under the pointer takes the focus as if the d-pad had moved there, with the game's highlight, sound and description; a left click is A on it, a right click is B. The keys and the pad keep working, and the focus moves only when the pointer moves. This happens only while the keyboard and mouse are the device used last: after a gamepad button press or a stick pushed past half way the menus are as on the console, with no pointer, until the mouse moves or a key is pressed (`input_kbm_last` in `runtime/input.c`). The ability wheel, the d-pad strips, Autobuild, the tablet's camera and the map stay gameplay: the mouse keeps the camera there. When the menu closes the cursor is captured again.

`runtime/menu_cursor.c` reads eui's screens at each pad read and moves the focus with the writes `eui::BoxCursorMgr::moveBoxCursor` makes, so no game code is called. `menu_cursor = 0` (`REKARINA_MENU_CURSOR=0`, live; the *Mouse in the game's menus* row of the Controls tab) turns it off; `REKARINA_MENU_CURSOR_LOG=1` logs every screen it reads (by default the first six), with the layout size, the node count and the hit panes it uses.

## Switch 2 controllers

Windows does not present the Switch 2 Pro Controller as a gamepad SDL can open (the log says `input: no gamepad`), so it needs a bridge program that wakes it and creates a virtual gamepad. The DS4Windows fork by hbashton ([github.com/hbashton/DS4Windows](https://github.com/hbashton/DS4Windows), tested with release VIIPERRC4.5.1, a pre-release) with the output set to an emulated DualSense gives buttons, rumble, gyro and accelerometer, and reads the sticks through the controller's calibration (they rest at 0 to 2%). The game reads its virtual DualSense directly; the log then shows `input: gamepad "DualSense Wireless Controller" (rumble, gyro and accelerometer, ...)` and `input: motion sensors on`. Its release notes also list Joy-Con 2 support, which is untested here.

Bridges that output a ViGEm DualShock 4 (ProCon2Windows, connectyourpro as tested) give buttons and rumble but no motion: their virtual DualShock 4 reports a gyro and an accelerometer that never change, so motion aiming and Ultrahand rotation do nothing. Run one bridge at a time; Steam Input can also claim the controller, so close Steam if the bridge does not find it.

## Scripted input

`REKARINA_INPUT_SCRIPT=<file>` replays presses. It is ORed with real input and still applies under `REKARINA_INPUT=none`. One event per line, `#` starts a comment:

```
<frame> <button> [hold_frames]            # A B X Y L R ZL ZR PLUS MINUS LEFT UP RIGHT DOWN LCLICK RCLICK; hold defaults to 4
<frame> LSTICK|RSTICK <x> <y> [hold_frames]   # -32767..32767, up is +y
<frame> MOUSE <dx> <dy>                  # direct-mode raw counts, once; down is +dy
@<ms> <button> [hold_ms]                  # timed on the world clock
@<ms> LSTICK|RSTICK <x> <y> [hold_ms]
@<ms> MOUSE <dx> <dy>
```

`<frame>` counts 60 Hz ticks of game time since the first frame (presented frames at 60 fps), so the title menus get the presses at the same game time at any frame limit. `@<ms>` counts milliseconds of the world clock: game time stepped since the end of the first save load (the player leaves the title screen's origin), so a timed event lands at the same game time at any frame rate and load length. A later load, such as fast travel, still shifts what follows. In a script with timed lines the untimed ones stop when the world clock starts, as the load ends at another tick in each run. Each event logs once (`input: script frame N tick T: A`). `tools/input/load_save.txt` loads the first save from the title screen.

`REKARINA_INPUT_RECORD=<file>` (`input_record` in `rekarina.ini`, or Input recording in the menu) writes the merged input in the same format, one line per button press and per stick segment: events before the world clock starts by tick, the rest timed; a button or stick held when the world clock starts is cut there and goes on as a timed event. The file replays as a script, which makes it the repro to attach to a bug report.

In direct mouse mode the recording also writes one `MOUSE` event per consumed raw displacement. These are one-shot
displacements rather than held stick segments. They replay without cursor capture, including with `input = none`;
the game must be accepting camera input when the event reaches its camera update.

`REKARINA_TEST_PAD=<lx>,<ly>,<rx>,<ry>` attaches an SDL virtual gamepad at start whose sticks rest at these raw SDL axis values (-32768 to 32767, down is +y), so a drifting or held stick can be tested without a controller: the values go through the gamepad mapping and `REKARINA_STICK_DEADZONE` like a real pad's, and `REKARINA_INPUT_RECORD` shows what reached the game. A fifth value, `ps5`, `ps4`, `switch` or `xbox`, gives the pad that controller's USB ids, so the automatic prompts and layout follow it: with `REKARINA_TEST_PAD=0,0,0,0,ps5` the game switches to the PlayStation prompts (`button prompts changed to playstation-label` in the log), with `switch` to the Nintendo ones and the Nintendo layout (checked 2026-11-15).

## RenderDoc capture (blocked)

`REKARINA_RDOC=<frame>[,...]` (`runtime/rdoc_win.c`; `REKARINA_RDOC_DIR`, `REKARINA_RDOC_DLL`) loads RenderDoc before `vkCreateInstance`. RenderDoc hides `VK_EXT_external_memory_host`, which the back end requires for guest pool imports (`runtime/vk_memory.c`), so device creation fails under it. A capture needs a copy-based pool fallback first.
