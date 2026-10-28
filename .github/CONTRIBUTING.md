# Contributing to jdrly/dxmt (the GW2 on Mac fork)

This fork changes DXMT for one game: Guild Wars 2 (Steam, `Gw2-64.exe`), run by GW2 on Mac in a
CrossOver 26.3 engine on an M5 Max at 3840×2160 and 60 Hz. The rules follow
[d9mt](https://github.com/jdrly/d9mt), SWTOR on Mac and SWGL on Mac. Upstream's own
[CONTRIBUTING.md](../CONTRIBUTING.md) still describes DXMT's code; its AI policy applies to
3Shain/dxmt, which receives nothing from here (see [AGENTS.md](../AGENTS.md)).

The fork's code is LGPL-2.1-or-later like DXMT's. By contributing you agree that your change is
licensed the same way.

## Sources of truth and reading order

1. **[AGENTS.md](../AGENTS.md)**: what the fork is, and what never goes upstream.
2. **[docs/gw2/README.md](../docs/gw2/README.md)**: every `DXMT_*` switch the fork adds and every
   field of its log lines.
3. **The GW2 on Mac workspace**: `AGENTS.md` and `docs/research/` (the traces and measurements
   that every change here is based on).
4. **The code**: `src/d3d11` (front end, swapchain), `src/dxmt` (command queue, encoding,
   presenter, statistics), `src/winemetal` (the Wine bridge, its unix side), `src/airconv`
   (shader compiler).

If two sources conflict, report the conflict instead of silently choosing one.

## Issues

- Every change that isn't trivial starts from an issue: a **task** (a precise specification)
  or a **bug report**.
- Titles use `Context: Actionable result`, for example
  `Background throttle: Key on the foreground process, not the window`. The issue, the pull
  request and the squash commit carry the same title; a measured result may follow in
  parentheses (`Present deadline: … (Lion's Arch 39 → 47 fps)`).
- The area is a label, never a title prefix ([triage labels](../docs/agents/triage-labels.md)).
- *Affected areas* lists only paths you have verified in the repository.
- *Verification (UAT)* is a scenario Jan plays through in GW2: each step is an action in the
  game and, after the arrow, the expected result.
- An issue that a council or review produced links it.

## Branches

- Never push to `gw2-on-mac` or `main` directly. Every change goes through a pull request into
  `gw2-on-mac`. `main` only mirrors upstream.
- Name the branch `<type>/<issue>-<short-description>`, in English `lower-case-kebab-case`, for
  example `fix/1-throttle-foreground-process`. Types: `feat`, `fix`, `perf`, `port` (from upstream
  or from d9mt), `diag`, `exp` (an experiment behind a switch, off by default), `chore`, `docs`.
- One branch is one issue. Measurement variants (`tmp/…`) are never merged.
- **Upstream syncs** merge `upstream/main` into `gw2-on-mac` through their own pull request
  (`chore/sync-upstream-<short sha>`), with a GW2 session before and after.

## Commits

- Write commit messages in **English**, in the imperative.
- The subject follows the title form, `Context: what the commit does`. No emoji, no scope tags.
- The body says why, with the evidence: what failed before, what was measured, what was not
  done. It also says that an AI agent wrote the change when one did.

## Pull requests

- The PR title is **identical to the issue title**.
- Fill in the [template](PULL_REQUEST_TEMPLATE.md), with `Closes #<issue>`. Its *Evidence*
  section carries the STATS lines before and after, the run conditions, and the off-switch check.
- Keep the PR a **draft** until it meets the Definition of Done.
- One change per PR. Don't mix a fix with unrelated refactoring, and don't bundle independent
  optimizations: each is measured on its own. Logging that serves several issues may be one PR
  when the issues say so.
- **Review and merge.** Every PR gets an **independent agent review** (preferably
  GPT-6.1-Sol) by an agent that didn't write the change; its verdict goes into the PR. The PR is
  **squash-merged**, with the PR title as the commit subject and the description as its body,
  once that verdict has no open `Blocker` or `High`. The maintainer doesn't review code by hand.

## Code review

- Read the issue and its UAT before the diff.
- The review ends in the verdict `MERGE` or `FIX FIRST`, often with a condition.
- Classify every finding by severity. `Blocker` and `High` prevent the merge; `Medium` and `Low`
  may merge with a follow-up issue.

  | Severity | Meaning |
  |---|---|
  | `Blocker` | The pull request must not merge until it is resolved. |
  | `High` | Materially wrong behaviour, or a hard project-rule violation. |
  | `Medium` | Significant duplication, overengineering or maintainability risk. |
  | `Low` | Local cleanup that can go with related work. |

- Here a finding is a `Blocker` by default when it can **change what GW2 draws** without the
  issue asking for it, **release a Metal object the GPU may still use**, **stall or deadlock the
  render or encode thread**, **make GW2 measurably slower**, or **change behaviour with no log
  line and no off-switch**.
- A finding describes one problem with one fix boundary. Findings stay in the PR.

## Definition of Done

- [ ] **Clean build.** `scripts/build-dxmt.zsh` (GW2 on Mac) builds x86_64 and i386 from the
      branch without new warnings, and installs it into the engine.
- [ ] **Logged and switchable.** The change writes a log line when it acts (once, or on each
      state change, never per draw), and a behaviour change has a `DXMT_*` off-switch that
      restores the base behaviour. Both are in [docs/gw2/README.md](../docs/gw2/README.md).
- [ ] **Measured in GW2.** STATS lines before and after, in the same place with the same
      settings (a quiet spot and Lion's Arch for anything on the frame path), alternating base
      and branch where the difference is small. Also the off-switch run. Never measure while
      anything else heavy runs, and never with the background throttle engaged.
- [ ] **Nothing measurably slower or worse to look at.** A visual change is checked where it
      matters (fire/smoke, stealth, Cursed Shore, the Fractal lobby for shader changes).
- [ ] **Bridge changes say so.** A change to `src/winemetal` changes the unix side and the PE
      side together; the PR says so, and the two always ship from one build.
- [ ] **arm64-clean.** No x86 intrinsics or inline assembly in new code (the fork must build as
      ARM64X before macOS 28).
- [ ] The independent agent review has no open `Blocker` or `High`, and its verdict is in the PR.
- [ ] The issue's UAT scenario has been played through and ticked off in the issue.

## Never do these things

- Open anything in 3Shain/dxmt from this fork.
- Push directly to `gw2-on-mac` or `main`.
- Start Wine, Steam or GW2 while Jan may be playing, or without his go.
- Merge a change that renders differently without the issue asking for it.
- Commit logs, captures or traces with account data.
