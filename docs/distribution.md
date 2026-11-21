# Distribution: the site, releases and updates

What a release consists of, where its files are served from, how the launcher finds and trusts an update, and the
steps of publishing one. The launcher's side is `installer/update.py`; the files are made by
`tools/make_rekarina_release.py` and signed by `tools/release_sign.py`.

## What is distributed

Only the recompiler and its launcher. No game file is in a release, none is downloaded, and none is uploaded: the
player points the launcher at their own dump and the setup builds the game from it on their machine.
`tools/check_content.py` runs over the release folder when it is made and fails the build on a hit. NVIDIA's and
AMD's libraries (DLSS, Streamline, FidelityFX) are not ours to hand out either: the setup's components step
downloads them from their publishers (`installer/components.py`).

A release is five files:

| File | What it is |
| --- | --- |
| `Rekarina.exe` | The whole launcher in one file, the download for a new player. At its first start it asks for a folder (Windows' own folder dialog; a folder that holds other things gets a `Rekarina` folder inside it) and keeps everything under it: the setup folder in `<folder>/setup/<version>`, the working files of a build in that folder's `temp`, and `<folder>/game` as the game folder the launcher offers. The choice is kept in `%LOCALAPPDATA%/Rekarina/home.txt`. An install made before the choice existed stays in `%LOCALAPPDATA%/Rekarina`; `--choose` asks again, `--home <folder>` names the folder without a question (`installer/launcher/rekarina_single.c` `find_home`). It unpacks its zip by itself (`unzip_builtin`: every file's CRC-32 checked, paths of any length, the setup in about 2 s) and, where that fails, with Windows' `tar.exe` (which cannot enter a folder whose path has letters outside the system's code page); `unpack.log` in the setup folder says which way. |
| `Rekarina-Setup-<version>-windows.zip` | The setup folder as an archive: what the launcher downloads for an update, and a portable install for whoever prefers a folder. |
| `SHA256SUMS` | The SHA-256 of the two files above, one line each. |
| `SHA256SUMS.sig` | The project key's Ed25519 signature over the bytes of `SHA256SUMS`, as 128 hex digits. |
| `latest.json` | The release record the launcher reads from the site: version, notes, and the address, size and SHA-256 of each file. Its shape is GitHub's release record, so the launcher reads both with one reader. |

## The site's layout

The site is `https://rekarina.dev`: static files served by Cloudflare (Workers static assets), kept in
[website/](../website) and deployed with `npx wrangler deploy` from that folder. Nothing on it computes anything.
Its files stop at 25 MiB each, and a release's two programs are about 150 MB each, so they live on a second host,
`https://dl.rekarina.dev`: a Cloudflare R2 bucket (`rekarina-releases`) with that domain attached.

    https://rekarina.dev/releases/latest.json                         the newest release's record
    https://rekarina.dev/download                                     a redirect to the newest Rekarina.exe
    https://rekarina.dev/releases/                                    every version: date, notes, files, SHA-256
    https://rekarina.dev/releases/releases.json                       the history that page is made from
    https://dl.rekarina.dev/releases/<version>/Rekarina.exe
    https://dl.rekarina.dev/releases/<version>/Rekarina-Setup-<version>-windows.zip
    https://dl.rekarina.dev/releases/<version>/SHA256SUMS
    https://dl.rekarina.dev/releases/<version>/SHA256SUMS.sig

`https://dl.rekarina.dev/releases` is what `--site-url` names: the record lists each file at its address there, and
the launcher downloads from wherever the record points. The download button of the front page links
`https://rekarina.dev/download`, so other pages can link the download without naming a version; the page also shows
the SHA-256 of that `Rekarina.exe`. Until the first release is published the record is absent (the launcher then asks
GitHub) and the page says the release is on its way.

Serving rules:

- HTTPS only. The launcher sends one header about itself, `User-Agent: Rekarina-Launcher/<version>`, and nothing
  about the player or the machine: the site needs no cookies, accounts or query strings for it, and its logs should
  not be kept longer than running it needs.
- `latest.json` is cached for minutes at most (`Cache-Control: max-age=300`); everything under `<version>/` never
  changes and can be cached for good by a CDN in front of the site.
- Upload a release's folder first and `latest.json` last, so the record never names a file that is not there yet.
- Old versions stay where they are: a launcher that read the record a few hours ago (it reuses an answer for six)
  may still ask for them.

The same five files, minus `latest.json`, are the assets of a GitHub release (tag `v<version>`), which keeps working
as a second source: the launcher asks the site first and GitHub when the site does not answer.

## How the launcher finds an update

`update.check()` asks at most once in six hours and when the player presses the button.

1. With a feed (`FEED` in `installer/update.py`, or `REKARINA_UPDATE_FEED` for a test or a mirror): GET
   `latest.json`. A site that is down, a 404 or an answer that is not a release record is not an error: GitHub is
   asked next.
2. GitHub: `https://api.github.com/repos/<REPO>/releases/latest`. Without an account GitHub allows 60 such requests
   an hour per address, which is one more reason to serve the record from the site.
3. The answer goes to `update.json` in the application data. The game reads two keys of it for its "a newer version
   is out" notice; when the file is older than six hours the game asks GitHub itself (`runtime/update_note.c`,
   Windows only). With releases on the site alone, the notice therefore comes from the launcher's check, which runs
   every time the launcher opens.

`FEED` is `https://rekarina.dev/releases/latest.json`. Until the site serves that file the request fails and GitHub
answers, so a launcher built before the site is up needs no change afterwards.

## Who vouches for a download

Without a key, an archive is trusted when its SHA-256 is the one GitHub lists for the asset, or its line in
`SHA256SUMS`: that protects against a damaged download, and against nothing else, because whoever can replace the
archive on the server can replace the checksum beside it.

With the project's public key in `SIGNING_KEYS` (`installer/update.py`), an update is installed only when

- the release has `SHA256SUMS` and `SHA256SUMS.sig`,
- the signature is that key's over the exact bytes of `SHA256SUMS` (`installer/ed25519.py`, RFC 8032), and
- the archive's SHA-256 is its line in that file.

A site, a CDN or a release page that somebody else got to write to can then at worst withhold an update. An older
signed release cannot be passed off as an update either: the launcher only offers a version newer than its own, and
refuses an archive whose `VERSION` is not the one the record named.

The key:

    python tools/release_sign.py keygen <a file outside every repository>

prints the public key; put it into `SIGNING_KEYS`. The file it wrote is the private half: whoever has it can publish
updates every installed launcher accepts, so it stays on the maintainer's machine (and one offline copy). Make a
second key at the same time, put its public half into `SIGNING_KEYS` as well and keep its private half offline: if
the first is lost or leaked, a release signed with the second still reaches every player and can bring a launcher
with new keys. The first release whose launcher carries a key must itself be offered unsigned-compatible, which it
is: launchers without a key ignore the signature file.

Windows itself is a separate matter: an exe without an Authenticode signature gets the SmartScreen "unknown
publisher" page until enough people have run it. A code-signing certificate removes that; it costs money and needs
an identity check, so it is the maintainer's decision. Until then the site's download page should show the SHA-256
of `Rekarina.exe` and say what the SmartScreen page is.

## Publishing a release

1. Set `VERSION`, build the translator, the runtime's shim and the PGO profile as for any release
   ([building.md](building.md)).
2. Make the files:

       python tools/make_rekarina_release.py --from <TotK-Setup release> --out <setup folder> ^
           --single <dist>/Rekarina.exe --dist <dist> ^
           --site-url https://dl.rekarina.dev/releases --notes <notes.txt> --sign <private key file>

   `<dist>` then holds the five files; the content check must say "no hit". Two more checks keep players' setups
   short. `--previous <the release before's setup folder>` runs `tools/update_cost.py`: an update that would run the
   lift or the shader translation again fails unless `--allow-long` names it. `--timing <before.json> <now.json>`
   compares two fresh installs timed by `tools/setup_timing.py <setup folder> --game <dump> --dest <empty folder>
   --json <out>` (every step from nothing, 30 to 60 minutes of every core): a step that takes more than a quarter and a
   minute longer than in the release before fails unless `--allow-slower` names it.
3. Check the signature against the keys the launcher carries: `python tools/release_sign.py verify <dist>/SHA256SUMS`.
4. Start `<dist>/Rekarina.exe` on a machine without Rekarina and build the game once.
5. Publish: `python website/publish_release.py <dist> --dry-run`, then without `--dry-run` and with `--deploy`. It
   refuses a folder that is not a signed release of the project's keys, uploads the four files to the bucket, reads
   them back from `dl.rekarina.dev`, and only then writes the record, the `/download` redirect and the page's
   download block into `website/public`, adds the version to the history `public/releases/releases.json` with the
   day's date, renders the releases page `public/releases/index.html` from it (newest first, notes from their
   Markdown) and deploys the site. A version that is already on the download host is not replaced: publish a new
   version instead. The history never replaces a version's record either: the same files again change nothing, other
   files under a version it holds are refused before anything is uploaded. A release published without the history
   is added with `--history-only --date <YYYY-MM-DD>`, which reads its files back from the host and touches nothing
   else; `python tests/publish_release_test.py` covers the notes' Markdown, the history and the page. A run that stopped after the upload goes on from there when run again:
   the host's `SHA256SUMS` is the folder's, so the files are read back, not sent again. Every request says
   `User-Agent: Rekarina-Publish`; the site's protection answers urllib's own with 403.
6. Optionally publish the same files as the GitHub release `v<version>` with the notes as its text.
7. From an installed older version: open the launcher, take the update, start the game.

Wrangler needs a login that may deploy the site and write to R2 (`npx wrangler login`; a login narrowed with
`--scopes` to the site's own needs cannot be assumed to reach the bucket), and the bucket and its domain exist once:
`npx wrangler r2 bucket create rekarina-releases`, then `npx wrangler r2 bucket domain add rekarina-releases` for
`dl.rekarina.dev`.

`python -m unittest installer.launcher_test` covers the feed, the fallback to GitHub and every refusal of the
signature check with a local server.

## Source repository

The sources' home is the project's own Forgejo at `https://git.rekarina.dev`, with GitHub as a push mirror; how
the server is set up and run: [deploy/forgejo/README.md](../deploy/forgejo/README.md).

## Community links

The site is `https://rekarina.dev` and the Discord server's permanent invite is `https://discord.gg/6N8PKr9zZA`
(`https://rekarina.dev/discord` redirects to it, so a changed invite needs one line in `website/public/_redirects`).
The launcher's window has no link row yet: where it goes and how it looks is a design question (mockup first).

## Not there yet

- Release channels (a beta beside the stable one): the version reader drops pre-release suffixes, and there is one
  record. A second record (`beta.json`) and a setting would do it.
- Linux: there is no Linux launcher or release; the setup and the update's swap are Windows programs.
- The game's own update request knows GitHub only (see above).
- A mirror list: one feed address, then GitHub.
