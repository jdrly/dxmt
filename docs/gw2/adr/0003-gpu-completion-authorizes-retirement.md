# GPU completion authorizes retirement

A chunk's holds are released only when the chunk retires: after the submission's wait for that chunk
(Metal 3: its command buffers completed; Metal 4: its commits' feedback and its completion event), in
a fixed cross-category order with readbacks before allocator reset, never because a CPU reference
count fell to zero. Metal 4 feedback and unretained resources outlive submission, and shared ownership
alone can't express that ordering. (The council's first draft, "hold whether or not Metal retains", is
withdrawn: unconditional holds would be a separate, measured decision.)

## Consequences

- L1 (#66) moves the chunk's typed keep lists, `split_cmdbufs` and the `KeepUntilRetired` overloads
  into one `RetirementLedger` per command queue (`src/dxmt/dxmt_gw2_retirement.hpp`), with the same
  gates at the call sites and the same order, and writes the finish thread's retirement once
  (`CommandQueue::RetireChunk`): the lifetime check's completion, the chunk's own state (readbacks,
  command list, allocation tracking, the last command buffer), the ledger (split command buffers,
  objects, presenters, spatial and temporal scalers), Metal 4's allocator, the CPU fence, the ring
  count, the ring allocators. Teardown drains chunk by chunk in the same order.
- The ledger stamps each ring slot with the chunk sequence that owns it, so a hold into a ring slot
  that another unretired chunk owns (a stale generation) or for a chunk that already retired is
  refused, and so is a repeated or out-of-order retirement. A refused hold is kept until teardown,
  never released early; a refusal is a bug and is logged.
- A parity fixture (`tests/gw2/retirement_ledger_test.cpp`) holds the ledger to the keep lists' release
  traces it replaced, for rings of 32, 64 and 128, teardown included.
- L2 (#69) moves the mandatory-vs-conditional decision into the ledger: every hold names a
  `HoldReason`, and the queue's `RetentionPolicy` (Metal 4, `DXMT_UNRETAINED`, the lifetime check, set
  once the queue has picked Metal 3 or Metal 4) decides; the call sites' `keep` gates are gone. The
  present split's buffers are always held; everything else is held unless Metal 3's retained command
  buffers hold it. A declined hold leaves the object with the caller, released where it always was,
  and a borrowed drawable is retained only when it is held. The test holds the policy to each former
  gate in every reachable configuration. (Metal 4 without `DXMT_UNRETAINED=1` or the check, which
  `metal4AttachQueue` refuses, is the one unreachable case where the two differ: the policy holds
  everything on Metal 4.)
- Still to come: L3, with the submission seam, makes retirement take the completed-chunk token from the
  submission's `wait` instead of a sequence number, and gives teardown one common drain. Allocation
  tracking, the ring allocators and the handshake counters stay where they are.
- Native tests don't replace in-game evidence: the #31 lifetime check (`DXMT_LIFETIME_CHECK=1`) stays
  the proof that nothing is released early in GW2.

Source: GW2 on Mac `council/2026-11-04-arch-grill/final.md` §3 (ADR draft 0003).
