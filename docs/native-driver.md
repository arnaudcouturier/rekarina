# Graphics as a native NVN driver (plan)

OoT's CPU code is recompiled and costs about 2 ms of a frame. The frame time goes to the graphics layer,
which works like an emulator's: it records the game's NVN commands, translates them into Vulkan on one thread when the
game submits, and then waits for the GPU before the game may start the next frame. This document is the plan for
replacing that with a layer that behaves like the console's driver: commands are translated when the game records them,
on the game's own threads, the GPU runs a frame behind the CPU, and what the console resolves on the GPU is resolved on
the GPU here too.

## Goal and roadmap (agreed 2026-11-07)

The player wants OoT to behave like a native PC game: a clean picture that matches the console, no
flashes or flicker, 100+ fps on an RTX 5070, and the PC features a port has (resolution, DLAA/DLSS, HDR, high refresh,
ultrawide, settings). The game's own code stays the recompiled original: rewriting the engine would mean writing the
game again. What is rebuilt is everything between that code and the PC, so that it costs what a native renderer
costs and never shows an emulator's artefacts. Phases, each ending in one test round (porch, flyover, still and moving
camera, flash watch), in this order.

Principle: a recompiled game, not an emulator. The game's code is native already; the graphics layer must stop
behaving like an emulator's (record, replay, guess the game's intent from memory traffic, copy memory back and forth).
Where the engine relies on a console property (unified memory, render targets aliased in a transient heap, the CPU
reading GPU results, its dynamic resolution), the recompiled function is replaced with a PC-native one through the
recomp's hooks (recomp/patches-oot-v111.txt, runtime patch sets) rather than the console's behaviour being emulated
underneath it. Every shader is translated at install time, as Unleashed Recompiled does, so none is translated in play.


- A. Memory and textures like the console's. A texture is a view of pool memory, not an object of its own: one image
  per storage range and layout, views for the descriptors that read it, content that follows the memory (the game's
  transient render targets reuse addresses every frame with new sizes). Counter reports, events and small read-backs
  written by the GPU in GPU order; a CPU read of a pending result waits for that result only. Fixes the black
  flashes (oot-notes.md) and the "left uninitialised" aliases as a class.
- B. Frames in flight: the present stops waiting for its own frame's GPU work; the GPU runs up to two frames behind
  (step 1 below). The porch at 37 fps waits ~11 ms of 27 ms on it now.
- C. CPU cost of the graphics layer: per-draw replay to ~1 us, then bindings as data and translation at record time
  (steps 1c, 2, 3 below), on the game's own threads.
