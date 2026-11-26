# A Metal 3 present's fences go to the Metal 4 queue, behind a switch

Decided 2026-11-26. Status: accepted, as an experiment (#353).

With `DXMT_MTL4_PRESENT=mtl3` (ADR 0008) the present pass runs in a Metal 3 command buffer, and the
pass's fence waits and its fence update were never encoded. The rest of DXMT still treats the present
as an encoder of the fence chain. `ArgumentEncodingContext::present` gives it an ID and a fence,
`endPass` runs its waits through `FenceLocalityCheck`, and it can be a checkpoint. Later encoders wait
on its fence slot, and the simplification drops their other waits on what the present's summary covers.
On the Metal 4 queue such a wait covers only the slot's update 256 IDs earlier, so the dropped waits
order nothing. Two kinds of encoder are affected: the checkpoint whose window covers the present, 64
to 127 IDs later, and, when the present is a checkpoint itself, every encoder up to the next one.
Since ADR 0017 the next frame's work before its first back-buffer write waits for no event that would
cover the gap either.

`tests/gw2/dependency_compression_test.cpp` shows the hole against its fence oracle. Two scripted cases
(a present on a checkpoint, and the checkpoint that covers a present) and 54 of 120 seeds of random
frames miss dependencies with the Metal 3 present's fences. With the Metal 4 present's fences, or with
the encoder below, none do.

I decided:

- **The fix is one empty encoder.** `DXMT_MTL4_PRESENT_FENCES=1` puts an empty blit encoder on the
  Metal 4 queue that waits on the present's waits and updates its fence. It goes at the end of the
  frame's commit, ahead of the present split's commit, so the frame keeps its two Metal 4 command
  buffers. This is the same shape as `DXMT_PRESENT_SKIP_NIL`'s encoder for a skipped pass (#324).
- **The fence means less than a pass's fence.** It says that what the present waited for has
  finished, not that the pass ran. The only encoders that depend on the pass itself write the back
  buffer, and they wait for `shown` (ADR 0009).
- **Off by default, on in my dev builds.** It isn't shown to be what hangs the M2 Max at the Lion's
  Arch aerodrome or what makes textures flash on my M5 there. The same log has two GPU address faults,
  which an ordering hole doesn't obviously explain, and device residency (#302) and the allocator pool
  (#233) changed between 0.1.4 and 0.2.1 too. ADR 0007 keeps a Metal 4 change off by default until a
  test says otherwise, and ADR 0010 decides afterwards: on by default if the aerodrome A/B shows it
  cures the flashing or the hang, deleted if it doesn't.
- **Diagnostics ship with it.** At a device error on Metal 4 the finish thread logs what ordered the
  failed chunk, the chunk before it and the chunk after it: the encoder positions each Metal 4 command
  buffer began at, the shown wait's position and value, the present's ID, kind and checkpoint state,
  whether its fences reached the queue, and how many encoders waited on a present fence that didn't.
  `DEPS presentcp` and `presentstale` give the base rate. By my estimate a stale wait is in about a
  third of GW2's frames with the switch off, so on its own it doesn't mark a race. The A/B does.

## Consequences

- A chunk on Metal 4 with mtl3 and the switch on encodes one more encoder per present. It has no
  commands, waits on the present's waits (the back buffer's writers, and a checkpoint's window when
  the present is one), and updates one fence.
- `DXMT_MTL4_SHOWN_WAIT=next` also closes the hole, because then every commit after a present waits for
  its `shown`. It brings back the dips ADR 0017 ended, though. It is an A/B setting, not a fix.
- The `Present fences` lines and the two DEPS fields are written whether the switch is on or off, so
  the next device error tells me what ordered the chunk.

## Evidence

- #353: the M2 Max log (`work/player-logs/2026-11-20/m2max-0.2.1-1808.zip` in the GW2 on Mac workspace)
  has 4 errors in a chunk's first Metal 4 command buffer: 2 `Hang` (frames 3110 and 4276) and 2
  `PageFault` (3713, 4108). Two more are `InnocentVictim`: 4108's second Metal 4 buffer, and frame
  3109's Metal 3 present buffer next to the first hang. `METAL4 shown at:0.95`, the 2 s window's
  average, puts the first buffer's end at about 95% of a chunk's encoders, so it holds nearly the
  whole frame.
- The M5 log (`m5max-0.2.1-2003.zip`) has no GPU error at the same spot, where textures flash.
- 0.1.4 (my fork at `e02a4d6`) ran the Metal 4 present, whose pass encodes its fences, and had neither
  `mtl3` nor the shown wait.
