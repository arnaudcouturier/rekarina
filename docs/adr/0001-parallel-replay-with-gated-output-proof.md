# Keep Parallel Replay Active With Gated Output Proof

Under `TotK` **Host HDR**, only draw stretches targeting a **Window Target** run serially so **Parallel Replay** remains active for off-screen scene work. On `OoT`, where both Vanilla HDR and RenoDX HDR output through **Guest PQ** and `vk_hdr_track` is unused, **Output Proof** (`vk_hdr_written_output()` and the captured PQ matrix) is only consumed when **Guest PQ** and Frame Generation are active together (for HUD-less PQ encoding) or when `RT_TRACE_GFX` is enabled. We therefore gate proof recording, per-draw overwrite checks, and proof-target replay serialization behind an inline zero-cost fast path armed strictly under `OoT` HDR + Frame Generation or `RT_TRACE_GFX`, and serialize only stretches that alias a pending proof target rather than every draw after scene encode.

## Considered Options

- **Unconditional Output Proof checks (ShortFuse's approach)**: Runs `vk_hdr_output_pending()` at the top of `par_try()` and calls `hdr_output_draw_written()` on every draw on every `OoT` frame, serializing post-encode draws and adding cross-TU atomic overhead even in SDR and non-FG HDR (~5 ms/frame regression in busy scenes when combined with whole-frame HDR serialization).
- **Gated fast path + narrowed HDR+FG arming (chosen)**: Preserves zero per-draw cost and full **Parallel Replay** in SDR and non-FG HDR on `OoT` (and off-screen work under `TotK` **Host HDR**), while retaining strict **Output Proof** verification whenever HUD-less PQ capture or graphics tracing requires it.

## Consequences

- Any future feature that depends on **Output Proof** outside `OoT` HDR + Frame Generation must explicitly opt into arming the fast-path gate.
- `REKARINA_HDR_SERIAL=1` remains available as a diagnostic fallback to force whole-frame serial replay.
