# Metal 4, bridge and chunk-submission changes merge with their switch off

Decided 2026-11-11, narrowed 2026-11-12 and 2026-11-19. Status: accepted.

GitHub's runners have no Metal 4, and by day the agents work on my Linux server, which has no macOS.
So a change to the Metal 4 path, the bridge (`src/winemetal`) or chunk submission can be built and
reviewed without me, but its native Metal tests can't run before it merges. Until 2026-11-11 such a
pull request waited for a test run on my Mac. Since then it merges when its review approves and the
`GW2` run is green, on one condition: its new behaviour is off by default, so the default path is
the old code. Its Metal tests go on a list for a Mac instead of gating the merge, and a default
flips to on only after they ran on one. A Mac here is any Mac with Metal 4: my M5, or the M1 I
rented on 2026-11-12, whose first use was these tests.

## Consequences

- A pull request that argues for "on" has its default changed to off first. #154 was the first
  (`DXMT_MTL4_TIMESTAMP_REUSE`, PR #189).
- The top of such a pull request says "Needs the Mac run" with the exact commands, and claims none
  of them.
- Since 2026-11-19 (#348) the Mac run is `tests/gw2/run.zsh metal`, and only a pull request that
  touches one of these paths owes it, whatever its issue says:

  - `src/winemetal/`, but not `src/winemetal/unix/cache.c` or `src/winemetal/unix/visibility_slots.h`
  - `src/dxmt/dxmt_gw2_mtl4_*`
  - `src/dxmt/dxmt_gw2_submission.*`
  - `src/dxmt/dxmt_presenter.*`
  - `src/dxmt/dxmt_gw2_lifetime.*`
  - `src/dxmt/dxmt_gw2_residency.*`
  - the eight tests' own files in `tests/gw2/`
  - `tests/gw2/run.zsh`

  The eight tests are the ones that show something only on a Mac with Metal 4, listed in
  [CONTRIBUTING](../../../.github/CONTRIBUTING.md#the-mac-run). `thread_priority_test` needs a real
  Mac, not Metal 4, and is registered as `metal` so that run keeps it. At `28e574e` the runner
  printed passed 38, skipped 8 (run 44603994415) and the rented M1 passed 46, skipped 0: the 34
  pure tests printed the same on both, so a Mac run of them repeats the runner. Of the 85 pull
  requests merged between 2026-11-11 and the audit on 2026-11-19, at most 34 touched one of these
  paths.
- A Mac run shows nothing about the PE-only code, `src/dxmt/dxmt_context.*`,
  `src/dxmt/dxmt_command_queue.*` and `src/d3d11/d3d11_*`, because no native test compiles it. A
  change there owes a session in the game.
- The change ships in the next 0.2 dev build, which turns every switch on (the app's ADR on dev
  builds). I play a session with it. Its issue stays open for the Metal run and that session.
- A change that can fault the GPU or change lifetimes always ships switch-off.
- A default flips after both: #153, #154 and #204 became on by default with #301 (PR #302,
  `5a185fb`, 2026-11-17) after a 2.6-hour session with all three on and the Metal run on the rented
  M1 (`run.zsh pure` 34 of 34, `metal` 10 of 10, x86_64 and arm64). #301's criterion says "on my
  Mac". The rented M1 meets it for the tests. The issue's UAT, a session on my M5 with the switches
  unset, is still mine.
- One exception, from #205 on 2026-11-12. A compile-time change of form in the bridge that leaves
  what runs unchanged, and can only fail at the first call before anything draws, needs no switch.
  It merges approved and green. Its rollback is the previous dev build and a revert.

## Alternatives I rejected

- **Holding the merge for the Mac's Metal tests.** This was the rule before, and #185 waited under
  it. The record has my decision to drop it, made when I moved the shift to the server and asked for
  the next dev build from GitHub's runners. My reason in my own words is not recorded.
- **A switch for every bridge change, #205 included.** The same unix function runs with the same
  arguments there, so a switch would guard nothing.

## Evidence

- GW2 on Mac workspace `plans/night/2026-11-11-plan.md`: 2026-11-11 19:08 ("My four decisions"),
  2026-11-12 20:50 (#205 merges without a switch), 2026-11-17 00:25 to 01:07 (#301's Mac run).
- The dispatch project file `dxmt.md`, "Merge".
- #154 (PR #189), #185 (PR #192), #205 (PR #213), #301 (PR #302).
- GW2 on Mac workspace `docs/research/dxmt-metal-run-2026-11-12.md` (the Metal runs on the rented
  M1).
- GW2 on Mac workspace `advisor-plans/2026-11-19-ci-audit/AUDIT.md`, recommendation 3 (the Mac
  run's paths, #348).
