## What changes

<!-- Two to five sentences: what this PR does and why, not a list of files. Say that an AI agent wrote it, if one did. -->

Closes #

## Switches and log lines

<!-- Every DXMT_* switch this adds or changes (name, default, what off restores) and every new log line or STATS field. They are also in docs/gw2/README.md. Write "none" if there are none. -->

## Evidence

<!-- The GW2 runs: build (fork commit), where (a quiet spot, Lion's Arch, the scene the issue names), settings, NCPU, how many runs and in which order. STATS lines before and after, and with the off-switch. A visual change: what was checked where. -->

## How to verify

<!-- Steps for Jan: what to start, where to go in GW2, what to look at, which log line shows it. -->

## Definition of Done

- [ ] `scripts/build-dxmt.zsh` builds x86_64 and i386 without new warnings.
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
