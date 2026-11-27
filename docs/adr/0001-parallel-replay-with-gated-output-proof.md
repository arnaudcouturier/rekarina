# Keep Parallel Replay Active With Gated Output Proof

Under **Host HDR**, only draw stretches targeting a **Window Target** run serially so **Parallel Replay** remains active for off-screen scene work. When porting **Output Proof** tracking from `clshortfuse/rekarina`, we gate proof recording, per-draw overwrite checks, and replay serialization strictly behind **Guest PQ** (or `RT_TRACE_GFX`) using an inline zero-cost fast path, and serialize only stretches that alias a pending proof target rather than every draw after scene encode.

## Considered Options

- **Unconditional Output Proof checks (ShortFuse's approach)**: Runs `vk_hdr_output_pending()` at the top of `par_try()` and calls `hdr_output_draw_written()` on every draw on every `OoT` frame, serializing post-encode draws and adding cross-TU atomic overhead even in SDR and **Host HDR** (~5 ms/frame regression in busy scenes when combined with whole-frame HDR serialization).
- **Gated fast path + Window Target serialization (chosen)**: Preserves zero per-draw cost and full **Parallel Replay** in SDR and **Host HDR**, while retaining strict **Output Proof** verification whenever **Guest PQ** or graphics tracing is active.

## Consequences

- Any future feature that depends on **Output Proof** outside **Guest PQ** must explicitly opt into arming the fast-path gate.
- `REKARINA_HDR_SERIAL=1` remains available as a diagnostic fallback to force whole-frame serial replay.
