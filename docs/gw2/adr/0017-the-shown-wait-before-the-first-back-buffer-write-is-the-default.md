# The shown wait before the first back-buffer write is the default

Decided 2026-11-17. Status: accepted.

ADR 0009 moved the Metal 4 queue's wait for a Metal 3 present's `shown` from right after the present
to the first encoder that writes the back buffer, behind `DXMT_MTL4_SHOWN_WAIT=write`, off in my
fork and on in my dev builds. It ended the frame dips in combat on 0.2. dev.18's short session held
60 in 20 of the 25 foreground windows that had a GPU frame over 16.7 ms, where dev.17 held 4 of 83.
dev.20's 10-minute session held 135 of 138, with none of its 6 dips coming from a long GPU frame.
That is the test ADR 0010 asks for, and it won, so `write` is the default now (#332).

## Consequences

- The switch's grammar turned around: exactly `next` gives back the old placement, and unset, empty
  and any other text mean `write`. Until #332 only exactly `write` turned it on. `next` stays, so a
  bisect can still put the wait right after the present.
- It acts only with `DXMT_MTL4_PRESENT=mtl3` on Metal 4, and only the app's 0.2 sets that (ADR
  0008). 0.1.x runs the Metal 4 present and doesn't change. Its startup line does: it shows
  `DXMT_MTL4_SHOWN_WAIT=unused` where it showed `next`.
- On 0.2 the `Metal 4: the next frame waits for a Metal 3 present only before it writes the presented
  texture` line and the `METAL4 shown` field appear without the variable. The app's dev switches
  table can drop its `write` row once the app takes a build with this.
- The flip touches the Metal 4 path, so it merges after `tests/gw2/run.zsh pure metal` ran on a Mac
  (ADR 0007), although no Metal test reads this switch.

## Evidence

- #271 and PR #272 (`79e9cea`): the switch, `tests/gw2/shown_wait_test.cpp`, and the dev.18 numbers.
- GW2 on Mac workspace `plans/night/2026-11-11-plan.md`, entries of 2026-11-16 12:25 (dev.18) and
  2026-11-17 02:03 (dev.20); logs in `work/arm64-0.2/logs/arm64-2026-11-16-dev18/` and
  `arm64-2026-11-17-dev20-10min/`.
- ADR 0009 (the placement) and ADR 0010 (a tested switch becomes the default).
