# Upstream sync

How I take `3Shain/dxmt` `main` into `gw2-on-mac` (#140 took `647b002`, #249 took `0df8610`).
Nothing goes the other way: never push, open or comment anything in 3Shain/dxmt.

1. Add `3Shain/dxmt` as the remote `upstream`, read only (`git remote set-url --push upstream
   DISABLED`), and fetch it.
2. On a branch off `gw2-on-mac`, `git merge --no-ff upstream/main`. Resolve conflicts by keeping both
   sides; the fork's switches stay as they are. The merge commit's body says an AI agent made it,
   when one did.
3. Check the merge before the PR:
   - for each file upstream changed since the last sync, `git diff origin/gw2-on-mac HEAD -- <file>
     | git patch-id --stable` equals the same for upstream's own diff, except a file with a resolved
     conflict (read that diff whole) or one whose upstream change I already ported by hand;
   - a commit already ported by hand (like `0df8610` in #165) adds no second copy: its files show no
     change from that commit in the merge;
   - `AIRCONV_VERSION` (`src/airconv/airconv_public.h`) is unchanged, or the PR says why not;
   - `scripts/shader-warm.zsh`'s parity check (GW2 on Mac workspace) and a GW2 session before and
     after, on my Mac.
4. The PR is merged with a merge commit, not squashed, so upstream's commits stay ancestors and the
   next sync starts from them.
5. Fast-forward the fork's `main` to the synced upstream commit (`git push origin
   <sha>:refs/heads/main`, never forced). `main` gets nothing else.

## Upstream's workflow stays disabled

Upstream's `.github/workflows/ci.yml` ("CI Build") runs 16 `macos-15` jobs on every push to
`main`. I disabled it in the fork (#249); only `gw2.yml` runs here. Before step 5, check it still reads
`disabled_manually`:

```zsh
gh api repos/jdrly/dxmt/actions/workflows/399298663 --jq .state
```
