# Rekarina: how OoT runs here, where the frame goes, and the ways forward (2026-11-08)

A map of the port for whoever works on it next: the parts and the data that flows between them, measured on the forest
porch (RTX 5070, Ryzen 7 9700X, 2560x1440 window, scene at 1920x1080), and an assessment of the other ways to build it.
Findings with their knobs: oot-notes.md; the plan in steps: native-driver.md; the frame-time ledger: roadmap-100fps.md.

## The parts

1. **CPU code: recompiled, native.** The game's ARM64 code (main and its modules, ExeFS of the 1.1.1 update) is lifted
   to C by `recomp/` (one function per guest function, `f_<address>`), compiled by clang for x86-64 and linked with the
   runtime into one exe. Guest threads are host threads; `runtime/kernel.c` gives them the console's view (virtual cores,
   priorities, sync objects, SVCs). The game logic is a few milliseconds of a frame and is not where time goes.
2. **The NVN2 API: reimplemented.** The game calls NVN through its import table; `runtime/nvn_api_*.c` implement the
   calls. Command buffers are recorded into the game's own memory as our op stream (`nvn_api_cmd.c`), exactly when the
   game records them. Memory pools are the game's memory: the GPU address of a pool is its guest address.
3. **The replay: NVN command stream -> Vulkan.** `nvnQueueSubmitCommands` runs `vk_exec_record` on the submitting game
   thread: it walks the ops, resolves addresses to buffers and textures (`vk_memory.c`, `vk_texture.c`), binds pipelines
   (`vk_pipeline.c`) and records a Vulkan command buffer, cut into parts (`vk_sync.c`) so the GPU starts before the
   recording ends. The game's six NVN queues go to one Vulkan queue in submission order.
4. **Memory between CPU and GPU.** The console has one memory. Here the pools the GPU reads a lot have VRAM mirrors:
   each part's prologue copies the pages the CPU wrote (write-watch) into the mirror, and at the end of a submission
   what the GPU wrote goes back to guest memory, as plain copies or, where the CPU may have written the same pages
   meanwhile, as a word merge against a snapshot (`wb_merge.comp`). Uniform blocks are copied at the submit
   (`vk_usnap.c`). Results the CPU reads (reports, events, counters) are written at the fence.
5. **Shaders.** NVN2 programs are Ampere machine code. `totk_xlate.exe` (Ryujinx's shader translator with MacNativeGames'
   and seedds/nxnative's Ampere front end, plus our patches 0021-0032) turns them into SPIR-V once; the results are
   cached (`build/oot/shader-cache`) and the Vulkan pipelines are prebuilt at boot from the keys of earlier runs. Memory
   reached through loaded pointers goes through the pool table (`vk_exec_draw.c` gmem_snapshot, patch 0032).
6. **Present.** The game's final image (R8G8B8A8_SRGB, upscaled by the game's own network from the scene resolution)
   is blitted to the swapchain; the bytes reach the screen unchanged.

## Where a frame goes (2026-11-08 morning, realtime = 1, unlocked: forest porch 60-63 fps, lighter scenes 92-103 fps)

- GPU ~16 ms of work a frame: the game's passes and dispatches ~11 ms (the lighting's full-screen material passes
  ~4 ms, the forest's ~1,200 mesh-shader draws ~2.3 ms, compute culling, lights and fog ~3 ms, the upscaler network
  0.6 ms), ours ~5 ms (the word merges 2.9 ms, the plain write-backs ~1 ms for ~46-54 MiB, the prologue uploads ~1-1.5 ms
  for ~23-31 MiB). The frame is latency-bound: ~100 W of the card's 250 W; drawing fewer pixels saves little.
- The game thread ~16.7 ms a frame: the game's own code ~6 ms, the replay ~7 ms (binding work, spread over many small
  functions), the wait for the CPU's late constants (OP_WAIT_EVENT) ~1-3 ms, its own waits ~1 ms.
- Both halves are near 16 ms, so 80-100 fps needs roughly 4-6 ms off each.

What tonight's measurements say about the cost model:
- Shader code size matters more than loads: the pool-table lookup is inlined at every access (hundreds per shader), and
  variants with fewer loads but more branches were slower (three levels 15.9 ms, two levels 14.8 ms, records with the
  delta 19.1 ms). The driver's view without Nsight: `REKARINA_PIPESTATS=1` (registers and machine code size per stage).
