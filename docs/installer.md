# Installer

A public release that turns the player's own game files into a playable TotK folder. The release carries no game
data and no code derived from the game: the installer builds the game on the player's machine. The installed folder
holds only the game; the installer and its tools stay outside it.

## What the player does

1. Downloads `TotK-Setup-<version>-windows.zip` from GitHub Releases (none is published yet), unpacks it anywhere and
   starts `TotK-Setup.exe`.
2. Picks:
   - the base game (XCI or NSP) and the 1.1.0, 1.2.1 or 1.4.3 update (NSP); an NSZ or XCZ (compressed by the nsz
     tool) works as it is, the setup unpacks it itself;
   - their `prod.keys`;
   - optionally the firmware folder (`system/Contents/registered` of an unpacked firmware or an emulator's install),
     needed for Chinese, Korean and Traditional Chinese text;
   - the game language and the install folder.
3. Waits for the setup, one time. The window shows each step and its progress.
4. Plays `totk.exe` from the install folder. The installer and the source NSP or XCI (or NSZ, XCZ) can be deleted.

The install folder (`main.c` `portable_paths()` reads this layout; it stays portable):

```
<install folder>/
  totk.exe, SDL3.dll, libopus-0.dll, totk_xlate.exe
  nvngx_dlss.dll                 NVIDIA DLSS, downloaded by the setup's DLSS step when the player keeps it ticked
  totk.ini                       language; the other keys as commented defaults (rekarina.ini.example)
  exefs/, romfs/                 extracted from the player's files
  shared-fonts/                  the Switch system fonts, with Chinese, Korean or Traditional Chinese text
  profile/                       saves
  mods/                          the player's mods, one folder per mod (created by the menu's "Open mods folder")
  logs/                          totk.log of the last run, totk.1.log to totk.4.log of the runs before it
  shader and pipeline caches     prepared by the setup; shader-cache/shader-cache.pack holds the translations
  rekarina-install.json              what built this folder: release version, input hashes, finished steps
```

## Updating the port

The player runs a newer installer on the same folder. It reads `rekarina-install.json`, keeps `romfs/`, `exefs/`,
`shared-fonts/`, `profile/` and `totk.ini` after checking their hashes, rebuilds `totk.exe` and re-prepares the
shaders. A newer release that changes nothing game-derived skips the rebuild.

Given an update file other than the recorded one (1.1.0, then 1.2.1, then 1.4.3), the installer extracts and builds again from it
and keeps `profile/`. Saves carry over both ways between 1.2.1 and 1.4.3: 1.4.3 converts a 1.2.1 save on load, and 1.2.1
loads the saves 1.4.3 wrote.

## Setup steps

1. **Check** before writing anything: an NSZ or XCZ input is unpacked first (below), the files open with the keys, the update is 1.1.0, 1.2.1 or 1.4.3 (by hash, as
   `tools/extract.py`), the keys are new enough for the update, enough space is free on the target disk (about 36 GiB at the peak of a
   first install, 20 GiB after it), the CPU is at least x86-64-v2 (SSE4.2; with AVX2/FMA the faster `v3` build is used,
   otherwise the `v2` one), a Vulkan 1.3 GPU exists. Each failure is one clear message.
2. **Fonts** with Chinese, Korean or Traditional Chinese text and a firmware folder: extract the Switch system fonts
   from the firmware into `shared-fonts/` (MacNativeGames' `extract_shared_fonts.py`, which verifies every NCA layer
   and BFTTF header). Without a firmware folder the setup warns and goes on; the game then lacks those glyphs.
3. **Extract** ExeFS and RomFS into the install folder and verify the hashes.
4. **Lift** the ExeFS to C (`recomp/gen.py`, `recomp/patches-v121.txt` or `patches-v143.txt`) into a temp folder next
   to the installer. The code cheats of the mods' cheat files and the mods' ExeFS patch files become patch sets first
   (`tools/cheats.py`, `tools/exefs_patches.py`, status in `build-cache/cheats.status` and `exefs.status`); a file the
   tools refuse costs those cheats or patches, not the install.
5. **Compile** the generated C, `modules.c`, `romfs.c`, `patches.c` and `ov_zstd.c` (they bake the per-install layout, RomFS
   hashes and the game version's patch page) and link
   them with the prebuilt runtime library for the host's level (`librekarina.a` or `librekarina-v2.a`) into the build's `rekarina.exe`, which goes into the folder as `totk.exe`
   (`build_game.py --runtime-lib`; about 3 min on 24 cores).
6. **Prepare** (only when the runtime, the translator or the game version changed, not for a new lift) the RomFS
   patches and the shaders (`tools/make_romfs_patches.py`, `tools/prepare_shaders.py`,
   `totk.exe --prepare-shaders`) in the install folder, then fold the shader cache's loose files (four per shader, about
   117,000) into `shader-cache/shader-cache.pack` (`tools/pack_shader_cache.py`). totk.exe reads the pack first and
   writes the translations it makes in play as loose files; running the tool again with the game closed folds those in.
   The patches are written four times: `romfs-patches`, `romfs-patches-xbox` and `romfs-patches-xbox-label`
   (`--prompts`: Xbox button prompts for `prompts = xbox`, the first for `pad_layout = nintendo`, the second for `xbox`
   and `auto`), and `romfs-patches-pc` (keyboard and mouse prompts for `prompts = pc`). The three prompt folders are
   optional: when one cannot be made, the setup warns and finishes.
   When the graphics driver crashes while building the shaders, `prepare_shaders.py` runs that part again without the
   variants it was building ([building.md](building.md), step 6), and the setup finishes with a warning that names how
   many were left out.
7. **DLSS** (the window's tick box, on by default, or `--dlss`; Windows with an NVIDIA RTX GPU): download
   `nvngx_dlss.dll` from NVIDIA's own DLSS SDK repository (github.com/NVIDIA/DLSS, a pinned tag, NVIDIA RTX SDK
   license) into the install folder and check its size and SHA-256 (`steps.py`, `Dlss`). The release does not ship
   the library; NGX loads it from the folder of totk.exe for `dlaa` and the DLSS modes. A DLSS library already in the
   folder (a newer one the player put there) is kept. Without an RTX GPU the step only logs and does nothing; a failed
   download warns, the game runs without DLSS, and the next run of the setup tries again.
8. **Clean up** the temp folder, except the generated C and the objects (about 1.3 GB), and write
   `build-cache/rebuild.txt` in the install folder.

A cancelled or failed setup resumes from the last finished step (`rekarina-install.json`).

## Rebuilding for the mods

The generated C and the objects stay in the temp folder, so the next lift rewrites only the chunks whose C changed and
Ninja recompiles only those: a new or edited code cheat costs the lift (under a minute on 24 cores, a few minutes on 4)
and a few dozen chunks, not a full compile, and the shaders stay prepared.

`build-cache/rebuild.txt` holds the command that opens the setup window in its rebuild mode on the install folder
(`installer/gui.py --rebuild <install folder>`, started through `python -c` so the release folder is on `sys.path`
whatever the working folder), one argument per line. When an enabled mod has code cheats or ExeFS patch files the build
lacks, `totk.exe` runs it at start and exits (`runtime/cheats.c` `rebuild_if_needed`); the
window runs the setup on the folder at once and starts the game again when it succeeds. A failed or cancelled rebuild
offers to start the game with the code it has; `build-cache/rebuild.skip` then keeps the same files from starting it
again, until "Rebuild the game now" on the Cheats or Mods tab. `[mods] mods_rebuild = off` turns it off. The rebuild
needs the release folder the setup ran from: a moved or deleted release folder means running a setup again.

## Command line

`installer/` (stdlib only; `installer/layout.py` maps the release folder and, in a repository, the dev toolchain):

```
python -m installer --base FILE --update FILE --keys FILE --lang CODE --dest DIR [--firmware DIR] [--root DIR] [--temp DIR] [--verify]
```

- On a folder with `rekarina-install.json` only `--dest` is needed; the game files only when the extraction must run again.
- `--firmware DIR` is the folder holding the firmware NCAs: an unpacked firmware, or the emulators' installed
  `Contents/registered` (the parent `Contents` folder works too). It is read only with Chinese, Korean or Traditional
  Chinese text, and only when `shared-fonts/` is not already in the install folder.
- stdout: one JSON event per line, types `start`, `plan`, `step` (started, done, skipped), `progress` (step, phase,
  done/total/unit), `space` (bytes needed and free on the install disk and the temp disk), `warning`, `error`, `end` (`installer/events.py`). Messages are ids with parameters;
  `installer/text/en.json` holds their English text. The human-readable log goes to `<temp>/logs/`.
- Exit codes: 0 done, 1 failed, 2 bad command line, 3 cancelled. The window cancels with Ctrl+Break to the
  installer's process group.
- Each step records a key over its inputs (runner.py). A step with the recorded key and intact output is skipped, so
  resume and update are one rule; output kept from another release is re-hashed first.
- Peak space on the install disk is about 36 GiB, not 22: the extraction copies both Program NCAs (16.3 GiB) next to
  the merged RomFS (15.9 GiB) before it deletes them. The check computes the exact figure.
- Window (`python -m installer.gui`, `installer/gui.py` draws, `installer/guilogic.py` decides): form, step list with
  progress, space line, collapsible log, Cancel (Ctrl+Break to the CLI's process group, then it waits for the exit),
  finish screen with Open folder. English, Russian, Brazilian Portuguese or Simplified Chinese, chosen from the Windows
  display language (zh-CN and zh-SG give Chinese; the Traditional Chinese regions give English), switchable in the
  window; every sentence is an id in `installer/text/<en|ru|pt-BR|zh-Hans>.json`. A Chinese window still starts with
  English as the game language, because Chinese game text needs the Switch shared fonts, which the setup extracts only
  from a firmware folder. A folder with
  `rekarina-install.json` needs only the folder. `--selftest [--shots DIR]` builds every screen in every language and
  closes itself.
- `tests/installer_gui_test.py`: event model, form, catalogs, cancel, the self-closing window and one end-to-end run
  of the CLI in `--verify` mode (needs `build/install-test` and `build/extracted/v121`; see its docstring).
- `tests/installer_test.py`: bookkeeping with fake steps. `tests/installer_e2e.py`: a dev-mode run with the files of
  `config.local.toml`.

## Release layout

The unpacked release; `tools/make_release.py` assembles it and `--zip` packs it:

```
TotK-Setup.exe        starts python/pythonw.exe -m installer.gui (installer/launcher/launcher.c, no console)
installer/            the installer: steps, CLI, window, en/ru/zh-Hans text catalog
python/               Python 3.14.9 embeddable + numpy, lz4, cryptography, cffi, pycparser (+ tkinter for the window)
recomp/, tools/       the lifter and the build, extraction and shader scripts the steps run (copied from the repo)
llvm/                 llvm-mingw, x86_64-only subset (Apache-2.0 with LLVM exception; mingw-w64 parts permissive)
runtime/              librekarina.a, librekarina-v2.a and the headers the generated code and modules.c/romfs.c/patches.c
                      include
bin/                  totk_xlate.exe, hactool.exe, SDL3.dll, libopus-0.dll, ninja.exe
data/                 shader_list.tsv (MacNativeGames, MIT); pgo-<ver>.profdata + .json, the execution profile an
                      update's code and the runtime libraries compile with (building.md, "Profile-guided build")
THIRD_PARTY_NOTICES.md
rekarina.ini.example, VERSION  the default ini; the release string (the repository's VERSION plus the commit)
```

- SDL3 comes from the upstream release, not MSYS2: MSYS2's DLL imports the LGPL libiconv.
- Every bundled third-party piece gets a row in `THIRD_PARTY_NOTICES.md`.
- The release contains no code, data, text or assets of the game; `make_release.py` runs `tools/check_content.py` over
  the folder before it finishes. The execution profiles name lifted functions by address (`f_<address>`) and hold
  branch counts and control-flow hashes of the generated C.

## Switch system fonts

Simplified Chinese, Traditional Chinese and Korean text needs the Switch system fonts (`0-standard.bfttf` and the other
nn::pl shared fonts). The game does not ship them. Without them it exits before its first frame in those languages
(`rt_fatal` in `runtime/ov_system.c`). Supply them from your own console, in either way:

- Give the setup the firmware folder (`--firmware DIR`, or the window's firmware field); the Fonts step extracts them to
  `shared-fonts/` in the install folder.
- Run `python tools/extract_fonts.py --firmware <dir>` with `.venv-py314` and the keys from `config.local.toml`, or
  point `REKARINA_SHARED_FONTS` at a folder that holds the extracted fonts.

## Linux release

`tools/make_release_linux.py` builds the same setup for Linux and packs it as `TotK-Setup-x86_64.AppImage` (the static
type-2 runtime: no libfuse2 needed on the host). The player makes it executable and starts it; on a Steam Deck, from
Desktop Mode. The command-line setup needs the desktop session's display (`DISPLAY` or `WAYLAND_DISPLAY`) because preparing the
shaders opens a window; the check step fails at once without it. Building the release also needs `make`, `pkg-config`
(without it Tk's configure finds no Xft and the build stops), and for OoT's AV1 movies `meson` and `nasm` (dav1d, built
static into `runtime/deps`); on Ubuntu `apt install make pkg-config meson nasm`. Run it attached to a terminal: under WSL a
build left running after its `wsl` command returned failed in `llvm-strip` with "No such file or directory". The bundled Tcl/Tk 9.0.4 is rebuilt with
Xft against the sniper sysroot so Russian text uses system TrueType fonts. The setup builds the game into the folder they pick, as on Windows, and the game is `totk` there with
`libSDL3.so.0` beside it. The folder inside the AppImage:

```
AppRun                starts python/bin/python3 -m installer.gui (no bytecode written into the read-only mount)
totk-setup.desktop, totk-setup.png
installer/, recomp/, tools/, data/, licenses/, THIRD_PARTY_NOTICES.md, rekarina.ini.example, VERSION   as on Windows
python/               python-build-standalone 3.14 (with tkinter) + the installer's packages; a .pth puts the folder,
                      tools/ and recomp/ on sys.path
llvm/                 LLVM 23 (clang, lld, llvm-ar) and llvm/sysroot: the files of the Steam Runtime 3 "sniper" SDK the
                      game's compile and link read (about 130 files)
runtime/              librekarina.a, librekarina-v2.a, headers, the install-side sources, totk.png; deps/ with the Vulkan
                      headers, VMA and a static zstd
bin/                  totk_xlate, hactool, ninja, libSDL3.so.0
```

- What we compile is built against the sniper sysroot (glibc 2.31): the game, hostcheck, hactool and the translator
  need no newer glibc. The game links Opus, libvpx, zstd and libstdc++ statically and needs only glibc, the system's
  Vulkan loader and the SDL3 beside it. The setup itself needs glibc 2.34 (Ubuntu 22.04, Debian 12, Fedora 35,
  SteamOS 3): the official LLVM binaries and the `cryptography` wheel are built against it. `make_release_linux.py`
  checks every ELF file in the folder against that floor (`check_glibc`), and the setup's check step stops with one
  message on an older system.
- The same setup folder is also packed as `Rekarina-Setup-<version>-linux-x86_64.tar.gz` (`tools/pack_linux_release.sh
  <setup folder>`, with `SHA256SUMS` for it and the AppImage): unpack it and run `./AppRun`; its setup keeps its temp
  folder in `temp/` of the unpacked folder. `.github/workflows/release-linux.yml` makes both by hand from the Actions tab.
  Checked 2026-11-20 on Fedora 44, from the AppImage's own mount (`--appimage-mount`, with `--temp` inside the install
  folder as the window does): check, extract, lift, compile (10 minutes with the bundled LLVM and sysroot; the binary
  needs glibc 2.29, links Opus, libvpx, zstd and dav1d statically and passes `--selftest`), prompts, shader
  preparation (16 minutes), package and the driver-cache warm-up (26 minutes on the first start) all finished, and the
  game it built starts and runs at 62 fps in the title scene. The Python of the AppImage lives on a read-only mount, so
  the `zbic` wheel goes under the setup's temp folder (`installer/oot.py` `ensure_zbic`). The setup's last step, the
  texture cache (`--no-textures` skips it), ran the machine out of memory (31 GiB with 16 GiB of zram swap) and froze it
  after 4.4 GiB of the cache; on a machine with less free memory pass `--no-textures`.
- The setup's temp folder is `.setup-temp` inside the install folder (`--temp` changes it): the AppImage is mounted
  read-only, and a Steam Deck's `/home` is often too full for the generated C while the game goes to the SD card.
- The game keeps Mesa's shader cache in `mesa-cache` beside `totk` unless `MESA_SHADER_CACHE_DIR` is set, for the
  same reason: RADV fills its default folder in `~/.cache` up to 1 GB.
- The window follows `LANG` for English or Russian, cancels through SIGTERM to the CLI's process group (the CLI handles
  it as Ctrl+Break on Windows) and opens the finished folder with `xdg-open`.
- Requirements beyond Windows': Linux 6.7 or later (the write watch, `docs/building.md`) and glibc 2.34.
- Building the release needs one game version extracted and lifted (`tools/extract.py`, `recomp/gen.py`, either
  version): the runtime library includes the generated `layout.h`, whose shared part (`MOD_COUNT`, `MOD_MAIN_BASE`)
  is the same for both. `tools/make_release.py` takes `generated/v121`, else `generated/v143`.

## Constraints

- The installer steps contain no Win32 calls: Linux runs the same steps with its own compiler ("Linux release").
- The keys file path is passed to the tools; key values never appear in the UI, logs or files.
- The window is a front end over the CLI (`python -m installer --base ... --update ... --keys ... --lang ... --dest
  ...`), which prints one JSON progress event per line. Tests and CI use the CLI.
- Paths may hold any Unicode characters (a Cyrillic user name puts the temp and install folders under it). Every exe
  the setup runs or builds reads its arguments and paths as UTF-8 through an `activeCodePage` manifest: hactool
  (our patch, [building.md](building.md)), totk.exe (`runtime/rekarina.rc`), ninja (upstream). The Python steps write
  their files and logs as UTF-8, and `build_game.py` quotes paths with spaces on its Ninja command lines.
- A game file whose data fails its hash checks or ends early stops the setup with `check.input_damaged`, which names
  the file and asks to copy it again (`extract.DamagedFile`). A failed merge checks the base Program NCA alone to tell
  which file is damaged.
- Unsigned exe: SmartScreen warns on first start.
- NSZ and XCZ input (game files compressed by the nsz tool: NCAs stored as NCZ, their sections decrypted and
  compressed with zstd): the check step tells them by their content, not their name (an NSP or XCI holding NCZ files),
  so the file dialog has one filter for all four. It unpacks each into `<temp>/unpacked/<base|update>.nsp`
  (`tools/ncz.py`), an NSP of the NSZ's files or of the XCZ's secure partition, and the extraction reads that.
  Unpacking encrypts the sections again, so every NCA comes back byte for byte, and checks each against the hash its
  name carries; a mismatch is `check.input_damaged`. It needs the unpacked size free in the temp folder (checked
  first), runs at about 500 MB/s, and an interrupted setup reuses a finished NSP of the same file. The extract step
  deletes the folder when it is done. An XCZ update is refused like an XCI one.

## Later

- Re-warm the driver cache on start after a move, GPU or driver change.

## Rekarina: the OoT setup

The working files of a game folder (the generated C, the objects, the build's caches: about 9 GB) are in the setup's
`temp/oot-<tag of the game folder>`; `--temp` names another base. When the setup's own disk has no room for them and
the game folder is on another disk that has, they go into `<game folder>\.setup-work` instead (`installer/oot.py`
`work_folder`; the log says so), and later runs go where the earlier one's objects are.

`python -m installer.gui_oot` (window) and `python -m installer.oot` (command line) build OoT (Switch 2)
the same way, from the player's NUP container. The player chooses the folder that holds the dump and the install
folder; the steps are those of `installer/oot.py`:


1. **Check**: the `.nup` in the folder (also one level down): the one that holds the game's title id
   (`0400486024DEC000`, `tools/oot_exefs.py find_nup`; the largest of those, else the largest `.nup`: a folder of
   several Switch 2 dumps gave another game's), its newest package's ExeFS against the
   hashes the code patches were written for (`tools/data/expected-oot.json`), the tools, the free space.
2. **Extract** the ExeFS (`tools/oot_exefs.py`, the NUP reader in `tools/vendor/nup`): as stored for the lifter,
   with uncompressed modules into `<install folder>/exefs` for the game.
3. **Lift** it to C (`recomp/gen.py`, `recomp/patches-oot-v111.txt`).
4. **Compile** (`tools/build_game.py --version oot`): the whole exe with its execution profile when
   `build/pgo/pgo-oot.profdata` (a release: `data/pgo-oot.profdata`) exists, which is the generated code at -O2 with
   the game's hot paths known (about a fifth more frames than without); otherwise the generated code at -O1 with the
   hottest chunks at -O2 (`tools/data/hot-oot-v111.txt`). About ten minutes on eight cores either way.
5. **Prompts** (`oot_prompts`): the button prompt overlays and fonts (`tools/make_oot_prompts.py`, `tools/oot_hint_layout.py`,
   `tools/button_icons.py`, `tools/make_ext_font.py`), a minute, redone when those tools or the dump change.
   **Prepare** (`oot_prepare`): the shaders (`tools/oot_prepare_shaders.py`). Its key (`Prepare.key`) holds only what the
   prepared shaders follow from: the dump, the translator's identity (`rekarina_xlate.id`), `oot_prepare_shaders.py`,
   `prepare_shaders.py`, `oot_romfs.py`, `shader_census.py`, the seed `tools/data/shader-answers-oot.bin.gz`, the mode and
   `SHADER_FORMAT` (a hand switch for a runtime change that alters what `--prepare-shaders` makes). Not the compile key:
   a new exe costs the compile and the package step (which copies it and warms the driver's cache under the game's file
   name), not the preparation. A record made under the older key form is taken over (`Step.adopts`) when its release
   is in `LEGACY_PREPARED_BY` and the translator is the one that prepared it.
   Every run translates every variant (`all`), in the foreground, and since 1.2.8 only translates
   (`REKARINA_PREPARE_DRIVER=0` through the environment, so the tools and the key stay as they were): the driver's
   build of every prepared variant's stage library, 25 of the step's 35-40 minutes on 16 threads and 20 minutes to 1.5
   hours on players' PCs (a cold driver cache; an AMD card, whose disk cache is keyed to the exe's path, built them a
   second time in the game folder), is the game's first start and the launcher's background work (below). The
   translation writes the shader cache without a disk flush per file (`gfx_shader_cache_flush`; the package step packs
   and checks every file afterwards): until 1.2.7 each translated shader waited for four flushes and four write-through
   renames, which held players on a DRAM-less NVMe, a SATA SSD or an HDD (and under Proton) to a few shaders a second with
   the translators and the processor idle. Its progress line every 5 s says where the time goes ("N of P translators
   busy, a translation X ms: translator, waiting for one, cache files, same program"). 1.2.1 to 1.2.3
   prepared the variants play met on a first install (`met`, `--met-only`, a sixth of the work) and left the rest to a
   background job of the launcher; play then translated and built the rest in its frames, and a setup run from a fresh
   temp folder over a folder that had prepared all prepared `met` and its package replaced the folder's pack of every
   variant (2 GB) with those (340 MB). From the release after 1.2.3 on (`shader_mode`):
   - a folder recorded `met` prepares the rest at its next setup run: the step first takes in what the game folder
     holds under the same translator (`seed_from_folder`: its pack merged into the build folder's with
     `pack_shader_cache.py --merge-from`, its pipeline cache, recorded pipelines and the driver's `nv-cache`), so it
     translates and builds only what is missing;
   - a record of every variant whose key stands is done with no output in the temp folder when the game folder holds
     the pack and `shader-prepared.bin` under this translator (`Prepare.current`, `folder_prepared`): a setup unpacked
     again or a cleaned temp folder prepares nothing, and the package step, finding no shader output in the build
     folder, leaves the folder's as it is;
   - the package step never takes prepared shaders away: before the build's pack replaces the folder's, the entries
     only the folder's holds go into it when both were made under one translator (`package_oot.py keep_pack_entries`).
   `--all-shaders` and `--shaders-later` are still taken and change nothing.
6. **Package** the folder (`tools/package_oot.py`): `Rekarina.exe`, its libraries, the caches and `rekarina.ini`
   (a `totk.ini` left by an earlier package is renamed to it). A copy and nothing else: the game's records go back
   only where the build folder's copy is newer (what the game wrote since stays), the driver's `nv-cache` only into a
   folder without one, and a file that is there already is not copied again. Until 1.2.7 the step then ran
   `Rekarina.exe --warm-shaders` and `tools/oot_prepare_textures.py` while the player waited (20 minutes to 1.5 hours
   and about 43 minutes on some PCs); now it writes `build-cache/background.json` with that work, each part keyed by
   what it follows from (`plan_background`).

**Shortcuts to the launcher.** `Rekarina.exe` in the game folder is the game alone, and players looking there for the
launcher (to update) did not find it: it lives in `<home>\setup\<version>\Rekarina-Launcher.exe`
(`%LOCALAPPDATA%\Rekarina\setup\<version>` for a 1.2.0 install). Every Windows run that leaves a game in the folder
writes "Rekarina Launcher" into the game folder, "Rekarina" into the player's own Start menu (no administrator) and,
unless the form's **Desktop shortcut** row says No (`--no-desktop-shortcut`), "Rekarina Launcher" on the desktop
(`installer/oot.py` `launcher_shortcuts`). They point at the launcher of the setup that ran, in its own folder, with
its icon, and are written again at every run, so a setup downloaded anew moves them to its launcher. The shell writes
them (WScript.Shell through PowerShell, the paths passed in its environment); one that cannot be written is logged
and costs nothing else. Linux gets none (its setup has no launcher executable). The launcher also checks them at
every start (`installer/oot.py` `ensure_shortcuts`): the game folder's and the Start menu's are written when missing,
when their launcher is gone, or when it is older than the one starting (a newer setup downloaded and opened on its
home screen, a setup run cancelled after an update); one to the same or a newer launcher stays, and the desktop's is
only followed, never made there. The launcher's own update swaps the release inside its folder, so the shortcuts'
path already names the new launcher. A setup unpacked from the zip release lies anywhere: the uninstaller finds it
through these shortcuts.

**Uninstalling.** `installer/uninstall.ps1` removes everything Rekarina put on the PC, for a clean reinstall, and is
the one implementation for every way in: the launcher's **Uninstall Rekarina** plate (home screen; its page lists the
folders with their sizes and the space freed, from the script's `-List`), the "Uninstall Rekarina" shortcut in the game
folder, and the per-user "Apps & features" entry (`HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall\Rekarina`,
no administrator). Every setup run that leaves a game in the folder copies the script there as
`rekarina-uninstall.ps1`, writes that shortcut and the entry (`installer/uninstall.py install`, from
`launcher_shortcuts`). What goes: the game folder, the `<home>\setup\<version>` folders with their working files, the
launcher's files in `%LOCALAPPDATA%\Rekarina` and `%APPDATA%\Rekarina`, the Start menu and desktop shortcuts that point
into those folders, and the entry. Each folder only when it carries our marks (`rekarina-install.json` with
`Rekarina.exe` or `rekarina.ini` for a game folder; `unpacked.txt`, or `Rekarina-Launcher.exe` with `installer\oot.py`,
for a setup folder); a drive's root and the system's and the user's own folders never; a dump or keys file
(`.nup .nsp .xci .nsz .xcz .keys`) stays wherever it lies, and a link or junction goes as a link, never what it points to.
The player chooses with two buttons: **Keep my saves** (the default, with the cursor) leaves the game folder's
`profile\` where it is, so a new install into the same folder finds it; **Delete everything** asks a second time,
naming that folder and saying it cannot be undone, and stops there at a no. From the launcher the script runs in a
PowerShell of its own once the launcher's process has ended (`-WaitPid`), so it can remove the launcher's folder too.
`python tests/uninstall_test.py` runs it on scratch installs (application data, Start menu, desktop and registry key
of the test's own).

**What the first start prepares, and the launcher before it.** Play never waits on anything that was not prepared:
the game's entry step (`runtime/vk_pipeline.c` `vk_pipeline_prewarm`, with its progress picture, closable and resumed
at the next start) builds the pipelines play recorded, then every prepared variant's stage library the driver does not
have yet (`shader-warm-done.bin`: a bit per variant of `shader-prepared.bin`, keyed by the device, the driver, its cache
identity, the library state, the exe's path and the list), and beside them walks the dump's texture archives into the
texture cache (`runtime/tex_prepare.c`, done once `tex-cache/prepared.txt` names this dump). The walk runs in a child
process of the game's exe (`--scan-textures`, `prep.c` `prep_child`), whose count lines the step reads: done inside
the game's process on a cold first start it left 11.6 GiB of buffers and 31.5 GiB of the driver's memory there, and
play failed for want of memory. The child ends when its input closes, so closing the game during the step leaves none
behind; it writes its own log (`logs/rekarina-scan-textures.log`, in the problem report), and only
`REKARINA_WARM_STOP` gives it a stop file (Play leaves the launcher's behind). The libraries are
built in the game's own process: the drivers give each process a disk cache file of its own (NVIDIA's `GLCache`: the
first process takes the folder's file, one started while it runs gets a new empty one, and none reads another's;
AMD's `VkCache` keeps `_1` files the same way), so a libraries child of the running game built every library into a
file the game never read; and their memory is small (the driver's footprint did not move over 48,000 of them in the
game's process). The picture counts all
three and gives one time left (`prep.c` `prep_progress_left`: each part at its own measured pace; the pipelines and the
libraries one after the other, the textures beside them), and the log has a line every 30 s with the parts' counts. It
takes long the first time and after a driver update: about half an hour on 16 threads with a cold driver cache.
The launcher gives it a head start: from the setup's finish screen on (which says how long the first start would take),
while its window is open and no game runs, it runs `python -m installer.oot --dest <game folder> --background`
(`background_main`) as a child at below-normal priority on all cores but one (`installer/shaderjob.py`, a row with a
count, a time left and Pause on the home sheet): `Rekarina.exe --warm-libraries` (the entry step without its pipelines:
the libraries alone, built as the game's start builds them, into the driver's disk cache, `pipeline-cache.bin` neither
read nor written), then `Rekarina.exe --scan-textures`. Play writes
`build-cache/background.stop` (`stop_background`): the build ends after the libraries in the driver's hands
(`REKARINA_WARM_STOP`, exit code 3), the texture walk after its archive. The launcher's window stays responsive
meanwhile and the game starts only once the job has ended (`ShaderJob.stop_begin`, `stop_poll`): a build killed while
it writes the driver's cache may leave that file damaged (a first start after such a kill crashed in the driver's
compiler), and one still alive holds it, so the game would get an empty file of its own. After
`BACKGROUND_STOP_WAIT` (20 s; a cold mesh library takes up to 15) the job is stopped the hard way: its tree killed,
its journal removed, and every process of it gone before the game starts. Closing the launcher stops the job within
two seconds the hard way. The stop file goes once the job has. Whatever was done counts for the entry step, which does the rest. 1.2.1 to 1.2.3 ran
the setup's own shader step this way for the rarer variants.

The RomFS is not copied: `rekarina.ini` names the NUP and the game reads it in place, so the dump must stay where it is.
A second run on the same folder continues or updates it under the rule of `installer/runner.py`; the dump is asked
for only on the first run. Upscaler libraries (DLSS, FSR 4.1 and 3.1, frame generation) are the player's own: `--dlls` names
a folder that holds them and the package step puts them beside the game.

A checkout has no compiler of its own: `--toolchain` (or `REKARINA_TOOLCHAIN`) names a folder with `llvm/` and
`bin/ninja`, such as an unpacked TotK-Setup release; without it the newest `TotK-Setup-*` release beside the
checkout's parent folder is taken. `python -m installer.gui_oot --selftest --shots <dir>` draws every screen in
English and Russian with sample events.

The same window is the launcher. It remembers the dump and install folders of the last run (`launcher.json` in the
user's application data); when the install folder holds `Rekarina.exe` it opens on a home screen with **Play**, the
Mods page ([mods.md](mods.md), "The launcher's side"), the Saves page (below), the settings file (`rekarina.ini`
in the player's editor), the game's folder and the way back to the form for an update.

**Play** first looks at what would keep the game from starting (`gui_oot.py` `game_problem`), and the home screen says
it in place of its usual line:

- The dump is not where `rekarina.ini`'s `romfs` line says (moved, renamed, a disk not there; `oot.dump_problem`):
  Play asks for the `.nup` where it is now and runs the setup with it. Only the check step works: it checks the dump
  and writes its path into `rekarina.ini` (`oot.set_ini_dump`, every other line kept); the other steps' keys do not
  hold the path, so nothing is built again. Any setup run with another dump folder does the same. The game itself, when
  it cannot open the dump, says so in a message box that names the file and this way out (`runtime/romfs.c`
  `dump_missing`), not the crash box.
- OneDrive keeps the dump online only (a cloud file whose data is not on the PC): Play asks before it starts the game,
  and the text says how to keep the file on the device.
- `Rekarina.exe` in the game folder is the downloaded one-file launcher (`oot.is_single_launcher`: its `RKSFX001`
  trailer), saved over the game: Play then opened the launcher again. Play offers to put the game back: a setup run
  whose package step copies the game from its build again, after `tools/package_oot.py` has renamed the download to
  `Rekarina-download.exe`. The one-file launcher itself, started as `Rekarina.exe` in a folder that holds
  `rekarina-install.json`, says so and opens the launcher on that folder.

After Play the window hides and watches the game for a few seconds (`watch_game`): a game that ends in that time with
an error brings it back, with the log's last FATAL line (or the exit code) on the home screen. A game that runs on, or
ends without an error, ends the launcher.

A small exe starts it without a console: `installer/launcher/rekarina.c` (icon `installer/launcher/rekarina.rc`), which
looks for `python\pythonw.exe` beside itself (a release folder) and for `.venv` and `src` otherwise (a checkout, the exe
lying beside them). The icon is drawn by `tools/make_rekarina_icon.py` and the window takes its colours from it.

### The Saves page: a save from an emulator or another port

A player who played the game elsewhere continues from that save. `installer/saves.py` looks in the places an
emulator or another port keeps it and the page lists what it finds, newest first, with the slots it holds and when it
was last written; **Import** copies the chosen one into the game folder, **Choose folder** and **Choose .zip** take a
save from anywhere else (a save manager's export, a copy from a console; a folder may hold the save some levels
down). The source is never changed.

- What a save is: a folder with at least one `slot_NN` folder holding `progress.sav` (beside them `account.sav`,
  `option.sav` and each slot's `caption.sav`). That is checked for everything a place yields, so a place is only a
  place to look. A save over 64 MiB or 400 files is refused.
- Places: the other ports' `user/save/<account>/` under their install folder (the default one under the home folder,
  and any folder a `.json` of their builder's or launcher's settings names); Ryujinx and its forks
  (`bis/user/save/<number>/0`); the yuzu family (`nand/user/save/0000000000000000/<account>/<title id>`), each under
  the roaming and local application data on Windows and under the config, data and Flatpak folders on Linux. Seen
  with a real install on the development machine: the two other ports' layout. The emulator layouts are the ones
  those emulators document for Switch games; no emulator runs this game yet, so they were tested with made-up
  folders only.
- Where it goes: `profile/save/<account>/` of the game folder, the account's name read from `profile/account.uid` as
  the game prints it (a game folder that never ran gets a new account, as the game makes one at its first start).
  The save does not name its account inside: one made under another account loads (checked with a save of another
  port in a new game folder).
- What it replaces is kept: `profile/save/<account>` and `profile/save-tx` (which may hold a newer commit of the
  running game, and which the game would otherwise load in the imported save's place) move to
  `profile/save-before-import/<time>/`. Replacing asks once (Import becomes Replace), and **Restore previous** swaps
  the newest such folder back, keeping the imported save in its place.
- A running game holds its save in memory and writes it back: on Windows the import is refused while the game's exe
  is in use.

`python -m unittest installer.saves_test` runs it on made-up folders.

The setup folder for other machines is made by `tools/make_rekarina_release.py --from <an unpacked TotK-Setup release>
--out <folder>`: that release's bundled Python 3.14 and llvm-mingw, this tree's runtime sources, tools, lifter and
installer, the translator, the dependencies' headers and libraries, the execution profile and
`Rekarina-Launcher.exe`, about 540 MiB with nothing of the game in it (the content check runs over it). On Python 3.14
the tools read zstd through the standard library (`tools/py314/zstandard.py` stands in for the zstandard package, also
for the vendored NUP reader; the prompt overlays come out byte for byte the same). One part is not in the folder: the
`zbic` package that unpacks the game's main module is GPL software, so the check step downloads its wheel for the
running Python from the Python Package Index (`win_amd64`, on x86-64 Linux the `manylinux` one) and verifies it against
the index's SHA-256 (`installer/oot.py` `ensure_zbic`); the first setup on a machine needs the internet for that.
When Python's certificate check fails (an antivirus or proxy that scans HTTPS, a chain Windows completes and
OpenSSL does not), the index and the wheel come through Windows' own TLS (`installer/net.py` `fetch_bytes`:
PowerShell's `Invoke-WebRequest`), the checksum checked as before; the setup log says which way it went. The shader
preparation runs under the game's own file name; the driver's build of the prepared shaders happens in the game folder
(the entry step, the launcher's background work), because NVIDIA's driver names its disk cache after the exe's file name
and AMD's keys it to the exe's path (docs/oot-notes.md, 2026-11-11 morning). The translation reports its count
(`gfx_shader: prepare: N of M pairs`) so the window's bar moves through it, and every estimate follows the measured
pace of the phase that runs (`installer/eta.py`).

The Linux setup folder comes from the same script with a Linux toolchain folder as `--from`:

```sh
.venv/bin/python tools/make_release_linux.py --oot --out build/release/Rekarina-toolchain-linux
.venv/bin/python tools/make_rekarina_release.py --from build/release/Rekarina-toolchain-linux \
    --out build/release/Rekarina-Setup-linux [--appimage build/release/Rekarina-x86_64.AppImage]
```

`--oot` builds the "Linux release" folder above without the TotK parts that need the `refs/` checkouts (the shader list
and its licence) and without the TotK setup's bundle check; the runtime library in it is built from `generated/oot`
(`tools/make_release.py` `lifted_version`), and its deps carry a static dav1d for the AV1 movies. The Rekarina folder
takes that folder's `python/`, `llvm/` with `llvm/sysroot`, `bin/` (ninja, the translator as `rekarina_xlate`,
`libSDL3.so.0`), `runtime/deps` and `licenses/`, adds this tree's runtime, lifter, tools and installer, and starts the
launcher from `AppRun` (`python3 -m installer.gui_oot`, with `rekarina.desktop` and `rekarina.png`), about 1.6 GiB. On
Linux the game is `Rekarina` with `libSDL3.so.0` beside it: `installer/oot.py` passes the sysroot to
`tools/build_game.py` and the library to `tools/package_oot.py --lib`, and copies it beside the build for the shader
preparation. That SDL3 is the sniper SDK's, which links ALSA, PulseAudio, the X screensaver extension and libdecor
(`libasound.so.2`, `libpulse.so.0`, `libXss.so.1`, `libdecor-0.so.0`): SteamOS and desktop distributions have them, a
minimal system (WSL's Ubuntu) does not, and the game then stops at start with exit code 127. An SDL3 of our own that
loads those at run time would remove the dependency (not done).
