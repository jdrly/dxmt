# Fork code stays out of upstream's files, and off the ends upstream appends to

Decided 2026-11-16. Status: accepted.

I take upstream's work regularly, so the fork must stay mergeable with 3Shain/dxmt. On 2026-11-16
the fork was 135 commits ahead with about 15,000 of its lines inside 114 upstream files. The audit
replayed upstream's history and its open branches against the fork and found that most conflicts
came from where my lines sat, not from what they did. Upstream adds each new bridge call at the end
of `winemetal.h`, `winemetal_thunks.c` and the handlers of `winemetal_unix.c`, and my declarations
and thunks sat at those ends. My rewrite of one upstream line was the last sync's only conflict. So
I set placement rules, and moved the code that broke them.

The rules:

1. Fork code lives in files of its own, with `gw2` in the name where the folder is upstream's. An
   upstream file gets a narrow hook, a call or an include.
2. Nothing of mine sits at the end of a file or a list that upstream appends to. Fork declarations
   and thunks are in fork files that the upstream file includes from its middle. Fork sources join a
   build list as `+= [ … ]` after upstream's list.
3. Where my fork rewrote an upstream line and gained nothing, upstream's line comes back.
4. A file ends as upstream's does. No added final newline.

## Consequences

- #289 gave back the `DXMT_METALFX_SPATIAL_SWAPCHAIN` read, four file ends and the end of
  `dxmt_src`. Upstream's `chore/idl` then conflicts in one file, not two.
- #291 moved the bridge's fork declarations, thunks and two handlers into `winemetal_gw2.h`,
  `winemetal_gw2_thunks.c` and `unix/winemetal_gw2.c`. A new upstream bridge call now conflicts only
  in the two generated call tables of `winemetal_unix.c`, which ADR 0005 keeps as they are.
- #293 moved the Metal 4 wrapper classes into `Metal4.hpp`. `Metal.hpp` went from 479 added lines
  against upstream to about 105.
- None of the three has a switch or a log line. Nothing the game sees changes.
- The weekly upstream watch trial-merges upstream's `main` and open branches into the fork and lists
  the files that would conflict. That report is how I see whether the rules hold.
- Left where it is on purpose: my mid-file additions in the bridge files, and the swap chain's block
  in `d3d11_swapchain.cpp`. A block added in the middle of a class merges clean unless upstream
  edits its edges, and no replayed conflict fell inside it.
- The rules are in `.github/CONTRIBUTING.md`, "Code rules", since the audit's plan 017 (#318, PR
  #319, 2026-11-17). The `GW2` workflow prints the fork's footprint in upstream files for every
  pull request.

## Alternatives I rejected

These three are the audit's verdicts, written into its plan index for me to overrule. I haven't.

- **One include per upstream file for all fork code (MERGE-05).** 47 upstream files at once. Not
  now.
- **Moving the swap chain block into a fork file (ARCH-07).** Present timing is my most sensitive
  path, and the evidence didn't ask for it. I revisit it if the weekly report shows upstream editing
  that block's edges.
- **`git rerere` for repeated conflicts (MERGE-12).** It pays only when trial merges are resolved
  ahead of time.

## Evidence

- #289 (PR #290, `64b3fbb`), #291 (PR #292, `49748bd`), #293 (PR #294, `f231e03`), #299 (PR #300,
  the upstream sync's recipe for a new bridge call), #318 (PR #319, the rules in CONTRIBUTING).
- GW2 on Mac workspace: `advisor-plans/2026-11-16-forks/BRIEF.md` ("Hard constraint"),
  `vetted-A-dxmt.md` (MERGE-01 to -12, ARCH-07) and `plans/README.md` (plans 011 to 013, 015 to 017,
  "Not planned, and why"); `plans/night/2026-11-11-plan.md`, 2026-11-16 12:45 and 22:25 to 23:25.
- jdrly/gw2-on-mac #213 (the trial merge in the upstream watch).
