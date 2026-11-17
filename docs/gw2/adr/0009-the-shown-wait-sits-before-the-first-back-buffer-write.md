# With the Metal 3 present, the next frame waits for `shown` only before it writes the back buffer

Decided 2026-11-16. Status: accepted. `DXMT_MTL4_SHOWN_WAIT=write` was off by default in the fork and
on in dev builds; since 2026-11-17 it is the default (ADR 0017, #332).

With `DXMT_MTL4_PRESENT=mtl3` (ADR 0008) the Metal 4 queue waited for the `shown` event right after
each present, before any of the next frame's work. `shown` comes after the Metal 3 present pass, and
that pass waits for its drawable, which a Metal 3 present gets back a refresh late. So frame N+1's
GPU work couldn't start until frame N's drawable was back (`gpugap` 0.00 on 0.2 against 1.20 on
0.1.3). Each frame had one refresh for its GPU work, and a frame over 16.7 ms skipped one. That was
the dip to 58 or 59 in combat where 0.1.3 holds 60 (#271). The wait exists because the swap chain
has a single back buffer, and the next frame mustn't write it while the pass reads it. So I moved
the wait to where it is needed, the first encoder that writes the presented texture. The rest of the
frame runs while the present pass waits for its drawable.

## Consequences

- `DXMT_MTL4_SHOWN_WAIT`, default `next`, is the old placement. `write` acts only with `mtl3` on
  Metal 4 and is read once per process.
- Writes are found in one place: both texture overloads of `ArgumentEncodingContext::access` flag
  the encoder when it names the presented texture with `ResourceAccess::Write`. A write path added
  later is covered without a new hook.
- One texture at a time. A present that reads another texture than the one before it (a second swap
  chain, a resize) waits right after it, as before. A present that falls back to the Metal 4 way
  places the pending wait at once. The review's one Blocker was a lost wait in that case.
- How much it gains depends on where GW2 first writes the back buffer. In the dev.18 session `METAL4
  shown at` read 0.95, so nearly the whole frame runs before the wait.
- dev.18, short session: 20 of 25 foreground windows with a GPU frame over 16.7 ms still held 60,
  against 4 of 83 on dev.17 and 20 of 21 on 0.1.3.
- `aftergpu` stays at 32. It times the present pass, which still waits for its drawable.
- The issue's step 2, a drawable's taken, committed, presented and given-back times in `PRESENT`,
  was not built. The given-back time needs a new bridge callback, and `drawwait`, `inuse`,
  `presented` and `aftergpu` answered the question.

## Alternatives I rejected

- **Copying the back buffer for the present pass.** It frees the back buffer at once, at the price
  of a 4K blit every frame, on the GPU time this frame is short of.
- **Forcing the window server to copy or composite.** It hides the late drawable and adds latency.
  The issue ruled it out before any code was written.
- **Taking `nextDrawable` off the encode thread.** The CPU-side wait is the same on both engines
  (13.1 and 13.2 ms), so it wouldn't touch the difference.
- The log names a third fix, "C", that I also turned down. What it was is not recorded.

## Evidence

- #271 and its comment of 2026-11-16, PR #272 (`79e9cea`): step 1's table, the switch, the test
  `tests/gw2/shown_wait_test.cpp`, "Rejected at step 1".
- GW2 on Mac workspace `plans/night/2026-11-11-plan.md`, entries of 2026-11-16 01:25 to 02:25 and
  12:25 (dev.18 played); logs in `work/arm64-0.2/logs/arm64-2026-11-16-dev17-long/`,
  `x86-2026-11-16-013-0.1.3/` and `arm64-2026-11-16-dev18/`.
- jdrly/gw2-on-mac #202 (the dev switches row).
