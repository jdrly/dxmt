# Issue tracker: GitHub

Issues and specifications for this fork live as GitHub issues in `jdrly/dxmt`. There is no
other tracker, and nothing is ever filed in 3Shain/dxmt (see [AGENTS.md](../../AGENTS.md)). Use
the `gh` CLI for every GitHub operation, and follow the issue templates in
[`.github/ISSUE_TEMPLATE/`](../../.github/ISSUE_TEMPLATE) and the title rule in
[`CONTRIBUTING.md`](../../.github/CONTRIBUTING.md#issues).

## Conventions

- **Create an issue**: `gh issue create --repo jdrly/dxmt --title "Context: Actionable result"
  --body-file <file> --label ...`, with a body that follows the matching template (`task.yml`
  for work, `bug_report.yml` for a bug). This repository is public: create issues only when Jan
  asks for them, or after he approves the draft.
- **Read an issue**: `gh issue view <number> --repo jdrly/dxmt --comments`, including its labels.
- **List issues**: `gh issue list --repo jdrly/dxmt --state open --json number,title,labels`
  with `--label` and `--state` filters as needed.
- **Comment**: `gh issue comment <number> --repo jdrly/dxmt --body "..."`
- **Labels**: `gh issue edit <number> --repo jdrly/dxmt --add-label "..."` / `--remove-label "..."`
- **Close**: `gh issue close <number> --repo jdrly/dxmt --comment "..."`. A merged pull request
  with `Closes #<number>` closes its issue by itself.
- **Pull requests** go into `gw2-on-mac`: `gh pr create --repo jdrly/dxmt --base gw2-on-mac`.
  `gh` defaults to the parent repository for a fork, so always pass `--repo jdrly/dxmt`.

## Pull requests as a triage surface

**PRs as a request surface: no.** GitHub shares one number space between issues and pull
requests, so a bare `#4` may be either: resolve it with `gh pr view 4 --repo jdrly/dxmt` and
fall back to `gh issue view 4 --repo jdrly/dxmt`.

## When a skill says "publish to the issue tracker"

Create a GitHub issue in `jdrly/dxmt` as above.

## When a skill says "fetch the relevant ticket"

Run `gh issue view <number> --repo jdrly/dxmt --comments`.
