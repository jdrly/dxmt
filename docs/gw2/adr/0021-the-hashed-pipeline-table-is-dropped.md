# The hashed pipeline table is dropped

Decided 2026-11-27. Status: accepted (#370).

Each context checks its last 4 plain pipelines before it asks the device's pipeline cache
(`DXMT_PSO_MRU`, #6). #366 tried 64 slots indexed by the key's hash in their place, behind a switch
that was off by default (PR #368), to take more lookups off the cache's lock.

My session of 0.2.2-dev.5 ran the table in Lion's Arch, against dev.4's session, both over the
windows with 3,500 draws or more. The table found 54% of pipeline changes without the cache, where
the 4-entry list found 40%, and the cache's lock fell from 0.079 to 0.061 ms a frame. GW2 cycles
through more pipelines a frame than 64 recent ones hold, so the table saves about 0.02 ms a frame of
the render thread. That is below the bar I dropped `macdrv-calls` at (0.6 ms a second). Under ADR
0010 a switch whose test is over and that lost is deleted, so the table goes.

## Consequences

- `src/dxmt/dxmt_gw2_pso_table.hpp` and its test are deleted, and the context loses its two hooks
  and its member. `src/` and `tests/` are what they were before PR #368, with the changes merged
  since.
- The switch leaves the switch table and the startup line, and the `PSO:` line no longer names it.
  Set, it now does nothing, and the second startup line still lists it among the variables set.
- The 4-entry list stays as it was, and so do the `PSO` line's `mru`, `lookups` and `lock`.
- The app's `DEV_GONE` row for the switch merged first (jdrly/gw2-on-mac #386), as ADR 0010 asks.
- In `SERIES.md` the switch moves to the dropped list.

## Alternatives I rejected

- **A bigger table.** GW2's working set of pipelines a frame is larger than 64, and 0.02 ms a frame
  doesn't make the case for trying more slots.
- **Keeping it off.** An off switch still costs a check on the pipeline lookup and lines in an
  upstream file, and every sync can conflict there.

## Evidence

- #366's verdict comment (the dev.4 and dev.5 medians of the windows with 3,500 draws or more).
- #366 (PR #368, `5d41751`).
- [SERIES.md](../SERIES.md), the dropped list.
