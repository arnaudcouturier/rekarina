"""Build the shader translator: build/xlate/totk_xlate.exe (Ryujinx 1.3.2 + patches, NativeAOT, SPIR-V for Vulkan).

Copies tools/vendor/ryujinx to build/xlate/ryujinx, applies tools/xlate/patches/*.patch there (tools/vendor stays
byte-identical; 0001-0004 are MNG's, 0005-0013, 0015 and 0016 are ours; 0017-0020, the Ampere / NVN2 frontend for
Switch 2 games, are MNG's and seedds/nxnative's, 0021-0046 ours), then runs `dotnet publish`. Needs the .NET 10 SDK, the MSVC build
tools and vswhere.exe (docs/building.md). Run with .venv. On Linux it builds build/xlate/totk_xlate for linux-x64, which
NativeAOT links with the system's clang (no MSVC); NativeAOT builds for the OS it runs on only.

Without MSVC (no vswhere.exe, or vswhere finds no installation with the x64 C++ tools) a Windows build falls back to a
JIT build: the same exe name, self-contained (the .NET runtime is inside it, nothing else is installed on the player's
machine), one file, ReadyToRun-compiled and trimmed like the NativeAOT one. --jit asks for it with MSVC present. It
translates the same; it starts slower and is larger.

The SDK: the dotnet named by DOTNET (an exe path), else one under DOTNET_ROOT, else the per-user install of
dotnet-install.ps1 (%LOCALAPPDATA%\\Microsoft\\dotnet), else the one on PATH; the first that lists a 10.x SDK is used
(C:\\Program Files\\dotnet may hold only runtimes).

Next to the exe it writes totk_xlate.id: the translator identity, a sha256 over everything the build reads (this script,
Program.cs, the csproj, the patches, the vendor tree). runtime/gfx_shader.c puts it into every shader cache key, so a
translator change re-keys the cache without a hand-made KEY_VERSION bump. --id-only writes it for the exe already built
(the caller vouches that the exe matches the sources). Both build kinds get the same identity: they translate alike.
SAME_AS below lets a change that touches a handful of shaders keep the earlier identity, and with it every player's
prepared shader cache.

  python tools/build_xlate.py [--id-only] [--out DIR] [--jit]

--out builds into DIR instead of build/xlate (a task build dir: totk.exe takes the translator next to it).
"""

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VENDOR = ROOT / "tools" / "vendor" / "ryujinx"
MNG_VENDOR = ROOT / "refs" / "MacNativeGames" / "vendor" / "ryujinx"
XLATE = ROOT / "tools" / "xlate"
BUILD = ROOT / "build" / "xlate"
WIN = os.name == "nt"
EXE = "totk_xlate.exe" if WIN else "totk_xlate"
VSWHERE_DIR = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Microsoft Visual Studio" / "Installer"
PATCH_COUNT = 47   # tools/xlate/patches/*.patch: a missing or stray file stops the build


def find_dotnet() -> str | None:
    """The first dotnet that lists a .NET 10 SDK (see the module docstring for the order)."""
    exe = "dotnet.exe" if WIN else "dotnet"
    candidates = []
    if os.environ.get("DOTNET"):
        candidates.append(os.environ["DOTNET"])
    if os.environ.get("DOTNET_ROOT"):
        candidates.append(str(Path(os.environ["DOTNET_ROOT"]) / exe))
    if WIN and os.environ.get("LOCALAPPDATA"):
        candidates.append(str(Path(os.environ["LOCALAPPDATA"]) / "Microsoft" / "dotnet" / exe))
    elif not WIN:
        candidates.append(str(Path.home() / ".dotnet" / exe))
    if shutil.which("dotnet"):
        candidates.append(shutil.which("dotnet"))
    for c in candidates:
        if not Path(c).is_file():
            continue
        r = subprocess.run([c, "--list-sdks"], capture_output=True, text=True)
        if r.returncode == 0 and any(line.startswith("10.") for line in r.stdout.splitlines()):
            return c
    return None


def find_vswhere() -> Path | None:
    if (VSWHERE_DIR / "vswhere.exe").is_file():
        return VSWHERE_DIR / "vswhere.exe"
    found = shutil.which("vswhere")
    return Path(found) if found else None


