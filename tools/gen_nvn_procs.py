"""Write runtime/nvn_procs.inc: the NVN proc names TotK 1.2.1 loads, then, under GAME_NS2, the Switch 2's (NVN2).

Switch 1 part. Source names come from the MacNativeGames checkout (MIT, Copyright (c) 2026 MacNativeGames
contributors): NVN/names.txt plus the NVN_EXTRA definitions of totk_nvn_ext.c, minus the two names 1.2.1 does not
load. The file list format follows d2r-mobile dm/gfx/tools/gen_protos.py (MIT). This part is unchanged by the GAME_NS2
rows: a build without GAME_NS2 sees the same 537 rows as before.

Switch 2 part (#ifdef GAME_NS2). Every name a Switch 2 game's proc loader can ask for that the Switch 1 part lacks:
- the NVN2 and later-SDK names MacNativeGames appended to names.txt after the 523 Switch 1 names (its GAME_NS2 lookup
  answers all of them), and seedds/nxnative's dm/gfx/nvn/names.txt (MIT, Copyright (c) 2026 seedds);
- OOT_NAMES below: the names OoT (0400486024DEC000, SDK 22.2.8) asks for that neither list has. They were
  found by reading its modules: main's proc loader passes 736 names to nvnDeviceGetProcAddress (string addresses loaded
  right before the call, each result stored to its pointer), the sdk's native-image and interception glue 48 more. Only
  API identifiers are listed; no game bytes.
A Switch 2 build answers each of these with a proc address; nvn_procs.c makes the ones without an implementation log
their name once and return 0 (MacNativeGames hle_nvn.c nvn_unimpl_c under GAME_NS2).

No game data is read. Run with .venv: python tools/gen_nvn_procs.py [--refs DIR]
(DIR holds MacNativeGames/ and nxnative/ checkouts; default refs/, which tools/fetch_refs.sh fills).
"""

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "runtime/nvn_procs.inc"
MNG_NVN = "MacNativeGames/games/totk/third_party/d2r_gfx/gfx/nvn"
NXN_NAMES = "nxnative/dm/gfx/nvn/names.txt"

NOT_LOADED = {"nvnDeviceApplyDeferredFinalizes", "nvnQueueAcquireTexture"}
SUBSDK0 = [  # section 0.1: asked by the movie library through nvnDeviceGetProcAddress
    "nvnDeviceGetProcAddress",
    "nvnSyncFinalize",
    "nvnSyncGetNvRmSyncNVX",
    "nvnSyncInitializeFromNvRmSyncNVX",
    "nvnTextureGetNvRmSurfaceNVX",
]
MAIN_COUNT = 534
SWITCH1_NAMES = 523   # names.txt: the Switch 1 names first, up to nvnDeviceSetCheckpointMaxCallStacksDepth

# OoT: main's loader asks for nvnInstallGlobalDebugCallback; the sdk's nvnNativeImage / external memory pool
# glue (sdk 22.2.8, near nvnBootstrapLoader) asks for the NVX names through nvnDeviceGetProcAddress and stops when one
# comes back NULL.
OOT_NAMES = [
    "nvnCUDAInteropFunction",
    "nvnCommandBufferCopyTextureToTextureWithCopyEngineNVX",
    "nvnDeviceBuilderGetReservedAddressRangesNVX",
    "nvnDeviceBuilderSetReservedAddressRangesNVX",
    "nvnInstallGlobalDebugCallback",
    "nvnInterceptionFinalizeNVX",
    "nvnInterceptionInitializeNVX",
    "nvnInterceptionOwnedNVX",
    "nvnInterceptionReleaseNVX",
    "nvnInterceptionSetDispatchResetCallbackNVX",
    "nvnInterceptionSetProcAddressNVX",
    "nvnInterceptionTryAcquireNVX",
    "nvnMemoryPoolBuilderGetNativeHandleNVX",
    "nvnMemoryPoolBuilderGetReservedAddressesNVX",
    "nvnMemoryPoolBuilderSetGenericAddressNVX",
    "nvnMemoryPoolBuilderSetNativeHandleNVX",
    "nvnMemoryPoolBuilderSetSpecialAddressNVX",
    "nvnMemoryPoolGetGenericAddressNVX",
    "nvnMemoryPoolGetNativeHandleNVX",
    "nvnMemoryPoolGetSpecialAddressNVX",
    "nvnProgramGetDebugDataHashesNVX",
    "nvnQueueSetTimeoutNVX",
    "nvnQueueSetTimesliceNVX",
    "nvnSetGLInteropNVX",
    "nvnSyncGetSyncpointsCountNVX",
    "nvnSyncGetSyncpointsNVX",
    "nvnSyncSetSyncpointsNVX",
]


