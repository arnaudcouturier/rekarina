# Windows launcher releases

Players download `Rekarina.exe` from this fork's [GitHub Releases](https://github.com/arnaudcouturier/rekarina/releases)
and report problems in its [Issues](https://github.com/arnaudcouturier/rekarina/issues). The launcher builds the game
locally from the player's own dump. The original Rekarina repository is the `upstream` remote. Import the changes the
maintainer requests and use the fork's own release version when shipping.

## Release files

| File | Purpose |
| --- | --- |
| `Rekarina.exe` | The Windows launcher for a first install. It unpacks to `<home>\setup\<version>` and offers `<home>\game` as the game folder. |
| `Rekarina-Setup-<version>-windows.zip` | The launcher and toolchain archive the updater installs. Its name and internal `VERSION` must match the release tag. |
| `SHA256SUMS` | SHA-256 checksums for the launcher and setup archive. |

The packager checks the release folder for game content. The launcher fetches NVIDIA and AMD libraries from their
publishers during setup. The release includes the fork's own Streamline shim for DLSS Frame Generation.

## How updates work

The launcher and the in-game notice normally query `api.github.com/repos/arnaudcouturier/rekarina/releases/latest`.
The launcher checks for a newer numeric version, verifies the ZIP against GitHub's asset digest or `SHA256SUMS`, and
checks the archive's version and fork marker. It stages the update for the native launcher to apply on restart.
The install folder retains the game, its saves, and settings. The installer rebuilds the steps whose inputs changed.

On Windows, the launcher remembers its home in `%LOCALAPPDATA%\RekarinaFork\home.txt`. Launcher preferences,
the shared update cache, and setup timings live under `%APPDATA%\RekarinaFork`. The fork has its own Windows
uninstall entry and its Start and desktop shortcuts include `(fork)`. To move from upstream, install into a
separate folder and use the launcher's Saves page to import a save.

## Build and stage a draft

1. Set `VERSION` to the next fork version. Use a numeric version greater than the last release and tag the same
   source as `v<version>`. Build the translator and Streamline shim from the release revision. See the
   [toolchain notes](building.md).
2. Create the Windows artifacts from a clean checkout using an unpacked Windows toolchain release as the base.

   ```powershell
   python tools/make_rekarina_release.py --from <toolchain-folder> --out build/release/setup --single build/release/dist/Rekarina.exe --dist build/release/dist
   ```

   Check that packaging succeeds and reports `content check: no hit`. For later releases, pass `--previous`
   with the previous fork setup to review the update cost.
3. Run `python -m unittest installer.launcher_test` and `python tests/uninstall_test.py` on Windows. Start the
   one-file launcher in a scratch install and verify the game build. Serve the actual ZIP and checksums from a
   local release fixture to test update discovery, staging and restart from an older setup. Verify that saved data
   and settings survive and that upstream registrations and state are unaffected.
4. Create a draft on GitHub targeting the exact commit used for packaging. Attach the three files and review the
   release notes and asset names. Publish the draft when the maintainer approves it.
5. After publication, verify that the public Releases API returns the new version and that an older fork launcher
   offers it. Use a new version for corrections to a published package.

The launcher also supports a feed override for local tests and optional signed checksums through
`tools/release_sign.py` and `SIGNING_KEYS` in `installer/update.py`.