- D. GPU cost: the lit pass and the remaining heavy passes, async compute where the game's work allows.
- E. PC features: resolution above 1440p, DLAA/DLSS (vk_dlaa.c with OoT's hooks), HDR, uncapped frame rate with the
  game's variable step, ultrawide, an in-game settings page.
- F. Picture parity with the console references: exposure and haze, the great tree's teal squares.

Measured on this machine (vulkaninfo, driver 610.88): resizable BAR on (memory type 4 is DEVICE_LOCAL|HOST_VISIBLE on
the whole 11.9 GiB heap); VK_EXT_device_generated_commands, VK_EXT_descriptor_buffer, VK_EXT_nested_command_buffer and
VK_EXT_shader_atomic_float present; VK_EXT_map_memory_placed absent, so pool memory cannot be placed at the guest's
addresses and CPU-written pools keep a VRAM mirror.

Done (round 1, oot-notes.md "GPU time, first round"): native float atomics, relaxed pass-after-pass barriers, the
released-texture VRAM rule: porch 15 -> 37 fps, GPU 57 -> 20.5 ms a frame.

## Where the frame goes now

Measured on a Ryzen 7 9700X with an RTX 5070, with the GPU to itself (docs/oot-notes.md, "Performance, where the frame
goes"):

- Porch of the save: 36 fps. A frame is replay 15-20 ms, then ~10 ms of waiting for that frame's own GPU work, then
  ~2 ms of game code. The CPU and the GPU take turns.
- A forest frame is ~3,000 draws and ~130 dispatches in ~265 passes: 5-6 us of replay per draw, where a native renderer
  spends well under 1 us.
- Indoors 60 fps; outdoors after the house 42 fps mean with 13 fps lows; the "the forest" flyover ~17 fps.

The steps below are ordered so that each one is measurable on its own and the picture stays checkable after each.

## What the other ports do (surveyed 2026-11-07)

Read in source: seedds/nxnative, ly05010419/MacNativeGames, peiikai/LASRecomp, Revivedx/SavageWindRecomp (all MIT);
read in docs: hedge-dev/UnleashedRecomp, rt64, Ship of Harkinian.

- None of the NVN ports translates at record time: nxnative, MacNativeGames, LAS Recomp and Savage Wind replay at the
  submit as we do. MacNativeGames measured a separate translation thread as no gain; nxnative lists hazards of reading
  a recording early (games patch recorded commands, deferred binds read at execution, handles written into uniform
  buffers earlier in the same submission). So step 3 below is the last and least certain step.
- They spend 0.5-2.2 us per draw with the same design (MacNativeGames ~0.5 us with eight replay caches, nxnative
  ~2.2 us in BotW, Savage Wind ~1-1.5 us). Our 5-6 us is implementation cost, which the profile
  should find, not the design.
- Every one lets the GPU run behind: LAS Recomp and Savage Wind answer the game's frame fence one frame late, keep
  uniform copies (Savage Wind: reading uniforms in place raced with the overlap, magenta and white frames) and read
  the exposure target back a frame late; Savage Wind went 72 -> 86 and 82 -> 119 fps with it. Unleashed Recompiled
  keeps two frames in flight and never waits for the GPU at present.
- nxnative on OoT (Mac, Metal): 60 fps in forest village with a GPU span of 16.5 ms, ~101 passes. Two GPU
  fixes measured there: barriers only where the game asks for them instead of a full barrier before every pass
  (GPU 25 -> 18 ms; NVN does not ask for ~180 full barriers a frame), and raw global-memory lookups that start from
  the entry used last (16.5 -> 14.3 ms). OoT's dynamic resolution needs each timestamp report to carry its
  submission part's GPU start or end: with one time per submission it never lowers the resolution, with every
  report's exact time it fell to its lowest.
- We start the GPU early (parts every 64 draws) and still wait ~10 ms after a 15-20 ms replay, so the GPU likely needs
  25-30 ms of a forest frame: once the CPU stops waiting, the GPU may be the limit. Step 0's GPU timeline decides.

## Step 0: measure

- Render thread sampling profile (REKARINA_PROFILE, REKARINA_PROFILE_RENDER=1, tools/prof_report.py): where the 5-6 us per draw
  go, so the parts of the replay that are already cheap are kept.
- Slow-frame chains (REKARINA_PROFILE_SLOW): the ~120 ms frame every 30 frames on the porch, of which only ~30 ms is replay
  and waits.
- GPU time per submission part (REKARINA_TRACE=perf) and per pass (REKARINA_GPU_PASSTIME): how much GPU time the translated
  shaders take (global memory table lookups, compare-and-swap loops) against the frame.
- Which threads record command buffers, and how many recordings a frame submits: the parallelism step 3 can count on.

## Step 1: frames in flight

nvnQueuePresentTexture waits for the frame's own GPU work (REKARINA_PRESENT_SYNC=c, nvn_api_obj.c), added because the game
reads GPU results of the frame it just presented during its next frame without an NVN sync, and about every other frame
came out white while the camera moved. Its records are contradictory about which read it is (the 1x1 "luminance" is a
sun visibility test that stops ~100 frames after a load), so the step starts with an A/B over the sync variants and the
fence readback (REKARINA_PRESENT_SYNC c / 1 / n, REKARINA_FENCE_READBACK 0 / 1), counting flashes in shot series with a moving
camera.

The console's GPU writes its results into memory in GPU order, with no CPU involved. The port should too:

- Counter reports and timestamps: today the completion thread writes them into guest memory after the fence. Written by
  the GPU instead (vkCmdWriteTimestamp and occlusion queries, then vkCmdCopyQueryPoolResults into the imported pool
  buffer at the report's address), they land when the GPU gets there, as on the console.
- Events: vkCmdFillBuffer / vkCmdUpdateBuffer into the pool at the event's address.
- Small render targets the CPU reads: the fence readback (vk_texture.c vk_texture_fence_readback) copies them out at
  the end of their submission; a compute pass that writes the block-linear storage straight into the guest pool would
  take the completion thread out of it as well.
- Only then is the present's wait replaced by a bound: the GPU at most one frame behind (REKARINA_PRESENT_SYNC=1), later two.
- Where a CPU read must still wait, it waits for that read only: guard the pages of pending GPU results (no access until
  their fence, a fault waits for it), as the write-watch side already guards idle pool pages
  (platform/win32.c plat_mem_fault_hook).

Expected: the ~10 ms wait leaves the frame; the porch goes from ~28 ms to ~20 ms.

## Step 1b: GPU time

- Barriers: the replay puts a full barrier before every pass (vk_exec.c, after LAS Recomp). Keep the game's own
  barriers (OP_BARRIER) and between passes wait only for what the next pass reads (nxnative's draw_barriers game: GPU
  25 -> 18 ms on OoT).
- Global memory table lookups (translator patch 0020): start from the entry the lane used last.
- Float atomic adds as native atomics (VK_EXT_shader_atomic_float) instead of compare-and-swap loops (patch 0022); the
  loop also reloads with a plain load, which can spin on a stale value.

## Step 1c: per-draw replay cost

Profile first; then the caches the other ports measured: an argument or descriptor block reused while the pass and
stage are clean (nxnative), uniform buffers bound only as far as the shader reads them (Savage Wind: 94 % of copy bytes
gone), MacNativeGames' replay caches. Target: ~1 us per draw, ~3 ms for a forest frame.

## Step 2: texture and storage bindings as data

On the console a texture bind writes a handle into the driver's constant bank and the shader indexes the texture and
sampler pools with it; a storage buffer bind writes an address. Nothing is written per draw but those words. The replay
instead resolves every bound texture and storage buffer of every draw into Vulkan descriptors.

- Textures and images: the translator already indexes arrays over the whole texture pool for handles loaded from
  storage buffers (vk_texpool.c, gfx_shader.c patch_pool_arrays). Doing the same for every bound texture (the handle
  read from c0, which vk_c0_fill then fills with the bound handles) leaves the per-draw texture descriptors with nothing
  to do. The pool set is update-after-bind; its entries are written when a texture is registered or its image changes,
  not per draw.
- Storage buffers: the address is in c0 already (vk_c0_fill). Through buffer device addresses (the global memory table
  that loaded addresses use, gmem_map, or a delta per pool) the shader reaches it without a descriptor.
- Uniform buffers stay descriptors (push descriptors through templates are cheap and uniform reads are fastest that
  way).

Expected: most of the per-draw resolve work goes; a translator change (cache re-keyed) and the texture cache's lazy image
creation move to registration time.

## Step 3: translation at record time

With bindings as data, a recording's Vulkan commands depend on little that only the submit knows. Each recording is
translated when the game ends it (nvnCommandBufferEndRecording), on the game's recording thread, into a Vulkan secondary
command buffer (dynamic rendering may begin inside a secondary) plus a short list of what the submit fills in:

- Uniform snapshots: the game rewrites per-object uniform blocks after it submits. The recording reserves the ranges of a
  per-submission ring at record time; the submit copies the blocks into them (vk_usnap.c does this copy today) and the
  descriptors already point there.
- Deferred binds (NVN's *Deferred entry points) and textures registered after the recording: written into the
  update-after-bind pool set at the submit.
- Conditional rendering: VK_EXT_conditional_rendering on a predicate the GPU computes from the counter it reads, instead
  of a CPU decision at replay.
- Pipelines: the state is known at record time, so the lookup and the asynchronous build start there.

The submit then uploads what the CPU wrote into mirrored pools (vk_memory.c, per submission as now), fills the list, and
executes the secondaries. CallCommands / CopyCommands inline at record time, or execute nested secondaries
(VK_EXT_nested_command_buffer).

Expected: the replay thread does per-submission work only; the per-draw translation spreads over the threads the game
records on.

## Picture quality

The console renders 1080p-1440p docked with dynamic resolution, FSR 1 and SMAA, at 60 fps (Digital Foundry). On PC:

- Resolution: the scene at 1.0 of the output (romfs ModelSceneExtensionParam, tools/make_oot_prompts.py --scene-scale)
  with the dynamic resolution pinned (patch set gpuload, or per-part timestamps as above); output above 1440p needs the
  output patch sites TotK has (runtime/resolution.c) found in OoT's code.
- Anti-aliasing: DLAA/DLSS through vk_dlaa.c needs OoT's hooks (the light target before the bloom, jitter, motion
  vectors from depth and the camera block); the per-object motion path needs OoT's uniform layout.
- Frame rate above 60: the game steps by a variable ratio (patch set realtime); check gameplay at 120 and 144 Hz
  steps (STEP_MIN_S allows 1/240) before raising the 1/30 s cap's counterpart at the top.
- Ultrawide: projection aspect, culling frustum and HUD anchors in the recompiled code (RT64's align commands and
  Unleashed's 21:9 are the models).
- HDR: replace the ACES tonemap pass (b624dfe03d856516) as vk_hdr.c does TotK's.

## Risks

- Every step changes when the GPU reads memory the CPU writes. The checks that caught the old bugs (foliage lists,
  white frames, flicker, the subtitle stripes) have to pass after each step: shot series with a still and a moving
  camera, the porch and the flyover against the console references.
- A recording submitted twice while the first submission is in flight needs its own snapshot ring ranges (a clone).
- The texture cache creates images lazily at bind time today; moving that to registration time must not upload textures
  the game never draws.
