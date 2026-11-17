# Upstream sync

How I take `3Shain/dxmt` `main` into `gw2-on-mac` (#140 took `647b002`, #249 took `0df8610`).
Nothing goes the other way: never push, open or comment anything in 3Shain/dxmt.

1. Add `3Shain/dxmt` as the remote `upstream`, read only (`git remote set-url --push upstream
   DISABLED`), and fetch it.
2. On a branch off `gw2-on-mac`, `git merge --no-ff upstream/main`. Resolve conflicts by keeping both
   sides; the fork's switches stay as they are. Two kinds of conflict are not "both sides": the
   bridge's two tables ([below](#the-bridges-two-tables-and-upstreams-new-calls)) and upstream
   code I moved to another file ([below](#moved-upstream-code)). The merge commit's body says an
   AI agent made it, when one did.
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

## The bridge's two tables and upstream's new calls

Upstream numbers a bridge call by its position in two hand-written tables in
`src/winemetal/unix/winemetal_unix.c`, `__wine_unix_call_funcs` and
`__wine_unix_call_wow64_funcs`, and its thunk passes that number: `UNIX_CALL(145, &params)`. My
fork generates both tables from `src/winemetal/winemetal_calls.h` and names the call in the thunk
([ADR 0005](../gw2/adr/0005-bridge-call-numbers-in-one-manifest.md)). So when upstream adds a
call, its `&_Name,` line in each table is a change to lines my fork deleted, and it conflicts at
every sync that brings one. Keeping both sides would put upstream's hand list back next to the
generated one.

For a sync with a conflict in those tables:

1. In the two table hunks, take my side. Each table stays the three `#define` lines and
   `#include "../winemetal_calls.h"`. Upstream's new handler function (`_Name`, above the tables)
   stays: keep both sides there as usual.
2. Run `tools/gw2/upstream-bridge-calls.py`. Without arguments it compares upstream's tables at
   my last sync (`git merge-base origin/gw2-on-mac upstream/main`) with `upstream/main`. It
   prints one manifest line for each call upstream appended, numbered from the manifest's next
   free number, under an `/* upstream <sha> */` comment, and `WMT_CALL_WOW64` where upstream's
   wow64 entry differs from the 64-bit one.
3. Append those lines to `src/winemetal/winemetal_calls.h`, right above `/* clang-format on */`.
   Never insert one in the middle and never renumber: a line's position is its number, and the
   build rejects a gap, a duplicate or a line out of order.
4. Upstream's new thunk, in `winemetal_thunks.c` or `airconv_thunks.c`, passes upstream's
   number, which here may be another call's. Write the call's name instead, as the script says:
   `UNIX_CALL(MTLTexture_getBytes, &params)`. A number left in place doesn't build.
5. A call the script reports as "already in the manifest" was ported by hand before the sync, as
   `0df8610` was in #165. It gets no second line, and its handler and thunk must not end up in
   the file twice.
6. If the script exits 1, upstream changed or removed an existing entry or reshaped its tables.
   Read both tables by hand at the two commits, and stop and ask me if a call's meaning changed.

The build checks the result: the static asserts in `winemetal_call_numbers.h`, and the pure
tests `bridge_manifest_test` and `bridge_manifest_reject.zsh` in `tests/gw2/`.

## Moved upstream code

Code my fork moved out of an upstream file. An upstream change to it conflicts as a change to
lines my fork deleted, or doesn't conflict at all when the whole file is gone on my side and git
keeps it deleted. Either way the change has to be carried to the new place by hand, and the
patch-id check of step 3 will flag the file without saying where the code went. A PR that moves
upstream code adds a row here.

| What | From (upstream) | To (my fork) | PR |
|---|---|---|---|
| The compile scheduler, `task_scheduler` | `src/dxmt/dxmt_tasks.hpp`, deleted in my fork | `src/dxmt/dxmt_compile_service.hpp`; upstream's file as it was: `git show 02532d0:src/dxmt/dxmt_tasks.hpp` | #65, #198 |
| The fence part of fusing two render passes | `checkEncoderRelation` in `src/dxmt/dxmt_context.cpp` | `fuseRenderPassFences` in `src/dxmt/dxmt_deptrack.hpp` | #132 |
| The bridge's two dispatch tables | `src/winemetal/unix/winemetal_unix.c` | `src/winemetal/winemetal_calls.h` (the section above) | #67 |
| airconv's call numbers, `enum airconv_unixcalls` | `src/winemetal/airconv_thunks.h` | the manifest's `WMTCall_SM50…` names in `src/winemetal/winemetal_call_numbers.h` | #76 |
| The native build's unix source list and link arguments | `src/nativemetal/meson.build` | `winemetal_unix_src` and `winemetal_unix_link_args` in `src/winemetal/unix/meson.build` | #61 |

Changed in place, not moved, but the same care applies: the `GenericAccessTracker::access*`
functions in `src/dxmt/dxmt_deptrack.cpp` (`accessShared`, `accessExclusive` and their
`PreRaster` and `Fragment` forms) take an `EncoderId horizon` parameter where upstream uses the constant
`kLane` (#133). An upstream change to those functions merges into the fork's signatures by hand.

## Upstream's workflow stays disabled

Upstream's `.github/workflows/ci.yml` ("CI Build") runs 16 `macos-15` jobs on every push to
`main`. I disabled it in the fork (#249); only `gw2.yml` runs here. Before step 5, check it still reads
`disabled_manually`:

```zsh
gh api repos/jdrly/dxmt/actions/workflows/399298663 --jq .state
```
