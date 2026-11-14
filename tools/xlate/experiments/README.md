# Translator experiments (not applied)

Patches that `tools/build_xlate.py` does not apply: it takes `tools/xlate/patches/*.patch` only. They are the tools of
one investigation, kept for the next one. Each changes nothing unless its job field is sent. They were cut in this
order on top of patch 0040, each on the one before; `0043` has since become patch 0041 of the build, so `0042` and
`0044` need their context lines moved before they apply again.

## Mesh shaders on AMD's Windows driver (2026-11-14)

On the integrated Radeon (AMD proprietary driver 2026.04.17, the "AMD proprietary shader compiler" branch for RDNA 1
and 2, 64-lane subgroups) every mesh draw of OoT came out empty. What the runs showed is in
docs/oot-notes.md under that date; in short:

- `0041-shared-byte-stores-without-a-loop.patch` (`"stores_no_loop": true`): byte stores into shared memory as an
  atomic AND and OR in place of the compare-and-swap loop. Changed nothing, and is not needed: with whole index
  vectors the loop works there.
- `0042-mesh-outputs-for-diagnosis.patch` (`"mesh": {"debug": n}`): the entry point replaces the outputs; 1 the most
  primitives whatever the count byte says, 2 one fixed triangle a workgroup, 3 the same without calling the kernel.
- `0043-mesh-indices-stored-whole.patch` (`"mesh": {"whole_indices": 1}`): the fix. Now `patches/0041`.
- `0044-mesh-debug-first-groups.patch`: debug 4 and 5 only a draw's first two workgroups emit (4 the kernel's
  primitives, 5 one fixed triangle without the kernel); 6 a task shader launches nothing, 7 its kernel does not run
  either, 8 it launches at most four mesh workgroups a task workgroup, 9 it is nothing but a launch of zero, 10 as 7
  without the payload copy.

The runtime side of the experiments was environment-driven and is not in the tree: `TOTK_X_DEBUG`, `TOTK_X_WHOLE`,
`TOTK_X_STORES` put the fields into the request (`gfx_shader.c ns2_request`), `TOTK_X_TAG=<n>` gave the task and mesh
programs a hash of their own per experiment (a byte of `build_code_hash`'s buffer), `TOTK_X_LOG` logged each mesh
pipeline's shader keys at creation and each direct mesh draw's count, `TOTK_X_SKIP_TASK` skipped the draws through a
task shader (`vk_exec_draw.c prepare_stages`). The translator was built aside (`tools/build_xlate.py --out`) and
named by `TOTK_XLATE` with `TOTK_XLATE_ID` set to the kept identity, so the device's cache stayed valid.

Open: the draws through task shaders (the short grass). On that driver the first one that emits a primitive stops
the GPU (a lost device with "No fault detected") or ends in an access violation inside `amdvlk64.dll`, also with
debug 9, a task shader that is nothing but `EmitMeshTasksEXT(0, 1, 1)`. Not tried: the same draws outside a render
condition with the conditions kept on the GPU for everything else, a task shader without the payload variable, and
the mesh shaders' output memory, which with that driver's granularity of 256 is over its 32,768 bytes for the
shaders of eight and more attributes (scratch `payload.py`).
