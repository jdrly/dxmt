# The switches GW2 never exercises are dropped

Decided 2026-11-27. Status: accepted (#363).

In 68 sessions of my own logs GW2 never ran these at another value, or never took the path they
count. Each costs code, most of it in upstream's files or the bridge, and none has an open case.

- `DXMT_PSO_TOPOLOGY_KEEP` (#6, on). `PSO topokept` is 0.0 in 21,729 of 21,732 windows and 0.1 in
  the other three.
- `DXMT_CHUNKS` and `DXMT_MAX_FRAME_LATENCY` (#8, unset). The ring peaks at 4 to 9 of 32, and
  `RING stalled` is 0 in every window.
- `DXMT_CONTROL` (#7, off). The control file for A/B minutes. I stopped running AFK A/Bs on
  2026-11-04, and nothing writes the file.
- `thread-priorities`: `DXMT_RENDER_THREAD_PRIORITY` (#9, off), `DXMT_THREAD_PRIORITY` (#151) and
  `DXMT_COMPILE_PRIORITY` (#152), both `critical`. No session ran another value. The watts they
  were for are 0.4 to 0.7 W by arithmetic, and the app's own comment says lower priorities cost
  frames in crowds on 0.1.x.
- `DXMT_PRESENTER_FREE` (#185, off). GW2 makes one swap chain, so it would save about 33 KB once at
  exit. The leak it was written for is in Steam's browser, which the game's rule never reaches.
- The `WaitForVBlank` count (#250), `STATS vblank`. It is 0.0 in all 11,348 windows that have the
  field. It answered its question: GW2 doesn't call `WaitForVBlank`.

ADR 0010 says a switch whose test is over is deleted or becomes the default. ADR 0015 in the
workspace makes my fork a curated patch series that drops what never became a default. So these go.

## What each build does now

The default stays exactly what it was.

- The topology keep path stays and is always on, since its switch was on. Dropping the path itself
  would change the default for the rare window that takes it, so only the switch read, its startup
  line field and its part of the `PSO:` line go. `PSO topokept` stays, so a hit stays visible.
- The ring is upstream's 32 again, `kCommandChunkCount`, and so is the Metal queue's bound on command
  buffers. The chunk array, the ring stall check and the ledger use the constant.
  `SetMaximumFrameLatency` is my fork's code from before #8: 0 means 3, and above 16 is refused.
  The `RING` line and the `CommandQueue:` line stay.
- The encode, finish and compile threads call `SetThreadPriority(THREAD_PRIORITY_TIME_CRITICAL)`
  as upstream does, which is what `critical` did. The compile service's `Options::priority` goes,
  since nothing sets it any more. The render thread is left as the game sets it.
- `Presenter` is upstream's `RcObject` again, so a presenter stays after its last reference, as it
  did with the switch off. The `Presenter: freed` line goes.
- `WaitForVBlank` returns `S_OK` as upstream's does, and `IMTLDXGIAdapter` loses `GetVBlankCalls`.

## Consequences

- Bridge calls 153 (`WMTSetThreadQoSClassSelf`) and 200 (`WMTSetThreadBasePrioritySelf`) become
  `WMT_RESERVED` holes, as 202 did in #277. No call is renumbered. `unix/winemetal_gw2.c` held only
  those two handlers and is deleted with its header; `winemetal_unix.c` loses its include, one fork
  line fewer in an upstream file.
- `src/dxmt/dxmt_gw2_control.*`, `src/dxmt/dxmt_gw2_thread_priority.*`,
  `tests/gw2/thread_priority_test.c` and `tests/gw2/vblank_count_test.cpp` are deleted. The present
  deadline's range check moves to `dxmt_gw2_present.hpp`. The Mac run has seven tests, not eight.
- `dxmt_presenter.*`, `dxgi_adapter.cpp`, `dxgi_interfaces.h` and `dxgi_output.cpp` lose every fork
  line #185 and #250 added. `dxmt_command_queue.*` gets upstream's ring and thread priority lines
  back.
- `presenter_layer_set_test` keeps its two phases and the lifetime-check build, without the
  `DXMT_PRESENTER_FREE` runs and the presenter count. `switches_test` drops its two specimens
  (`DXMT_RENDER_THREAD_PRIORITY` and `DXMT_CHUNKS`) and checks `strtoul32` against the read they
  replaced. `sw::legacy::ulongOneOf`, which only `DXMT_CHUNKS` used, is deleted.
- Set, any of these variables now does nothing, and the second startup line still lists it among
  the variables set. The app's dev list writes `DXMT_PRESENTER_FREE=1`, so its `DEV_GONE` row
  (jdrly/gw2-on-mac #373) lands first.
- In `SERIES.md`, `thread-priorities` moves to the dropped list. The other five topics keep their
  rows with the switch gone. The removal commit joins the topics when the series is next built.

## Alternatives I rejected

- **Leaving `thread-priorities` out of the series.** `--omit thread-priorities` leaves 14 of 86
  series commits in conflict, all in the bridge's call lists and one hunk of
  `d3d11_swapchain.cpp`. A removal commit doesn't.
- **Deleting the topology keep path.** It would change the default with no switch, for a path that
  three windows took.
- **Keeping the priorities for a watts test.** A `powermetrics` reading would decide it, but the
  gain is under 1 W of 23 by its own ticket, and the commits stay in the history if I want it back.

## Evidence

- GW2 on Mac workspace: `advisor-plans/2026-11-26-topic-analysis/DECISIONS.md` (section 1),
  `ANALYSIS-dxmt.md` and its `tools/dxmt-all-topics/` (the per-window sums and the topic replays).
- #6, #7, #8, #9, #151, #152, #185, #250, and their pull requests.
- [SERIES.md](../SERIES.md), the rows of the six topics.
