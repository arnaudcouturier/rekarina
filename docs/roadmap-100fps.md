# Rekarina: the road to 100 fps (measured 2026-11-08)

Where an OoT frame goes on the forest porch (save "porch", scratchpad script fast_porch), what was tried on
the night of 2026-11-08, and what is worth building next, ordered by the frame time each step can win. Machine: Ryzen 7
9700X, RTX 5070 (driver 610.88, resizable BAR), 2560x1440 window. Companion to native-driver.md (the plan of
2026-11-07) and oot-notes.md (the findings, with the knobs).

## Where the frame goes

Frame rate on the porch, unlocked (TOTK_FPS_LIMIT=240):

| build | present waits for its own frame (default, `c`) | one frame in flight (`TOTK_PRESENT_SYNC=1`) |
|---|---|---|
| 2026-11-07 evening | 35-37 fps | 39-41 fps |
| 2026-11-08 night | 42 fps | 45-46 fps |
| 2026-11-08, new default `c1` (all but the frame's last submission) | 45-47 fps, no flicker | |
| 2026-11-08 morning: translator patch 0032, getenv off the report path | 57 fps (GPU 14.8 ms, 1 % low 33) | same (c1, c2 and 1 within 0.3 fps) |
| same, measured with TOTK_REALTIME=1 (the rows above ran with 0: capped at 60, missed frames took two vsync steps) | porch 60-63 fps, lighter scenes 92-103 fps | |

Update of the morning (details in oot-notes.md, "The shaders as the driver compiles them"): the GPU frame went from
19.8 to 14.8 ms, nearly all of it from translator patch 0032 (the raw global memory lookup, inlined ~500 times per
shader, without its unrolled binary search), and the replay thread lost ~20 % of its time to a getenv per counter
report. Now GPU ~14.8 ms busy + ~2 ms idle, CPU interval ~16.7 ms: ~57-62 fps on the porch. Measured and left: the
merges cost 2.9 ms of GPU time for ~20 MiB a frame (about 1.2 ms of it the shader's writes into host memory; the range
search is not the cost; every merged range has pages the CPU wrote, so they cannot become plain copies safely); a
deferred OP_WAIT_EVENT (TOTK_EVENT_DEFER=1) removes the replay's ~2.5 ms waits but draws another frame's shadows one
frame in 300; async replay is no faster (56.8 fps both).

GPU, one frame in flight (vk_sync.c part timestamps, TOTK_GPU_PASSTIME):

- Busy ~19.6-20 ms a frame, idle ~2 ms (0.8 ms of it waiting for the CPU to submit).
- The game's passes and dispatches take ~15.3 ms of it: queue 6 (main graphics, ~157 passes) 12.1 ms, queue 3
  (compute: culling, lighting, fog; 8 command buffers, ~135 dispatches) 3.15 ms, the four other queues next to nothing.
  The game's six NVN queues run concurrently on the console; the replay runs them one after another on one Vulkan
  queue, so at best ~3 ms of queue 3 could overlap queue 6.
- The rest (~4 ms) is ours: per frame ~31 MiB of mirror uploads, ~46 MiB of GPU writes copied back to guest memory
  (9 ranges over 4 MiB: the 9.5 MiB light lists and others), ~20 MiB of merge snapshots and ~57 merge dispatches, and
  ~425 full barriers.
- The GPU draws ~100 W of its 250 W at 84 % utilisation: the frame is latency-bound (long chains of small dependent
  passes and dispatches), not throughput-bound. Proof: drawing 64 % / 44 % of the pixels (graphics scale tier 1 / 2)
  only goes from 19.9 to 18.4 / 17.9 ms, and the 1,200-draw forest pass costs 3.1 ms at 1920x1080 and 2.9 ms at
  1536x864, while the depth pre-pass with the same draws takes 0.2 ms (the cost is in its vertex, mesh and fragment
  shaders, not in the draws or the pixels).

Per draw (TOTK_DRAWTIME=<frame>, vk_exec_draw.c: a GPU timestamp before every draw and at every pass end, summed per
shader pair; TOTK_DRAWTIME_DIR writes the dearest shaders' SPIR-V): ~10 ms of the frame is in draws, and the dearest
are not the forest's 1,200 draws (0.2-1 ms per 200-draw part) but single full-screen draws of 0.2-1.0 ms each: the
material passes. Each draws one screen-sized triangle depth-tested against a material-ID depth (func LEQUAL, no depth
write), so it shades the pixels of its material only; they have no discard, no depth write and no stores (early depth
test applies). Their fragment shaders (fs 97bc6d48176e1bc7 0.98 ms, 811b27df778b5472 0.62 ms, 8fde5c5b0e4b3a6b
0.58 + 0.35 + 0.21 ms, 8520c2e63239e04d 0.44 ms, bf0ba6f69e9e1f54 0.44 ms) are 5,000-15,000 SPIR-V instructions:
~11 texture samples, ~30 % loads and stores of Function variables (the translator's registers), and one loop of
~3,200 instructions of math per light of the pixel's cluster. The clusters are small (0-2 lights each: the
9.5 MiB list pool at 0x25ea86000 read back at frame 1420), so the cost is the per-pixel body, not the light count.
Whether that body compiles to something much larger than the console's original shader needs the driver's view
(Nsight Graphics shader profiler: SASS size, registers, occupancy, stall reasons); it is not installed here.

CPU (render thread, TOTK_PROFILE_RENDER=1, frames in flight):

- Replay ~15-20 ms a frame of recording, 5-6 us per draw. Of the render thread's time: ~19 % asleep in OP_WAIT_EVENT
  (the game's last submission waits for constants another thread writes late; on the console the GPU waits there, here
  the game's thread does), ~17 % copying guest memory into the mirrors (the game flushes ~47 MiB a frame, ~21 MiB of
  it written), ~5 % submit-time snapshots, the rest descriptor and binding work.
- When the GPU is not the limit (barriers removed, a broken picture) the CPU bound is ~19.7 ms.

So 100 fps (10 ms) needs both halves roughly halved. Neither single fix gets there.

## What the night tested

| change | result | kept |
|---|---|---|
| float sums written back as additions (wb_merge.comp) | auto exposure stable with frames in flight | yes |
| dispatch after dispatch ordered by the game's barriers only | GPU -0.6 ms | yes (`TOTK_DISPATCH_BARRIER=full` to undo) |
| the game's fragment-only barriers relaxed (~65 a frame) | no measurable change, picture same | yes (`TOTK_GAME_BARRIER=full`) |
| indirect mesh draw counts copied once per stretch | 60 -> 28 barriers a frame | yes (`TOTK_MESH_BATCH=0`) |
| parts submitted inside long passes (every 200 draws) | GPU wait for submission 2.9 -> 0.8 ms; 41.7 -> 45.2 fps | yes (`TOTK_SPLIT_IN_PASS`) |
| graphics scale tier 1 / 2 (the game draws 4/5, 2/3 of the window) | GPU -1.2 / -2 ms | option, off (`TOTK_GRAPHICS_SCALE`) |
| debug code off the hot path | a few % of the render thread | yes |
| every one of our barriers removed | picture white | no (measurement) |
| UpdateUniformBuffer dropped entirely | GPU -0.4 ms | no (measurement) |
| big write-backs skipped | foliage flicker, uploads 113 MiB | no (measurement) |
| replay on its own thread (`TOTK_REPLAY_ASYNC=1`) | same fps, more flicker | no |
| "the CPU's word wins" in the merge | foliage flicker every 8th frame | no |
| present waits for all but the last submission (`c1`) + late copies for shader-written pools | 45-47 fps, 1,200 frames without flicker | yes, default |
| result guard (`TOTK_RESULT_GUARD=1`) | foliage flicker gone, 13-20 fps (~47 waits a frame) | off |
| result guard on reads only | 53 waits a frame, 13 fps, flicker | no |
| write-back of the 46 MiB of big ranges skipped (pages kept as the mirror's) | GPU -1 ms, one stale read (flash) | no (measurement) |

## Next steps, by the frame time they can win

Order after the morning of 2026-11-08: (a) the merge shader rebuilt with a compiler (glslc is not on this machine):
write merged words into VRAM and let the copy engine move whole blocks the GPU rewrote, or batch the host-memory writes;
(b) the raw global memory access: CSE of lookups for accesses through the same loaded pointer (the 2-level lookup is
still ~108 instructions inlined per access; heavy compute shaders have 200+ of them); (c) HMMA.16816 as
VK_KHR_cooperative_matrix (the driver has it, compute only): the game's agl upscaler network (dmsh_hmma16816_f16,
1,220 instructions, 176 calls, 0.63 ms a frame); (d) the CPU side below (the replay ~10 ms of the game thread's
16.7 ms); (e) the late-signal wait without stalls: find what the replay reads at record time besides uniform blocks and
mirrors (TOTK_EVENT_DEFER's shadow frame).

GPU (from ~20 ms):

1. Write-back off the graphics queue (~3 ms). Measured with marks around the mirror prologue, the merge dispatches and
   the write-back copies (GPU_PASSTIME, VK_MARK_PROLOGUE / MERGE / WBCOPY): per frame ~1.0 ms of prologues, ~1.0 ms of
   plain write-back copies (46 MiB) and ~3 ms of "merge" time, all in queue 3's submission; with the merge off
   (TOTK_WB_MERGE=0, a measurement: it breaks the exposure) the GPU frame drops 19.5 -> 17.5 ms. The merge itself
   moves only ~4.5 MiB (100 ranges, most in the 88 MiB pool 0x15af30000); the cost is the serialisation around it (full
   barriers before and after, the next submission's prologue waiting for it). Relaxing those barriers is not safe on
   one queue: the next prologue uploads the CPU's bytes into the same pools the merge reads. Plan: the merges and
   copies of a submission on an async compute queue, waiting on its timeline value and signalling one of their own;
   a submission is done when both are; a later submission waits for that signal only if it writes (uploads into or
   shader-writes) a pool the pending write-back reads.
   Measured 2026-11-08: the write-back on a second graphics-family queue (built, then removed) took 3.5 ms off the
   graphics queue but cost more than it saved: the next submissions overwrite words the pending write-back reads (a
   64 KiB pool of status words written by fills in every submission), so queue 6 waited for it, and with those waits
   off (unsafe) the two queues slowed each other down (graphics 16 -> 32 ms). A second queue is not the way on this
   card; cheaper merges are (they move only ~4.5 MiB: the cost is the full barriers around them).
   Old note:  The write-backs (46 MiB) and the merge snapshots run on the graphics queue
   between the game's passes. A transfer queue (the card has a dedicated one, vk_device.c already finds it) waiting on
   the part's timeline value takes them off the critical path; the completion thread counts a submission done when its
   copies are. Better still, lazy write-back: pools the CPU never reads (the light lists) keep their GPU data in the
   mirror only, their guest pages closed; a CPU access opens them with a synchronous copy (the result guard machinery
   of memory.c, which exists and is tested).
2. Queue 3 on an async compute queue (up to ~3 ms). Map each NVN queue to a Vulkan queue (graphics family 16 queues,
   compute family 8 on this card) and the game's GPU-side event words (0x13D6C80xx: signal on one queue, wait on
   another) to timeline semaphores. The mirror and merge bookkeeping is per submission and assumes one order: it has
   to become per queue first.
3. Shader cost (the material passes, ~4 ms together; then the forest pass): first get the driver's view of the dearest
   material shaders (Nsight Graphics). If the translation is the cost (register traffic, conversions, scalarised vector
   math), a translator pass that emits it the way the console's compiler did pays everywhere. Separately, every load
   through a loaded pointer goes through the
   memory pool table (translator patch 0020: a 2 MiB-chunk hint, else a 13-step binary search over 8,192 entries) before
   it reaches memory. One device address range for all guest pools (a sparse buffer, or the pools bound at their guest
   offsets inside one large buffer with buffer device address) makes such a load one add. Needs per-shader timing to
   rank (Nsight Graphics on this machine).
4. Resolution: the graphics scale tier is the game's own dynamic resolution; with DLSS Super Resolution (vk_dlaa.c,
   NGX) on top it costs no picture. Worth ~2 ms only, because the frame is not pixel-bound.

CPU (from ~20 ms):

5. OP_WAIT_EVENT on the GPU (~3-4 ms of the render thread). Record the rest of the submission behind a Vulkan event
   (or a split point submitted later) and copy the late uniform ranges when the game signals; the game's thread goes
   on, as on the console.
6. Per-draw replay cost (5-6 us -> ~1 us): descriptor sets reused while the stage's bindings are unchanged, uniform
   ranges bound only as far as the shader reads them, bindings as data (native-driver.md step 2).
7. Queue 3 and queue 6 recorded on two threads (they are separate submissions; with per-queue bookkeeping from step 2
   they can record at once).
8. Mirror uploads: copy only pages the write-watch saw written inside the flushed ranges (21 of 47 MiB a frame).

Frames in flight (done for one frame's last submission, `c1`; deeper overlap next):

9. (Page-granular guards do not work for this game: the pages that hold GPU results are touched 50 times a frame.)
   The CPU reads some GPU results without a sync and expects them current (foliage culling counts, f_7002bfd340): with
   a frame in flight our write-back at the end of the submission is too late now and then (the far trees lose their
   leaves for a frame). The result guard fixes it but closes whole pages, which hold CPU data too (~47 waits a frame).
   Refine: guard only after a guest read fault on that range was seen (learned), or write back each part's small
   ranges when the part completes, so results reach guest memory about as early as the console's GPU writes them.

PC features once the frame is there: DLSS / DLAA (vk_dlaa.c), frame generation (DLSS FG or FSR 3 FG; the reference
port weeping-of-the-empire has both in ngx.c and ffx.c), HDR, ultrawide.

## Recompilation approaches considered

- The CPU side is already native: the game's ARM64 code is recompiled to C and compiled for x64; the game logic
  (~2 ms of a frame) is not the problem. Rewriting it in another language buys nothing.
- What depends on Nintendo is the graphics contract: NVN command buffers, its memory model (one memory for CPU and
  GPU), its queues and events, Maxwell/Ampere shader binaries. Three ways to stop depending on it, from least to most
  work:
  1. A native NVN driver (native-driver.md, in progress): keep the game's NVN calls, make their implementation cost what
     a PC driver costs (no snapshots, no copies back and forth, bindings as data, per-queue execution).
  2. Native hot paths: where profiling shows a translated subsystem dominating (the foliage culling and mesh draws, the
     volumetric fog, the clustered lighting), hook the recompiled function that records it (recomp patch sets) and
     record a hand-written Vulkan version of that pass. The rest stays generic.
  3. A new shader translator: the reference port wrote its own Maxwell-to-SPIR-V translator (maxwell/translate.c,
     spvgen.c) instead of Ryujinx's; one written for this game's Ampere shaders could emit direct memory access, fp16
     where the original uses it, and no compatibility scaffolding. Large; only after step 3 above shows the shaders are
     the limit.
- DirectX 12 instead of Vulkan: no gain in reach or speed for this work (every feature used exists in Vulkan; Nsight
  and RenderDoc cover Vulkan); not worth the rewrite.