def read_names(path: Path) -> list[str]:
    return [n.strip() for n in path.read_text(encoding="utf-8").splitlines() if n.strip()]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--refs", type=Path, default=ROOT / "refs",
                    help="directory holding the MacNativeGames and nxnative checkouts (default refs/)")
    args = ap.parse_args()
    nvn_dir = args.refs / MNG_NVN
    names = read_names(nvn_dir / "names.txt")
    extras = re.findall(r"^NVN_EXTRA\((\w+)", (nvn_dir / "totk_nvn_ext.c").read_text(encoding="utf-8"), re.M)
    if len(extras) != 13:
        print(f"expected 13 NVN_EXTRA names, found {len(extras)}", file=sys.stderr)
        return 1
    if len(names) < SWITCH1_NAMES or names[SWITCH1_NAMES - 1] != "nvnDeviceSetCheckpointMaxCallStacksDepth":
        print(f"{nvn_dir / 'names.txt'}: the Switch 1 names do not end at line {SWITCH1_NAMES}", file=sys.stderr)
        return 1
    switch1 = names[:SWITCH1_NAMES]
    main_names = [n for n in switch1 if n not in NOT_LOADED] + extras
    if len(main_names) != len(set(main_names)) or len(main_names) != MAIN_COUNT:
        print(f"expected {MAIN_COUNT} distinct main names, found {len(main_names)} "
              f"({len(set(main_names))} distinct)", file=sys.stderr)
        return 1
    # nvnDeviceGetProcAddress is in both lists; keep one row per name.
    extra_sub = [n for n in SUBSDK0 if n not in main_names]
    rows = sorted(main_names) + sorted(extra_sub)

    nxn = read_names(args.refs / NXN_NAMES)
    ns2 = set(names[SWITCH1_NAMES:]) | set(nxn) | set(OOT_NAMES) | NOT_LOADED
    ns2 -= set(rows)
    ns2.discard("nvnBootstrapLoader")   # the entry the game imports (ov_gfx.c), never a proc it looks up
    bad = [n for n in ns2 if not re.fullmatch(r"nvn[A-Z]\w+", n)]
    if bad:
        print(f"not NVN proc names: {bad}", file=sys.stderr)
        return 1
    ns2_rows = sorted(ns2)

    text = ("// Generated by tools/gen_nvn_procs.py: the NVN proc names of TotK 1.2.1, then (GAME_NS2) the Switch 2's.\n"
            + "".join(f"NVN_PROC({n})\n" for n in rows)
            + "#ifdef GAME_NS2\n"
            + "".join(f"NVN_PROC({n})\n" for n in ns2_rows)
            + "#endif\n")
    OUT.write_text(text, encoding="utf-8", newline="\n")
    print(f"{OUT.relative_to(ROOT)}: {len(main_names)} main names + {len(extra_sub)} subsdk0-only names "
          f"({len(SUBSDK0)} asked by subsdk0), {len(rows)} rows; GAME_NS2: {len(ns2_rows)} more "
          f"({len(rows) + len(ns2_rows)} in a Switch 2 build)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
