# Rekarina

Static recompilation of a Switch 2 game (ARM64) into a native Windows x86-64 exe with a Vulkan renderer. The code and the notes call the game `OoT` (`GAME_OOT`; `GAME_NS2` is what any Switch 2 game needs). It is built on the TotK Recomp lifter and runtime, and the same tree still builds that project's Switch game, `TotK`. Design: [docs/architecture.md](docs/architecture.md) and, for what OoT adds, [docs/oot-architecture.md](docs/oot-architecture.md). Toolchain, build commands and tests: [docs/building.md](docs/building.md). Working notes on OoT: [docs/oot-notes.md](docs/oot-notes.md). The launcher and the release flow: [docs/installer.md](docs/installer.md), [docs/distribution.md](docs/distribution.md).

## No game is named

- No game, franchise, character or place is named anywhere in this repository: not in code, comments, docs, issue
  forms, release notes or commit messages. The two games are `OoT` and `TotK`, the abbreviations the code uses; a page a
  player reads says "the game". Places are plain words ("the forest", "the field", "the Market", "the village"), the
  player's character is "the hero". The game's own identifiers that the code must match (map and event names in
  `runtime/oot_warp.c`) are the one exception.
- No pictures taken from a game: no screenshots, no frames of a video, no textures. Pictures of the launcher and charts
  are fine.
- The public history is one commit per release of the sources; the working history stays in the local repository.

## Names

- The player sees Rekarina's names: `Rekarina.exe`, `rekarina.ini`, `logs\rekarina.log`, `rekarina_xlate.exe` with its `.id`, `rekarina_dlssg.dll` (`runtime/host.h` `RT_FILE_STEM`; `tools/package_oot.py` and `tools/make_rekarina_release.py` name the files). A folder made before those names existed keeps working: the game moves `totk.ini` over and still takes the old translator and library names.
- Build outputs (`build/<ver>/totk.exe`, `build/xlate/totk_xlate.exe`), the `TOTK_` prefix of environment variables and the TotK installer keep the runtime's original names.
- `tools/build_xlate.py`, `tools/xlate/Program.cs`, the `.csproj` and `tools/xlate/patches/` make up the translator identity. A change to any of them, a rename included, makes every player's shader cache stale: change them only when the translation itself has to change.

## Game content boundary

This repository is public. Game data lives outside it and every derived file stays in ignored folders.

- The user's game and key paths are in `config.local.toml` (ignored). Pass the keys file path to tools; the tools read it themselves. Key values stay out of your output, logs, commands and files.
- Everything extracted or generated goes under `build/` or `generated/`.
- Every file the code generator emits carries the marker defined as `GENERATED_MARKER` in `tools/check_content.py`.
- Run `python tools/check_content.py --staged` before proposing a commit. It is the same check CI runs.

## Reusing code

The project is under the GNU General Public License, version 3 (`LICENSE`); the recompiler it is built on is MIT, and
its notice is kept in `THIRD_PARTY_NOTICES.md`.

`refs/` holds the upstream checkouts (`tools/fetch_refs.sh` recreates it). Read-only.

- Copy only MIT, BSD, ISC, zlib or Apache-2.0 code, keep its header, and add a row to `THIRD_PARTY_NOTICES.md`.
- yuzu, suyu, OpenSmash-Ultimate and other GPL code: read for behaviour, write our own.
- `tools/vendor/` stays byte-identical to upstream; Windows changes go in `tools/extract.py`.

## Code

- Build C with MSYS2 UCRT64 Clang (`C:\msys64\ucrt64\bin`). Generated C uses GNU extensions, so `cl.exe` is out. Call Ninja from that directory by full path or put it first on PATH.
- Two Python environments: `.venv` (3.12) for the lifter and the build, `.venv-py314` for the extraction and shader scripts, which need `compression.zstd`.
- Generated C is never edited by hand: change the generator (`recomp/`).
- Every lifter change passes the differential tests against Unicorn ([docs/building.md](docs/building.md), "Checks"). After changing `recomp/jumptables.py`, `tests\jumptables_check.py` reports 0 disagreements on 1.2.1 and 1.4.3.
- `recomp/layout.py` alone decides module bases.
- Graphics bugs: [docs/maxwell.md](docs/maxwell.md) lists the Maxwell behaviour the Vulkan path must match and the TotK bugs other ports fixed.
- Comments say what the code does and why, in words that stand on their own. Cite our code by file plus function (a line number may follow as a hint) and upstream code by project, file and function: `refs/` moves, so its line numbers rot. Commit hashes belong to the attribution of copied code.

## Writing

Code, comments, docs and commit messages are in English. Commit messages are plain sentences without type prefixes.
