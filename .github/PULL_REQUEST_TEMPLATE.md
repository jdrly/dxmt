<!-- Needs the Mac run: a PR that touches one of these paths starts with the line below and claims no
result of it. CONTRIBUTING's "The Mac run" names the seven tests. Any other PR deletes the line.

- `src/winemetal/`, but not `src/winemetal/unix/cache.c` or `src/winemetal/unix/visibility_slots.h`
- `src/dxmt/dxmt_gw2_mtl4_*`
- `src/dxmt/dxmt_gw2_submission.*`
- `src/dxmt/dxmt_presenter.*`
- `src/dxmt/dxmt_gw2_present.*`
- `src/dxmt/dxmt_command.metal` and `src/dxmt/dxmt_command_constants.hpp`
- `src/dxmt/dxmt_gw2_lifetime.*`
- `src/dxmt/dxmt_gw2_residency.*`
- the seven tests' own files in `tests/gw2/`
- `tests/gw2/run.zsh`
-->
Needs the Mac run: `tests/gw2/run.zsh metal`

## What changes

<!-- Two to five sentences: what this PR does and why, not a list of files. Say that an AI agent wrote it, if one did. -->

Closes #

## Switches and log lines

<!-- Every DXMT_* switch this adds or changes (name, default, what off restores) and every new log line or STATS field. They are also in docs/gw2/README.md. Write "none" if there are none. -->

## Evidence

<!-- The GW2 runs: build (fork commit), where (a quiet spot, Lion's Arch, the scene the issue names), settings, NCPU, how many runs and in which order. STATS lines before and after, and with the off-switch. A visual change: what was checked where. -->

## How to verify

<!-- Steps for me: what to start, where to go in GW2, what to look at, which log line shows it. -->

## Definition of Done

- [ ] The `GW2` workflow is green: x86_64 without new warnings, arm64, the native tests.
- [ ] The change logs when it acts, and a behaviour change has a `DXMT_*` off-switch; both are in `docs/gw2/README.md`.
- [ ] Measured in GW2: STATS before and after in the same place and settings, plus the off-switch run.
- [ ] Nothing measurably slower or worse to look at.
- [ ] A `src/winemetal` change is called out, and both sides ship from one build.
- [ ] New code is arm64-clean (no x86 intrinsics or inline assembly).
- [ ] The independent agent review (preferably GPT-6.1-Sol) has no open `Blocker` or `High`, and its verdict is in the PR.
- [ ] The issue's UAT scenario has been played through and ticked off in the issue.

## Review

<!-- An independent agent (preferably GPT-6.1-Sol) reviews this PR and puts its verdict here or in a comment. Squash-merge into gw2-on-mac (subject = PR title) after a clean verdict. -->

- **Verdict:** pending

## Risks and what I didn't do

<!-- Known limits, deferred work, what the off-switch doesn't cover. An empty field is an answer too: write "none". -->
