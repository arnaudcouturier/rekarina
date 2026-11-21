#!/bin/sh
# Builds the game on this Linux machine from a dump with the system's own toolchain, with no release folder: the steps
# the setup runs (installer/oot.py), one after the other. For development and for testing a change at once; players use
# the AppImage or the tar.gz setup (docs/installer.md, "Linux release").
#
#     tools/build_linux_local.sh <game.nup> <install folder> [--all-shaders] [--from STEP]
#
# STEP is one of: lift compile xlate prompts shaders package. Work files (extracted code, generated C, objects, the
# translated shaders) stay under build/ and generated/ (ignored); the install folder holds what the game reads. The game
# is built for this machine, linked against the system's libraries; the shader step opens a window and runs the GPU
# for about twenty minutes. The packages (Fedora names; Arch and Debian have the same ones): clang lld ninja make
# pkgconf SDL3-devel opus-devel libvpx-devel libzstd-devel libdav1d-devel vulkan-headers vulkan-loader-devel
# VulkanMemoryAllocator-devel libshaderc-devel spirv-tools-devel libstdc++-static uv python3.12 python3.14 dotnet-sdk-10.0.
set -eu

[ $# -ge 2 ] || { sed -n 2,14p "$0" | sed 's/^# \{0,1\}//'; exit 2; }
nup=$(realpath "$1")
dest=$(mkdir -p "$2" && realpath "$2")
shift 2
shaders=--met-only
from=lift
while [ $# -gt 0 ]; do
    case $1 in
        --all-shaders) shaders= ;;
        --from) from=$2; shift ;;
        *) echo "build_linux_local: unknown option $1" >&2; exit 2 ;;
    esac
    shift
done

cd "$(dirname "$0")/.."
[ -f "$nup" ] || { echo "build_linux_local: $nup is not a file" >&2; exit 1; }
for tool in clang ld.lld ninja uv dotnet git pkg-config; do
    command -v "$tool" >/dev/null || { echo "build_linux_local: $tool is not installed" >&2; exit 1; }
done
dotnet --list-sdks | grep -q '^10\.' || { echo "build_linux_local: the shader translator needs the .NET 10 SDK" >&2; exit 1; }
pkg-config --exists sdl3 opus vpx libzstd dav1d || { echo "build_linux_local: SDL3, Opus, libvpx, zstd or dav1d development files are missing" >&2; exit 1; }
[ "$(uname -r | cut -d. -f1)" -gt 6 ] || [ "$(uname -r | cut -d. -f2)" -ge 7 ] || \
    echo "build_linux_local: warning: Linux 6.7 or later is needed for the write watch (this is $(uname -r))"

# Python environments: .venv runs the lifter and the build, .venv-py314 the extraction and shader scripts.
if [ ! -x .venv/bin/python ]; then
    uv venv --python 3.12 .venv
    uv pip install --python .venv/bin/python -r tools/requirements.txt zbic
fi
if [ ! -x .venv-py314/bin/python ]; then
    uv venv --python 3.14 .venv-py314
    uv pip install --python .venv-py314/bin/python -r tools/requirements-py314.txt zbic
fi
PY=.venv/bin/python
PY14=.venv-py314/bin/python
work=build/oot-work/build
exe=Rekarina

step() { case " lift compile xlate prompts shaders package " in *" $1 "*) ;; *) echo "build_linux_local: no step $1" >&2; exit 2 ;; esac; }
step "$from"
go=0
run() { [ "$go" = 1 ] || [ "$1" = "$from" ] && go=1; [ "$go" = 1 ]; }

if run lift; then
    echo "== lift"
    $PY tools/oot_exefs.py "$nup" --out build/extracted/oot/exefs --plain "$dest/exefs" > build/oot-exefs.json
    $PY recomp/gen.py --exefs build/extracted/oot/exefs --out generated/oot --patches recomp/patches-oot-v111.txt
fi
if run compile; then
    echo "== compile"
    mkdir -p build/oot
    $PY tools/build_game.py --version oot --exefs "$dest/exefs" --no-romfs --gen generated/oot --out build/oot
fi
if run xlate; then
    echo "== translator"
    if [ ! -d refs/MacNativeGames/.git ]; then   # the one upstream tree it needs, at the commit tools/fetch_refs.sh pins
        commit=$(sed -n 's/^fetch MacNativeGames  *\([^ ]*\)  *\([0-9a-f]\{40\}\).*/\1 \2/p' tools/fetch_refs.sh)
        url=${commit% *}; hash=${commit#* }
        mkdir -p refs && git init -q refs/MacNativeGames && git -C refs/MacNativeGames remote add origin "$url"
        git -C refs/MacNativeGames fetch -q --depth 1 origin "$hash"
        git -C refs/MacNativeGames checkout -q FETCH_HEAD
        git -C refs/MacNativeGames update-ref refs/remotes/origin/HEAD "$hash"
    fi
    $PY tools/build_xlate.py
fi
mkdir -p "$work"
if run prompts; then
    echo "== prompts"
    $PY14 tools/make_oot_prompts.py --out "$work" --nup "$nup"
    for style in switch xbox xbox-label; do
        mkdir -p "$work/shared-fonts-$style"
        $PY14 tools/make_ext_font.py "$work/shared-fonts-$style/5-nintendo-extension.bfttf" --style "$style"
    done
fi
if run shaders; then
    echo "== shaders (a window opens; this takes a while)"
    # The preparation and the driver's cache both run under the game's own file name (NVIDIA keys its cache by it).
    cp build/oot/rekarina "$work/$exe"
    cp build/oot/rekarina "$work/rekarina"
    cp build/xlate/totk_xlate build/xlate/totk_xlate.id "$work/"
    $PY14 tools/oot_prepare_shaders.py "$work" --nup "$nup" --exefs "$dest/exefs" --exe-name "$exe" $shaders
    $PY14 tools/pack_shader_cache.py "$work/shader-cache"
fi
if run package; then
    echo "== package"
    cp build/oot/rekarina "$work/rekarina"
    $PY14 tools/package_oot.py --nup "$nup" --build "$work" --exefs "$dest/exefs" --xlate build/xlate/totk_xlate \
        --out "$dest" --prompts auto --lang "${REKARINA_LANG:-en-US}"
    echo "build_linux_local: done. Run it with: cd $dest && ./$exe"
fi