def msvc_present(vswhere: Path | None) -> bool:
    """True when vswhere finds an installation with the x64 C++ tools, which NativeAOT links with on Windows."""
    if vswhere is None:
        return False
    r = subprocess.run([str(vswhere), "-latest", "-products", "*", "-requires",
                        "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"],
                       capture_output=True, text=True)
    return r.returncode == 0 and bool(r.stdout.strip())


def tree_hashes(base: Path) -> dict[str, str]:
    """Hash every source file; src/*/obj and src/*/bin are build output (an IDE restore writes them, the vendor
    .gitignore ignores them and prepare_sources() drops them), so they are left out."""
    def build_output(rel: Path) -> bool:
        return len(rel.parts) > 2 and rel.parts[0] == "src" and rel.parts[2] in ("obj", "bin")

    return {
        p.relative_to(base).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
        for p in sorted(base.rglob("*"))
        if p.is_file() and not build_output(p.relative_to(base))
    }


def mng_pin() -> str:
    """The MacNativeGames commit tools/fetch_refs.sh pins."""
    for line in (ROOT / "tools" / "fetch_refs.sh").read_text(encoding="utf-8").splitlines():
        f = line.split()
        if len(f) >= 4 and f[:2] == ["fetch", "MacNativeGames"]:   # a fifth word asks for the history
            return f[3]
    sys.exit("tools/fetch_refs.sh pins no MacNativeGames commit")


def pinned_blobs() -> dict[str, str]:
    """Git blob ids of MNG's vendor/ryujinx at the pinned commit. The checkout itself may be newer than the pin or
    carry CRLF line ends (core.autocrlf), so its files are not compared."""
    out = subprocess.run(["git", "-C", str(MNG_VENDOR.parent.parent), "ls-tree", "-r", mng_pin(), "vendor/ryujinx"],
                         capture_output=True, text=True)
    if out.returncode != 0:
        sys.exit(f"git ls-tree of the MacNativeGames pin failed: {out.stderr.strip()}")
    blobs = {}
    for line in out.stdout.splitlines():
        meta, path = line.split("\t", 1)
        blobs[path.removeprefix("vendor/ryujinx/")] = meta.split()[2]
    return blobs


def blob_ids(base: Path) -> dict[str, str]:
    """Git blob ids of the source files tree_hashes() hashes."""
    ids = {}
    for rel in tree_hashes(base):
        data = (base / rel).read_bytes()
        ids[rel] = hashlib.sha1(b"blob %d\0" % len(data) + data).hexdigest()
    return ids


def verify_vendor() -> None:
    """tools/vendor/ryujinx must be byte-identical to MNG's copy at the pinned commit (when refs/ is present)."""
    if not MNG_VENDOR.is_dir():
        print("refs/MacNativeGames missing: vendor comparison skipped")
        return
    ours, theirs = blob_ids(VENDOR), pinned_blobs()
    if ours != theirs:
        diff = sorted(k for k in ours.keys() | theirs.keys() if ours.get(k) != theirs.get(k))
        sys.exit(f"tools/vendor/ryujinx differs from MNG in {len(diff)} files, first: {diff[:5]}")
    print(f"vendor identical to MNG: {len(ours)} files")


def prepare_sources() -> Path:
    src = BUILD / "ryujinx"
    if BUILD.exists():
        shutil.rmtree(BUILD / "ryujinx", ignore_errors=True)
    shutil.copytree(VENDOR, src)
    for junk in list(src.glob("src/*/obj")) + list(src.glob("src/*/bin")):
        shutil.rmtree(junk)
    patches = sorted((XLATE / "patches").glob("*.patch"))
    if len(patches) != PATCH_COUNT:
        sys.exit(f"expected {PATCH_COUNT} patches, found {len(patches)}")
    for patch in patches:
        # git apply works outside a repository when given a directory; --check first so a failure names the patch.
        # Inside one (build/xlate is in ours) it skips, silently, the files of a "diff --git" patch that fall outside the
        # current directory; -v names them, and any such file stops the build.
        for extra in (["--check"], []):
            r = subprocess.run(
                ["git", "apply", "-v", *extra, "--whitespace=nowarn", "--directory", str(src.relative_to(BUILD)), str(patch)],
                cwd=BUILD, capture_output=True, text=True,
            )
            if r.returncode != 0 or "Skipped patch" in r.stderr:
                sys.exit(f"patch does not apply: {patch.name}\n{r.stdout}{r.stderr}")
        print(f"applied {patch.name}")
    return src


