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
- *Verification (UAT)* is a scenario I play through in GW2: each step is an action in the
  game and, after the arrow, the expected result.
- An issue that a council or review produced links it.

## Branches

- Never push to `gw2-on-mac` or `main` directly. Every change goes through a pull request into
  `gw2-on-mac`. `main` only mirrors upstream: after an upstream sync merges, it is fast-forwarded
  to the synced upstream commit, never forced, and that is the only push it gets.
- Name the branch `<type>/<issue>-<short-description>`, in English `lower-case-kebab-case`, for
  example `fix/1-throttle-foreground-process`. Types: `feat`, `fix`, `perf`, `port` (from upstream
  or from d9mt), `diag`, `exp` (an experiment behind a switch, off by default), `chore`, `docs`.
- One branch is one issue. Measurement variants (`tmp/…`) are never merged.
- **Upstream syncs** merge `upstream/main` into `gw2-on-mac` through their own pull request
  (`chore/sync-upstream-<short sha>`), with a GW2 session before and after. The steps are in
  [upstream-sync.md](../docs/agents/upstream-sync.md).

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
  once that verdict has no open `Blocker` or `High`. An upstream sync is the exception: it merges
  with a merge commit, so upstream's commits stay ancestors. I don't review code by hand.

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

## Code rules

These keep the fork mergeable with upstream. Every line of mine inside one of upstream's files
is a place where the next sync can conflict, so most of the rules are about where code goes.

1. **New code goes into fork-owned files.** `dxmt_gw2_*` files in `src/dxmt`, new files of the
   fork elsewhere (`src/winemetal/unix/mtl4.c` is one), `tests/gw2/`, `tools/gw2/` and
   `.github/gw2/`. Upstream has none of them, so they never conflict.
2. **An upstream file gets a hook, not the feature.** One call, one include or one member per
   feature, each marked `// gw2 (#N):` with the issue's number (`/* gw2 (#N): */` in C; a
   council step names itself too, as in `gw2 (B2, #67)`). `git grep 'gw2 ('` then finds every
   fork line.
3. **Never rewrite an upstream line** for a diagnostic, a rename or a switch read. If a counter
   needs to know why something happened, work it out inside the fork's function, not by
   changing upstream's call sites or signatures.
4. **Stay away from the ends.** Upstream appends at the end of its files, its lists (meson
   sources, includes, tables) and its constructor bodies. Keep at least one untouched upstream
   line between a fork addition and such an end. For a list, add the fork's entries from
   outside it, after upstream's closing bracket. For a constructor, let a fork member's own
   constructor do the work.
5. **Never change upstream's final newline.** A file that upstream ends without a newline keeps
   ending that way, with fork lines above its last line.
6. **Moved upstream code is listed.** A PR that moves upstream code into another file adds a
   row to "Moved upstream code" in [upstream-sync.md](../docs/agents/upstream-sync.md), so a
   sync knows where an upstream fix to it has to go.
7. **A switch is declared by its module.** `sw::Switch` at namespace scope in the file that
   reads it, with its grammar, never a new `env::getEnvVar("DXMT_…")`
   ([ADR 0004](../docs/gw2/adr/0004-switches-declared-by-their-module.md)). The bridge's unix
   side is C and reads with `getenv`; its README row says so.