- Writing from shaders into host memory over PCIe is expensive (~1.2 of the merges' 2.9 ms); the copy engine moves plain
  copies at ~46 GB/s.
- Host-side debug left on a hot path costs as much as a feature (a getenv per report: 20 % of the replay thread).

## The ways to stop depending on how the Switch runs it

| approach | what it means | assessment |
|---|---|---|
| A. Native NVN driver (in progress, native-driver.md) | keep the game's NVN calls; make their implementation behave like a PC driver: GPU-side waits and results, per-queue execution, bindings as data, no copies back and forth | the main road: every step so far (frames in flight, translator fixes, event waits) is part of it |
| B. Native hot paths | hook the recompiled function that records an expensive subsystem (foliage culling and mesh draws, the clustered lighting, the fog) and record a hand-written Vulkan pass for it | worth it once A's generic costs are gone: it removes translation overhead where the profile says it matters most |
| C. Own shader translator | write the Ampere -> SPIR-V translation for this game's shaders (the reference port wrote its own Maxwell translator) | large; tonight's results show the translator's output is a big lever (patch 0032: -5 ms), so targeted translator work (lookups, HMMA as cooperative matrices, CSE of pool lookups) comes first |
| D. Another language for the game code | rewrite or re-lift the CPU code in another language | no gain: the CPU code is already native and cheap; the cost is in the graphics contract |
| E. DirectX 12 instead of Vulkan | a second backend | no gain in speed or reach: every feature used exists in Vulkan, NVIDIA's Vulkan driver exposes cooperative matrices, mesh shaders, pipeline statistics; DLSS and FSR have Vulkan paths |

What depends on Nintendo, and how each dependency is handled:
- One memory for CPU and GPU -> mirrors with snapshots and merges now; the native-driver end state is GPU-side results
  plus guest pages that move to the GPU when only the GPU uses them (write-backs only for what the CPU reads).
- Six queues with GPU-side events -> one Vulkan queue now; next: queue 3 (compute) on its own queue with timeline
  semaphores for the game's GPU events, once the mirror bookkeeping is per queue.
- The CPU signalling events the GPU waits for -> the replay waits on the game thread now (REKARINA_EVENT_DEFER, which
  records past the wait, still shows another frame's shadows one frame in 300: the replay reads more guest memory as it
  records than uniform blocks and mirrors).
- Ampere machine code with tensor-core ops (the agl upscaler network's HMMA.16816) -> on the tensor cores through
  VK_NV_cooperative_matrix since patch 0033 (the upscaler 0.62 -> 0.38 ms), emulated in shared memory elsewhere.
- Frame pacing: the game's 30/60 Hz assumptions -> `realtime=1` steps the game by the clock down to 30 fps, so an
  unlocked rate does not change game speed.

## Order of work toward 80-100 fps

1. GPU, ours (~4.5 ms; ~40 % of a frame in the lighter scenes): GPU results stay on the GPU. Every merged range on the
   porch has pages the CPU writes (rings of 2.25 MiB buffers the CPU fills and shaders update), so neither plain copies
   nor a better merge kernel remove the cost; the step is a lazy write-back: ranges the CPU does not read stay in the
   mirror, the CPU's writes reach the mirror word by word (vk_memory.c gmem_merge_page has the page shadows for it), a
   guarded page writes back on a CPU access.
2. GPU, game (~11 ms): queue 3 beside queue 6, then CSE of pool lookups in the translator. The two queues are a
   fork-join on the GPU (REKARINA_EVLOG2=<frame>, which logs every event signal and wait with its command position;
   2026-11-08, forest frame): 19 event words 0x13D6C8000..+0x48, values counting up per frame. Queue 6 (graphics, ~22,400
   commands) signals +0x48 at its first command; queue 3 (compute, ~400 commands) waits for it, runs ~80 commands and
   signals +0x00/+0x04/+0x0C, then waits for +0x10..+0x20, which queue 6 signals after its first ~460-910 commands (its
   early passes); queue 6 waits for queue 3's words from command ~7,700 on. So the console runs queue 3's ~3 ms of
   compute beside queue 6's graphics, while the replay runs all of queue 3 before queue 6 and counts those waits as met
   by later signals: queue 3's middle part reads the previous frame's results of queue 6's early passes. Cutting both
   submissions at these points gives the console's order on one Vulkan queue (correctness) and, with queue 3 on a compute
   queue and the words as timeline semaphore values, the overlap (speed).
3. CPU, replay (~7 ms + waits): GPU-side event waits (approach A), bindings as data, a second recording thread.
4. Frame generation (DLSS-G or FSR 3 FG, as in the reference port) once the base frame rate is stable: it needs the
   vendors' runtime DLLs (not on this machine) and motion vectors (vk_dlaa.c already makes them for DLSS).
