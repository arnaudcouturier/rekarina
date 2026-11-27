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
HDR10 PQ output encoded directly by the game and presented on an HDR10 swapchain without host re-encoding.
_Avoid_: native HDR, game HDR

**Host HDR**:
Host-side RenoDX grading, tonemapping, and PQ encoding applied over the game's float or SDR picture.
_Avoid_: legacy HDR

### Replay & Presentation

**Parallel Replay**:
Chunked multi-threaded Vulkan command recording used for draw stretches that do not require strict serial ordering against host hooks.

**Window Target**:
An image backing a guest presentation surface—the only target class that requires serial replay under **Host HDR**, keeping **Parallel Replay** active for off-screen scene work.

**Swapchain Owner**:
The single presentation backend (ordinary Vulkan, Streamline DLSS-G, or AMD FSR SDK 2) exclusively responsible for swapchain creation, acquire, present, and destruction.

**Present Drain**:
Waiting for all in-flight presents of the active **Swapchain Owner** to finish before replacing or falling back from its swapchain.
