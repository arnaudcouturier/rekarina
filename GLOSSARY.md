# Rekarina

Native Vulkan rendering, presentation, upscaling, and HDR pipeline for the static recompilation runtime.

## Language

### Frame Configuration

**Render Snapshot**:
An immutable set of rendering configuration values published after presentation and latched once per guest frame at window acquire.
_Avoid_: frame settings, pinned config, live settings

**Projection Revision**:
A monotonic counter on a **Render Snapshot** that advances only when projection-affecting settings change, allowing temporal upscaling history to survive unrelated menu edits.

### Scene Verification & HDR

**Checked Scene**:
A guest scene whose color attachments, depth attachments, and draw roles were verified during command recording and carried through to presentation.

**Output Proof**:
A replay-time record proving which output encoder pass produced a presented image, invalidated by any later write or aliased overwrite to that target.
_Avoid_: checked draw, written encoding

**Guest PQ**:
HDR10 PQ output encoded directly by the game's output pass and presented on an HDR10 swapchain without host re-encoding, used on `OoT` for both Vanilla HDR and RenoDX HDR.
_Avoid_: native HDR, game HDR

**Host HDR**:
Host-side PQ encoding applied over a linear float picture; used for `TotK` presentation and for `OoT` HUD-less frame-generation captures.
_Avoid_: legacy HDR

**Signed Compositor**:
A signed floating-point scene compositor surface selected for wide-gamut tonemapping so out-of-gamut values survive until the game's final BT.2020 PQ encode.

**Guest-Compositor Menu**:
Host settings menu and performance overlay blended in linear light into the game's float compositor before the game's final SDR or PQ output pass.

### Replay & Presentation

**Parallel Replay**:
Chunked multi-threaded Vulkan command recording used for draw stretches that do not require strict serial ordering against host hooks.

**Window Target**:
An image backing a guest presentation surface—the only target class that requires serial replay under `TotK` **Host HDR**, keeping **Parallel Replay** active for off-screen scene work.

**Pre-HUD RCAS**:
Contrast-adaptive sharpening applied directly to the 3D scene before HUD drawing, independent of final display scaling.

**Checked Pre-Bloom Reconstruction**:
Guest-tagged temporal upscaling (`DLSS` / `DLAA` / `FSR`) resolved before bloom and tone mapping so scenes that bypass the late scaling dispatch still reconstruct cleanly, distinct from the legacy opt-in in-place pre-bloom `DLAA` route.
_Avoid_: experimental pre-bloom, pre-bloom DLAA

**Stock-Scaler Frame Generation**:
Frame generation running with motion inputs gathered alongside the game's stock or spatial scaler without forcing DLSS or FSR temporal upscaling.

**Swapchain Owner**:
The single presentation backend (ordinary Vulkan, Streamline DLSS-G, or AMD FSR SDK 2) exclusively responsible for swapchain creation, acquire, present, and destruction.

**Present Drain**:
Waiting for all in-flight presents of the active **Swapchain Owner** to finish before replacing or falling back from its swapchain.
