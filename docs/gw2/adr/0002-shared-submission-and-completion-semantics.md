# Shared submission and completion semantics, backend-specific encoding

Metal 3 command-buffer completion and Metal 4 commit feedback satisfy one chunk lifecycle (begin,
commit, wait for a completed chunk, retire, drain) behind a queue-level runtime seam. Encoding stays
specific to each backend behind the existing compile-time template (`flushCommandsImpl` over
`WMT::CommandBuffer` and `gw2::Mtl4Encoding`). A universal encoder backend would put dispatch on the
per-draw path and enlarge the interface without helping #33's completion consumers. Both backends stay
in production: Metal 4 is the default (#54), and Metal 3 is the fallback that MetalFX temporal and the
spatial swapchain need.

## Consequences

- Q1 (#77) gives both finish paths one description of a completed commit, the commit record
  (`src/dxmt/dxmt_gw2_commit_record.hpp`): its ordinal, its GPU start and end as `std::optional` (an
  unknown time is not 0), its error, and on Metal 3 its logs. Metal 3 builds the records on the finish
  thread from the chunk's split buffers and its last buffer (waiting as before, reading the error and a
  split buffer's logs only on a failure, the GPU times only with `DXMT_STATS`); Metal 4 converts its
  commits' feedback. One report reads them for both: the device-error lines (the split buffers'
  throttled), the GPU line's spans and PRESENT's aftergpu.
- The report keeps each backend's line order (Metal 3 logs the last buffer's error first, Metal 4 in
  commit order), because Q1 changes no behaviour. Unifying the two is a separate, visible change.
- A parity fixture (`tests/gw2/commit_record_test.cpp`) holds the records to a copy of the two finish
  bodies they replaced (gw2-on-mac 13007ad), on scripted chunks with errors, logs, missing times and
  the throttle; `tests/gw2/commit_record_metal_test.mm` runs the Metal 3 builder on real command
  buffers.
- Readbacks that need a native handle (the timestamp queries reading a command buffer's `gpuEndTime`
  or an `Mtl4Commit`'s feedback) keep their own path; they are not record fields.
- Still to come: Q2 (#78) puts both backends behind `ChunkSubmission` (begin, commit, waitEvent,
  signalEvent; `wait` returns a completed chunk, `retire`, `drain`) and removes the `if (mtl4_)` sites
  from the command queue; L3 then retires the ledger with the completed-chunk token. Q3 (one `split()`
  and one present-mode function) waits for lane G's Metal 4 latency A/B. #33 adds real GPU timestamps
  as a record field, not as a third readback kind.

Source: GW2 on Mac `council/2026-11-04-arch-grill/final.md` §2 (ADR draft 0002); Q1 implemented in
jdrly/dxmt #77.
