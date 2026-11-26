# A switch whose test is over is deleted or becomes the default

Decided 2026-11-16. Status: accepted.

Every behaviour of the fork sits behind a switch its module declares (ADR 0004), off by default, and
my dev builds turn them all on. That is how a change gets tested. It was never meant to keep every
experiment forever. The audit of 2026-11-16 counted what the finished ones still cost: thousands of
lines, hooks in upstream files, branches and checks on per-present and per-call paths, and conflicts
with upstream's branches. My rule after reading it: what was tested and lost goes, and what was
tested and is on stays and becomes the default. The audit's brief has the wider form that the
deletions follow: a switch whose experiment is over, and that still costs code or a hot-path check,
goes.

## Consequences

Deleted, each with its code, tests, log fields and README rows:

- The PSO archive, four switches (#275, ADR 0011).
- `DXMT_MTL4_PRESENT=done` and `DXMT_MTL4_LAYER_SET`, the two #223 test switches (#277). `done` was
  played and left the sawtooth unchanged. The layer set was a diagnostic I never needed.
- `DXMT_PROMOTE_FLUSH_MIN`, `DXMT_PROMOTE_FLUSH_MS` and `DXMT_TIMESTAMP_MODE=boundary` (#279). GW2
  issues no occlusion queries, and since #26 a timestamp `End` requests no commit, so the deferral
  had nothing to hold in 4,767 windows.
- `DXMT_PRESENT_QUEUE` (#281). `two` can latch 30 fps, which my rule against caps below 60 bars.
  `pace` gave up at frame 181 in all three sessions it ran in.
- `DXMT_WARM_ORDER` (#285). `rowid` failed the warming UAT of 2026-11-04, and `use` was already the
  default.

Became the default:

- The one-call `UpdateSubresource` path, on in every session since 2026-11-11. Its switch
  `DXMT_UPDATE_ONE_CALL` is gone, and the path follows `DXMT_MAP_FAST` (#287).
- `DXMT_MTL4_DEVICE_RESIDENCY`, `DXMT_MTL4_TIMESTAMP_REUSE` and `DXMT_MTL4_ALLOCATOR_POOL` (#301, PR
  #302). They keep their switch, and `=0` restores the old path.

What follows from the rule:

- A deleted bridge call's number becomes a `WMT_RESERVED` line (ADR 0005). Numbers are not reused.
- A value that lost reads as the default afterwards: `DXMT_MTL4_PRESENT=done` now means `mtl4`.
- The app's dev switches table drops a row before the fork deletes its switch (jdrly/gw2-on-mac
  #212). Dropping the rows of the three Metal 4 switches that are now on by default was the audit's
  plan 009, an extra I didn't ask for. It landed on 2026-11-17 (jdrly/gw2-on-mac #243, PR #244).
- A flip to on in the Metal 4 path still waits for the Metal tests on a Mac (ADR 0007).
- Three deletions fall under that wider form and not under the narrower one. The layer set was a
  diagnostic I never needed once the cause of #223 was known. The promote deferral had nothing to
  hold. `pace` gave up at frame 181 in every session, so it never acted. I let all three run as
  plans 003 to 005, wave by wave. #157 and #221, the issues for `DXMT_PRESENT_QUEUE` and its test,
  closed as not planned on 2026-11-17, with a pointer to #281.
- Not decided yet: `DXMT_RENDER_THREAD_PRIORITY`, `DXMT_CONTROL`, and `DXMT_WARM=functions`. The
  AIR reader pool (`DXMT_AIR_READERS`) was dropped on 2026-11-26 (ADR 0019).

## Alternatives I rejected

- **Keeping every switch.** A finished experiment still costs code in upstream files and a check on
  a hot path, and each one is a place where an upstream merge can conflict.
- **Deleting the native Metal 4 present with them.** 0.1.x ships it (ADR 0008).

## Evidence

- GW2 on Mac workspace `plans/night/2026-11-11-plan.md`, 2026-11-16 13:20 (my rule after the
  vetting) and the waves that follow; `advisor-plans/2026-11-16-forks/BRIEF.md` ("Decided, not
  findings"), `vetted-A-dxmt.md` (ARCH-01, -02, -03, -09, -12, EFF-01, EFF-05), `plans/README.md`
  (plans 002 to 010).
- #275 (PR #276), #277 (PR #278), #279 (PR #280), #281 (PR #282), #285 (PR #286), #287 (PR #288),
  #301 (PR #302).
- jdrly/gw2-on-mac #212 (PR #216), #243 (PR #244).
