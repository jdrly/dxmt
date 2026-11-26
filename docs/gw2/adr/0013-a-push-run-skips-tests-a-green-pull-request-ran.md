# A push run skips the native tests a green pull request ran on the same tree

Decided 2026-11-16. Status: accepted.

Every push to `gw2-on-mac` ran the Native tests job again, although the pull request it merged had
run the same tests on the same tree. That job takes 5.5 to 5.7 macOS minutes, a quarter of a push
run. Runner time is the limit on this project: the forks build only on GitHub's runners, and GitHub
disabled my Wine fork's workflows once for wasted compute. So a push skips the Native tests when a
green pull request run on the same tree exists. When none exists, the push runs them. My Wine fork
had the skip since 2026-11-12 and got its guard on 2026-11-16. DXMT got both at once.

## Consequences

- The `tested` job looks up the commit's merged pull requests, a head with the same tree and a
  successful `pull_request` run of `gw2.yml` on it, and the `tests` job skips only when it finds
  one. On a pull request, on no match, and when a lookup fails, the tests run.
- Narrowed on 2026-11-17 (#310, PR #311): the head must also contain the push's parent. Every job
  checks out the head, but the run takes its workflow from the merge into `gw2-on-mac`. Only when
  the head contains the branch as it was before the push were the run's steps the head's own.
- Publish still uploads the build for dev builds after a skip. The first push run after the merge
  showed both (run 43874542317).
- A pull request's run takes its workflow from GitHub's merge ref and checks out the head commit. A
  branch cut before a CI change then runs new steps against an old tree and fails. That showed on PR
  #302 after #300 added a step. Until it is fixed, such a branch rebases first.

## Alternatives I rejected

- **Running every test on every push.** It repeats what the pull request proved, at a quarter of
  each push run.
- **Skipping on every push without the lookup.** A stale pull request merged into a moved branch
  would publish a build that no test saw.

## Evidence

- #273 (PR #274, `34dc411`), run 43874542317, #310 (PR #311, `43dbef4`).
- jdrly/wine PR #11 (the first skip, 2026-11-12) and PR #33 (the guard, jdrly/gw2-on-mac #214).
- GW2 on Mac workspace:
  `advisor-plans/2026-11-16-forks/plans/026-dxmt-ci-skip-native-tests-on-tested-push.md` and
  `plans/README.md` ("Follow-ups found while executing", from 008);
  `plans/night/2026-11-11-plan.md`, 2026-11-16 14:15 to 14:30.
