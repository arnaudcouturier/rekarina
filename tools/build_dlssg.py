"""Build our DLSS Frame Generation library (runtime/dlssg/nx2_sl.cpp) with the project's clang against NVIDIA's
Streamline SDK.

    python tools/build_dlssg.py --sl <Streamline SDK folder> [--toolchain <llvm-mingw folder>] [--out <dll>]

The library sits between the game and Streamline (runtime/vk_dlssg.c loads it as rekarina_dlssg.dll, or by its build
name totk_dlssg.dll); it takes Streamline's functions by name at run time, so only the SDK's headers are needed, and
they stay outside the repository (--sl names the folder that holds include/sl.h; the game ships Streamline 2.14.16,
build against the same release's headers). Streamline's own DLLs are MSVC's: the library passes them structures by
pointer or reference and enums only, which clang lays out as MSVC does on x64 Windows, and nx2_sl.cpp checks the
layouts it passes against the offsets Streamline asserts (a different SDK release that moved them fails to compile).
The C++ runtime is linked in (-static): the DLL needs only Windows' own libraries.

The output's default is build/oot/totk_dlssg.dll, where tools/make_rekarina_release.py and tools/package_oot.py take
it from (the package names it rekarina_dlssg.dll). The clang is --toolchain's (an llvm-mingw tree, as for
tools/build_game.py --toolchain), else MSYS2 UCRT64's, else the one on PATH; the Vulkan headers are
toolchain/deps-mingw's.
"""
import argparse
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'runtime' / 'dlssg' / 'nx2_sl.cpp'
UCRT = Path(r'C:\msys64\ucrt64\bin')


def compiler(toolchain):
    names = ('x86_64-w64-mingw32-clang++.exe', 'clang++.exe', 'x86_64-w64-mingw32-clang++', 'clang++')
    dirs = [toolchain / 'bin'] if toolchain else [UCRT]
    for d in dirs:
        for n in names:
            if (d / n).is_file():
                return d / n
    if toolchain:
        sys.exit(f'build_dlssg: no clang++ in {toolchain / "bin"}')
    found = shutil.which('clang++')
    if not found:
        sys.exit('build_dlssg: no clang++ (pass --toolchain with an llvm-mingw folder)')
    return Path(found)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--sl', type=Path, required=True, help='the Streamline SDK folder (holds include/sl.h)')
    ap.add_argument('--toolchain', type=Path, help='an llvm-mingw folder (its bin/ holds clang++)')
    ap.add_argument('--vulkan', type=Path, default=ROOT / 'toolchain' / 'deps-mingw' / 'include',
                    help='the folder that holds vulkan/vulkan.h')
    ap.add_argument('--out', type=Path, default=ROOT / 'build' / 'oot' / 'totk_dlssg.dll', help='the DLL to write')
    a = ap.parse_args()
    sl = a.sl / 'include'
    if not (sl / 'sl.h').is_file():
        sys.exit(f'build_dlssg: {sl / "sl.h"} is missing')
    if not (a.vulkan / 'vulkan' / 'vulkan.h').is_file():
        sys.exit(f'build_dlssg: {a.vulkan / "vulkan" / "vulkan.h"} is missing')
    cxx = compiler(a.toolchain)
    a.out.parent.mkdir(parents=True, exist_ok=True)
    cmd = [str(cxx), '--target=x86_64-w64-mingw32', '-std=c++17', '-O2', '-Wall', '-Wno-invalid-offsetof',
           '-DNOMINMAX', '-DWIN32_LEAN_AND_MEAN', '-shared', '-static', '-s',
           f'-I{sl}', f'-I{a.vulkan}', f'-I{SOURCE.parent}', str(SOURCE), '-o', str(a.out)]
    r = subprocess.run(cmd)
    if r.returncode or not a.out.is_file():
        sys.exit(f'build_dlssg: the build failed ({r.returncode})')
    print(a.out)


if __name__ == '__main__':
    main()
