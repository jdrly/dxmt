# jdrly/dxmt agent guidance

Upstream DXMT (3Shain/dxmt) says: **"AI must not be used to generate code for contributions to
this project."** Its `CONTRIBUTING.md` adds that it can't accept contributions made or co-authored
by AI. That rule binds anything offered to 3Shain/dxmt, and nothing from this fork is.

This fork, `jdrly/dxmt`, belongs to [GW2 on Mac](https://github.com/jdrly): Guild Wars 2 through
Steam in a CrossOver 26.3 engine on Apple Silicon. Its branch `gw2-on-mac` (the default) carries
AI-written changes for that use, and they stay here:

- **Never open a pull request, issue or discussion in 3Shain/dxmt** from this fork's work. A
  finding upstream should hear about goes to Jan, who decides whether to report it as research
  (upstream's policy allows sharing findings, not code).
- Mark every AI-written commit as such in its body.
- `main` mirrors upstream and receives nothing else. Work branches off `gw2-on-mac` and merges back
  through a pull request.

How to work here: [`.github/CONTRIBUTING.md`](.github/CONTRIBUTING.md) (issues, branches, commits,
pull requests, review, Definition of Done), the tracker in
[`docs/agents/issue-tracker.md`](docs/agents/issue-tracker.md), the labels in
[`docs/agents/triage-labels.md`](docs/agents/triage-labels.md), and the fork's switches and log
lines in [`docs/gw2/README.md`](docs/gw2/README.md). The project's own guidance, measurements and
traces are in the GW2 on Mac workspace (`AGENTS.md` and `docs/research/` there).
