# Nothing from this fork goes upstream

Decided 2026-10-27, tightened 2026-11-11. Status: accepted.

Upstream DXMT refuses code that AI wrote or co-wrote (its `AGENTS.md` and its `CONTRIBUTING.md`, "AI
Policy"). My GW2 work is written by AI agents, so it lives only in this fork, and no pull request
ever goes to 3Shain/dxmt. Until 2026-11-11 I still allowed findings to go there as issues or
research, which their policy permits. I took that back when an agent told me twice that a bug exists
upstream too. Nothing goes there now, not an issue, not a comment, not a finding. Upstream accepts
nothing made with AI, so I would have to rewrite a finding by hand before they could use it. A bug
that stays upstream is their problem. I keep my own DXMT polished.

## Consequences

- `gw2-on-mac` is the default branch and carries my work. `main` mirrors upstream and gets nothing
  of mine.
- A `gh repo clone` of the fork adds an `upstream` remote. Remove it, and pass `--repo jdrly/dxmt`
  to every `gh` call, so nothing lands in 3Shain's tracker by accident.
- Every commit an agent wrote says so.
- A bug the fork inherited from upstream is fixed here, at its cause. On the same day I turned down
  a per-process workaround for one.
- I still take upstream's work into the fork. That only stays cheap while the fork merges cleanly,
  which is what ADR 0012 is for.
- DXMT is LGPL after v0.80, so a fork I ship publishes its changes. The fork is public for that.

## Alternatives I rejected

- **Pull requests upstream.** Their policy closes that door to AI-written code.
- **Findings as issues or research.** This was the rule from 2026-10-27 to 2026-11-11. A finding
  made with AI would need me to redo the work by hand before upstream could use it, and I'm not
  doing that now.
- **Working around an inherited bug per process.** I don't want band-aid fixes.

## Evidence

- Upstream's `AGENTS.md` and `CONTRIBUTING.md` ("AI Policy").
- This fork's `.github/CONTRIBUTING.md` and `AGENTS.md`.
- GW2 on Mac workspace: `AGENTS.md` ("Issue tracker", and the LGPL note under "Findings that shape
  the work"); the dispatcher's notes `dxmt-fork-ai-policy` (2026-10-27, updated 2026-11-11) and
  `arm64-0-2-line-and-ci` (what I said on 2026-11-11).