8. **No `thread_local` on a per-call path.** The x86_64 and i386 DLLs are built with mingw-GCC,
   whose `thread_local` is emulated TLS: several function calls per access, about 0.1 µs under
   Rosetta. At 21,000 timing scopes a frame that cost the render thread 2.3 ms (#28, #37). Keep
   per-thread state in an object the thread already holds, as `ApiThreadCache` does
   (`src/dxmt/dxmt_gw2_stats.hpp`), or pass it in.
9. **arm64-clean.** No x86 intrinsics and no inline assembly. The same source builds the ARM64X
   DLLs for the arm64 engine.

The `GW2` workflow prints the fork's footprint in upstream's files for every pull request: the
files it changes, the lines it adds there, and the upstream lines it rewrites or deletes. A PR
that raises the removed lines says why in its description.

The decisions these rules rest on, and the ones a change must stay consistent with:

- [ADR 0001](../docs/gw2/adr/0001-one-compile-job-per-key.md): one compile job per key; warming
  and demand share it.
- [ADR 0002](../docs/gw2/adr/0002-shared-submission-and-completion-semantics.md): Metal 3 and
  Metal 4 share submission and completion semantics; encoding stays specific to each.
- [ADR 0003](../docs/gw2/adr/0003-gpu-completion-authorizes-retirement.md): GPU completion
  authorizes retirement.
- [ADR 0004](../docs/gw2/adr/0004-switches-declared-by-their-module.md): switches are declared
  by the module that reads them.
- [ADR 0005](../docs/gw2/adr/0005-bridge-call-numbers-in-one-manifest.md): bridge call numbers
  are explicit in one manifest.
- [ADR 0006](../docs/gw2/adr/0006-nothing-goes-upstream.md): nothing from this fork goes
  upstream, not a pull request, an issue, a comment or a finding.
- [ADR 0007](../docs/gw2/adr/0007-metal-4-changes-merge-with-their-switch-off.md): Metal 4,
  bridge and chunk-submission changes merge with their switch off; a default flips after the
  Metal tests ran on a Mac.
- [ADR 0008](../docs/gw2/adr/0008-0.2-presents-metal-4-frames-through-a-metal-3-command-buffer.md):
  0.2 presents Metal 4 frames through a Metal 3 command buffer, and 0.1.x keeps the Metal 4
  present.
- [ADR 0009](../docs/gw2/adr/0009-the-shown-wait-sits-before-the-first-back-buffer-write.md):
  with the Metal 3 present, the next frame waits for `shown` only before it writes the back
  buffer.
- [ADR 0010](../docs/gw2/adr/0010-a-tested-switch-is-deleted-or-becomes-the-default.md): a
  switch whose test is over is deleted or becomes the default.
- [ADR 0011](../docs/gw2/adr/0011-the-pso-archive-is-deleted.md): the PSO archive is deleted.
- [ADR 0012](../docs/gw2/adr/0012-fork-code-stays-out-of-upstream-files-and-off-their-ends.md):
  fork code stays out of upstream's files, and off the ends upstream appends to.
- [ADR 0013](../docs/gw2/adr/0013-a-push-run-skips-tests-a-green-pull-request-ran.md): a push
  run skips the native tests a green pull request ran on the same tree.
- [ADR 0014](../docs/gw2/adr/0014-the-project-rules-that-bind-this-fork.md): the project's
  rules that bind this fork, each pointing to the workspace ADR that holds it.
- [ADR 0015](../docs/gw2/adr/0015-compile-service-tests-without-the-old-scheduler.md): the
  compile service is tested on its own, without the old scheduler.
- [ADR 0016](../docs/gw2/adr/0016-ccache-for-pull-request-builds.md): pull requests compile
  through a ccache that only pushes save, and pushes don't read it.
- [ADR 0017](../docs/gw2/adr/0017-the-shown-wait-before-the-first-back-buffer-write-is-the-default.md):
  the shown wait before the first back-buffer write (ADR 0009) is the default.
- [ADR 0018](../docs/gw2/adr/0018-a-buffer-class-without-sparse-buffer-tier-gets-it-from-the-bridge.md):
  a buffer class without `sparseBufferTier` gets it from the bridge, returning
  `MTLBufferSparseTierNone`; no fallback, no switch.

## Definition of Done

- [ ] **Clean build.** The `GW2` workflow (`.github/workflows/gw2.yml`) is green on the pull
      request. It builds x86_64 and i386 the way `scripts/build-dxmt.zsh` (GW2 on Mac) does,
      without new warnings, builds arm64 for Mach9's engine and runs the native tests. That run is the build gate.
      A local build is only for installing and playing, and the run's artifact can replace it
      ([CI](../docs/gw2/README.md#ci)).
- [ ] **Logged and switchable.** The change writes a log line when it acts (once, or on each
      state change, never per draw), and a behaviour change has a `DXMT_*` off-switch that
      restores the base behaviour, declared with `sw::Switch` in its module
      ([Code rules](#code-rules)). Both are in [docs/gw2/README.md](../docs/gw2/README.md).
- [ ] **Measured in GW2.** STATS lines before and after, in the same place with the same
      settings (a quiet spot and Lion's Arch for anything on the frame path), alternating base
      and branch where the difference is small. Also the off-switch run. Never measure while
      anything else heavy runs, and never with the background throttle engaged.
- [ ] **Nothing measurably slower or worse to look at.** A visual change is checked where it
      matters (fire/smoke, stealth, Cursed Shore, the Fractal lobby for shader changes).
- [ ] **Bridge changes say so.** A change to `src/winemetal` changes the unix side and the PE
      side together; the PR says so, and the two always ship from one build. A new bridge call is
      a line appended to the call manifest, `src/winemetal/winemetal_calls.h`, never a number
      handed out by hand ([ADR 0005](../docs/gw2/adr/0005-bridge-call-numbers-in-one-manifest.md)).
- [ ] **arm64-clean.** No x86 intrinsics or inline assembly in new code (the fork must build as
      ARM64X before macOS 28).
- [ ] The independent agent review has no open `Blocker` or `High`, and its verdict is in the PR.
- [ ] The issue's UAT scenario has been played through and ticked off in the issue.

## Never do these things

- Open anything in 3Shain/dxmt from this fork.
- Push directly to `gw2-on-mac`, or to `main` anything but the fast-forward to a synced upstream
  commit.
- Start Wine, Steam or GW2 while I may be playing, or without my go.
- Merge a change that renders differently without the issue asking for it.
- Commit logs, captures or traces with account data.
