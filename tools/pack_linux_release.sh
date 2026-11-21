#!/bin/sh
# Packs the Linux setup folder (tools/make_rekarina_release.py --from <Linux toolchain folder>) as a tar.gz beside it and
# writes SHA256SUMS for the tar.gz and every AppImage in the same directory.
#
#     tools/pack_linux_release.sh <setup folder> [<output directory>]
#
# The output directory defaults to the setup folder's parent. The archive is Rekarina-Setup-<VERSION>-linux-x86_64.tar.gz
# with one top folder, Rekarina-Setup/, so it unpacks into a folder of its own; start it with ./AppRun. Nothing of a game
# is in the folder (tools/check_content.py runs over it when make_rekarina_release.py makes it, and again here).
set -eu

[ $# -ge 1 ] || { sed -n 2,9p "$0" | sed 's/^# \{0,1\}//'; exit 2; }
src=$(cd "$1" && pwd)
out=$(cd "${2:-$src/..}" && pwd)
[ -f "$src/VERSION" ] && [ -x "$src/AppRun" ] || { echo "pack_linux_release: $src is not a Linux setup folder (no VERSION or AppRun)" >&2; exit 1; }
version=$(tr -d ' \n' < "$src/VERSION")
root=$(cd "$(dirname "$0")/.." && pwd)

python3 - "$root" "$src" <<'PY'
import sys
from pathlib import Path
sys.path.insert(0, sys.argv[1] + "/tools")
import check_content
bad = [f"{f}: {p}" for f in Path(sys.argv[2]).rglob("*") if f.is_file() and not f.is_symlink()
       for p in check_content.problems_for(f, allow_large=True)]
print("\n".join(bad))
sys.exit(1 if bad else 0)
PY

name="Rekarina-Setup-$version-linux-x86_64.tar.gz"
# A fixed owner and a sorted member list: the same folder gives the same archive.
tar --sort=name --owner=0 --group=0 --numeric-owner --mtime='2000-01-01 00:00:00' \
    --transform "s,^\./,Rekarina-Setup/," -C "$src" -cf - . | gzip -n -9 > "$out/$name"
echo "pack_linux_release: $out/$name ($(du -h "$out/$name" | cut -f1))"

(cd "$out" && sha256sum "$name" ./*.AppImage 2>/dev/null | sed 's,  \./,  ,' > SHA256SUMS)
echo "pack_linux_release: $out/SHA256SUMS"
cat "$out/SHA256SUMS"