# Sources that translate like earlier ones. Left: what the sources hash to (source_identity); right: the earlier identity
# they keep. Patch 0040 changed the translation of mesh shaders that output points or a point size and of no other
# shader (seven of OoT's), and patch 0041 that of mesh shaders on the devices that ask for whole index
# vectors, and patch 0042 that of programs whose VOTEU writes a uniform predicate other than UPT (OoT's eye
# shaders); patch 0043 adds a translation only a job with "as_compute" asks for (a mesh emulation kernel, its own
# stage GFX_STAGE_MSC in the program hash); patch 0044 changes programs with REDUX, MATCH or a per-lane HMMA, LDSM or
# MOVM only in compute shaders translated for subgroups wider than 32 lanes (AMD GCN); patch 0046 marks DontInline the
# raw global-memory helpers of mesh shaders whose code is 60,000 to 70,000 bytes long (OoT's foliage); patch 0047 changes only
# the mesh emulation kernels (the buffer's address from push constants), which runtime/gfx_shader.c MESHC_REVISION re-keys;
# patch 0048 changes only mesh programs with per-primitive outputs and their emulation kernels; runtime/gfx_shader.c
# build_code_hash gives exactly those a program hash of their own, so every other cache
# entry made under the earlier identity is what this build would write. A change to any source gives another left
# side and the entry stops applying by itself; then it is removed, or made anew with the same care (translate a
# corpus with both builds and compare the files). The sources' comments were reworded since (no game is named in
# them): comment lines only, checked by applying the patches and comparing the patched tree with the one the built
# translator came from.
SAME_AS = {
    "d69568d7529e818425697e9eb3a769888f796dc8fb255090a92e70ee841a50cd":
        "b9d1309569a87e96595514e3e1eccb56e907a0b67a1bac3577ae00acd9c8e726",
    "509b66df57c65317b67426f91979f3c6aed7ed1fb375b5db785c523cf23067ff":
        "b9d1309569a87e96595514e3e1eccb56e907a0b67a1bac3577ae00acd9c8e726",
    "7db37617542e2afade37a4d45c19ba14f3b92fc24c89d22d403709289252c5d1":
        "b9d1309569a87e96595514e3e1eccb56e907a0b67a1bac3577ae00acd9c8e726",
    "b57096b248730a9d5a96a9cba4d295971b0717eb19a487b314ae561b5b799245":
        "b9d1309569a87e96595514e3e1eccb56e907a0b67a1bac3577ae00acd9c8e726",
    "f94d04cd9c4823db5a94b7a2fc0e0e0a21c50b5037202184561551c72d63ec79":
        "b9d1309569a87e96595514e3e1eccb56e907a0b67a1bac3577ae00acd9c8e726",
    "e1873b65346245c72d6a907d32e33fc033cf94e915235b93a7ddc5811d4052a0":
        "b9d1309569a87e96595514e3e1eccb56e907a0b67a1bac3577ae00acd9c8e726",
    "b083493588dc2531023674fbbc3be55e96c960dc28a29938ebdf0072f44c6a82":
        "b9d1309569a87e96595514e3e1eccb56e907a0b67a1bac3577ae00acd9c8e726",
}   # same-as-end


def source_identity() -> str:
    """sha256 over the translator's inputs, in a fixed order: path and content hash of each file."""
    h = hashlib.sha256()
    script = Path(__file__).resolve().read_bytes()
    a, b = script.find(b"SAME_AS = {"), script.find(b"}   # same-as-end")
    if 0 <= a < b:   # the table names this hash, so it cannot be part of it
        script = script[:a] + script[b:]
    h.update(f"{Path(__file__).name}\0{hashlib.sha256(script).hexdigest()}\n".encode())
    files = [XLATE / "Program.cs", XLATE / "totk_xlate.csproj", *sorted((XLATE / "patches").glob("*.patch"))]
    for f in files:
        h.update(f"{f.name}\0{hashlib.sha256(f.read_bytes()).hexdigest()}\n".encode())
    for rel, digest in tree_hashes(VENDOR).items():
        h.update(f"vendor/{rel}\0{digest}\n".encode())
    return h.hexdigest()


