# The AIR reader pool is dropped

Decided 2026-11-26. Status: accepted (#361).

`DXMT_AIR_READERS=n` opened up to n read-only connections to the AIR cache, each behind its own
lock, in place of upstream's one reader behind one lock (#95, #98). A native bench on a copy of
GW2's 1.5 GB cache made the case for it. At 8 workers the lock wait per cached variant fell from 0.19 ms with one
reader to 0.00 with eight. Play never made that case. The switch stayed at upstream's 1 from the
day it merged, and the app never names it. My own log says the lock costs nothing in play: in the
48 minutes of the long dev.23 session the reader lock took 3.6 ms in all.

ADR 0015 in the workspace makes my fork a curated patch series and drops topics that never became
defaults and have no open case for one. `air-readers` is the first such drop in this fork. So the
pool goes.

## Consequences

- `src/dxmt/dxmt_shader_cache.cpp` and `.hpp` are upstream's text again, so two upstream files
  lose every fork line they had. `src/dxmt/dxmt_gw2_air_readers.hpp` and
  `tests/gw2/air_readers_test.cpp` are deleted.
- `DXMT_AIR_READERS` leaves the switch table and the startup line. Set, it now does nothing, and
  the second startup line still lists it among the variables set.
- With the switch at its default the build does what it did: one reader behind one lock.
- COMPILE's `variant=lock` stays. It measures that one lock, and it is how I'd see the case come
  back.
- In `SERIES.md` the topic moves to the dropped list. Its removal commit joins the topic when the
  series is next built, so the topic's code squashes to nothing.

## Alternatives I rejected

- **Keeping it at 1 until #159.** The code stays in two upstream files while it waits, and every
  sync can conflict there. If a cold cache ever makes the lock show up in `variant=lock`, the two
  commits are in the history.
- **Making the pool the default.** No session has run with it, so it would be a change nobody has
  played.

## Evidence

- #95 (PR #96, `1654800`) and #98 (PR #99, `8cf99bc`), with the native bench in #96's commit
  message.
- GW2 on Mac workspace: `docs/adr/0015-my-forks-are-curated-patch-series-on-mach9-releases.md`
  (extended on 2026-11-26, "The first two drops"), `docs/research/upstream-watch-triage-2026-11-25.md`
  (the reader lock's 3.6 ms), `AGENTS.md` ("AIR reader pool").
- [SERIES.md](../SERIES.md), the `air-readers` row.
