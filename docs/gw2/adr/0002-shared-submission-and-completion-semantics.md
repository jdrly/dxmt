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
- Q2 (#78) puts both backends behind `ChunkSubmission` (`src/dxmt/dxmt_gw2_submission.hpp`): begin,
  waitEvent, signalEvent, encode, commit on the encode thread; `wait` returns a `CompletedChunk` (the
  records, valid until retired) and `retire` takes it back on the finish thread; `drain` at teardown.
  `Metal3Submission` and `Metal4Submission` (which owns the `Mtl4Queue`) are the production adapters,
  chosen once per command queue; the `if (mtl4_)` sites in `dxmt_command_queue.cpp` are gone.
- The council's interface gained one call, `encode(seq, ChunkEncoder &)`: the adapter hands the
  chunk's encoder its own target (a Metal 3 command buffer, or the `Mtl4Queue`), so the compile-time
  encoding seam stays where it was, at one virtual call per chunk. Metal 4's `begin` opens nothing
  (command buffers begin as the encoders need them), and its `wait` also waits for the chunk's
  completion event, the chunk's last queue operation, which `wait` receives as a value.
- Metal 3's chunk keeps owning its last command buffer and releases it when it resets, before the
  ledger's holds, as ADR 0003's order says; the adapter only borrows it until `retire`. The present
  split still commits inside the encoding (one `split()` is Q3's).
- `tests/gw2/chunk_submission_test.mm` runs both production adapters on real Metal through the real
  bridge table (empty chunks, 0, 1 and 3 splits, event waits, residency before every commit, timing,
  drain), and a deterministic scripted adapter for error records, missing times and the token's
  lifetime.
- L3 (#91): the submission is the issuer of completion tokens. `wait` returns a `CompletedChunk`
  that only it can make, the retirement ledger releases a chunk's holds only with it, and `retire`
  (now a non-virtual check around the adapter's `retireChunk`) takes it back only if it is this
  submission's token for its last wait. `drain` returns a drained-queue token that the ledger's
  teardown drain needs (ADR 0003).
- Still to come: Q3 (one `split()` and one present-mode function) waits for lane G's Metal 4 latency A/B. #33
  adds real GPU timestamps as a record field, not as a third readback kind.

Source: GW2 on Mac `council/2026-11-04-arch-grill/final.md` §2 (ADR draft 0002); Q1 implemented in
jdrly/dxmt #77, Q2 in #78.