def identity() -> str:
    """The translator identity: source_identity, or the earlier identity SAME_AS gives it."""
    ident = source_identity()
    return SAME_AS.get(ident, ident)


def write_identity(exe: Path) -> None:
    ident = identity()
    exe.with_suffix(".id").write_text(ident + "\n", encoding="ascii")
    kept = "" if ident == source_identity() else f" (kept by SAME_AS; the sources hash to {source_identity()[:16]})"
    print(f"translator identity {ident[:16]}{kept} -> {exe.with_suffix('.id')}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--id-only", action="store_true", help="write totk_xlate.id for the exe already built")
    parser.add_argument("--out", type=Path, help="build directory instead of build/xlate")
    parser.add_argument("--llvm", type=Path, help="Linux: an LLVM directory whose bin/clang and lld link the exe")
    parser.add_argument("--sysroot", type=Path, help="Linux: link against this sysroot's glibc instead of the host's "
                                                     "(tools/make_release_linux.py)")
    parser.add_argument("--jit", action="store_true", help="Windows: a self-contained JIT (ReadyToRun) build instead of "
                                                          "NativeAOT; the default when MSVC is missing")
    a = parser.parse_args()
    if a.out:
        global BUILD
        BUILD = a.out.resolve()
    if a.id_only:
        write_identity(BUILD / EXE)
        return
    started = time.time()
    dotnet = find_dotnet()
    if not dotnet:
        sys.exit("missing: dotnet with a .NET 10 SDK (DOTNET, DOTNET_ROOT, %LOCALAPPDATA%\\Microsoft\\dotnet or PATH)")
    print(f"dotnet: {dotnet}")
    env = dict(os.environ)
    # The SDK resolves its runtimes and workloads from DOTNET_ROOT; pointing it at the dotnet that runs keeps a per-user
    # SDK from mixing with C:\Program Files\dotnet.
    env["DOTNET_ROOT"] = str(Path(dotnet).parent)
    jit = a.jit
    if WIN and not jit:
        vswhere = find_vswhere()
        if not msvc_present(vswhere):
            why = "no vswhere.exe" if vswhere is None else "vswhere finds no x64 C++ tools"
            print(f"MSVC missing ({why}): NativeAOT cannot link, building the self-contained JIT exe instead")
            jit = True
        else:
            env["PATH"] = str(vswhere.parent) + os.pathsep + env["PATH"]

    verify_vendor()
    src = prepare_sources()
    work = BUILD / "xlate"
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    for name in ("Program.cs", "totk_xlate.csproj"):
        shutil.copy2(XLATE / name, work / name)
    out = BUILD / "publish"
    shutil.rmtree(out, ignore_errors=True)
    cmd = [
        dotnet, "publish", str(work / "totk_xlate.csproj"), "-c", "Release", "-r", "win-x64" if WIN else "linux-x64",
        "-p:InvariantGlobalization=true", f"-p:RyuSrc={src}", "-o", str(out),
    ]
    if jit:
        # One self-contained exe: the runtime inside it, the IL precompiled (ReadyToRun: crossgen, no C++ linker) and
        # trimmed as NativeAOT trims; the few native libraries of the runtime are bundled too.
        cmd += ["-p:PublishAot=false", "--self-contained", "true", "-p:PublishSingleFile=true",
                "-p:IncludeNativeLibrariesForSelfExtract=true", "-p:PublishReadyToRun=true", "-p:PublishTrimmed=true"]
    else:
        cmd.append("-p:PublishAot=true")
    if a.llvm:
        env["PATH"] = str(a.llvm.resolve() / "bin") + os.pathsep + env["PATH"]
        cmd += ["-p:CppCompilerAndLinker=clang", "-p:LinkerFlavor=lld"]
    if a.sysroot:
        cmd.append(f"-p:SysRoot={a.sysroot.resolve()}")
    if subprocess.run(cmd, env=env).returncode != 0:
        sys.exit("dotnet publish failed")
    exe = BUILD / EXE
    shutil.copy2(out / EXE, exe)
    write_identity(exe)
    kind = "JIT, self-contained, ReadyToRun" if jit else "NativeAOT"
    print(f"built {exe} ({kind}, {exe.stat().st_size / 1e6:.1f} MB) in {time.time() - started:.0f} s")


if __name__ == "__main__":
    main()
