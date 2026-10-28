# The GW2 fork's switches and log lines

Everything the fork adds on top of upstream DXMT. A pull request that adds or changes a switch
or a log line updates this file in the same PR.

GW2 on Mac sets `DXMT_LOG_PATH` to its `work/logs`, so the lines below land in
`Gw2-64_d3d11.log` there. The file is rewritten each time the game creates its device.

## Switches

| Variable | Default | What it does | Off restores |
|---|---|---|---|
| `DXMT_STATS` | on | The 2 s statistics lines (`STATS`, `STATS2`, `PACE`, `GPU`, the sections), `HITCH` lines, the startup line, and all their measuring | none of it, and no measuring |
| `DXMT_HITCH_MS` | 50 | A `HITCH` line for a Present-to-Present interval (game's render thread) at least this long; `0` turns them off | — |
| `DXMT_PRESENT_SPLIT` | on | Commit the frame's command buffer before `nextDrawable` and present from a new one | the present in the frame's last buffer |
| `DXMT_DISPLAY_SYNC` | on | A present with a sync interval uses `CAMetalLayer`'s vsync (`displaySyncEnabled`); a minimum duration only for intervals over one refresh, half a refresh short | DXMT's own pacing (minimum duration of exactly one refresh, display sync off) |
| `DXMT_PRESENT_DEADLINE` | one refresh | A present without a minimum duration goes out with `presentDrawable:atTime:` now + the deadline, so macOS can raise the GPU clock; `=<ms>` sets it | `=0`: a plain `presentDrawable:` |
| `DXMT_STENCIL_PFV` | off | `=1` keeps `PixelFormatView` (no lossless compression) on typeless depth-stencil targets with a stencil plane | — |
| `DXMT_BG_FPS` | 30 | Present sleeps to this rate while another process's window is in the foreground | `=0`: no throttle |
| `DXMT_BG_FPS_MINIMIZED` | 10 | The same while GW2's window is minimized | `=0`: no throttle |

Not switchable: `SetMaximumFrameLatency(0)` means the default, 3 (DXGI's definition), and typeless
depth targets without stencil (`R32_TYPELESS`, `R16_TYPELESS` as depth) never get
`PixelFormatView`.

## The STATS line

`STATS fps= ms=avg/max cb= rp=(+ merged) clear= split= drawable=avg/max commit=avg/max
sync=Nx avg/max stall= encode=prepare+flush/max latency=avg/max maxlatency= interval= deadline=`

Averages are per frame over the 2 s window; times are ms.

| Field | Meaning |
|---|---|
| `fps`, `ms` | Present-to-present on the **encode thread**, not the game's own Present interval |
| `cb` | Command buffers committed per frame (chunks; the split present buffer isn't counted) |
| `rp` | Render passes per frame; `+ merged` = passes the optimizer merged away |
| `clear` | Clear passes that weren't folded into a render pass |
| `split` | Present splits per frame (`DXMT_PRESENT_SPLIT`) |
| `drawable` | All of `Presenter::encodeCommands`: `nextDrawable` **and** the present copy's encoding |
| `commit` | The render thread waiting for a free chunk (ring backpressure), **not** the cost of `commit` |
| `sync` | CPU waits for the GPU (Map, GetData) per frame, and their time |
| `stall` | Event stalls in the window |
| `encode` | Encode-thread time per frame: preparing + flushing (drawable excluded), and the flush's max |
| `latency` | The render thread waiting in `PresentBoundary` for frame N − `maxlatency` to finish on the GPU |
| `maxlatency` | The frame latency in effect |
| `interval` | The sync interval of the last Present |
| `deadline` | The deadline requested (ms); a present that takes the minimum-duration branch ignores it |

## The other 2 s lines

All are written right after `STATS`, from the same window.

| Line | Field | Meaning |
|---|---|---|
| `STATS2` | `nextdrawable` | `nextDrawable` alone, avg/max ms per frame (part of `drawable`) |
| | `present` | Presents per branch in the window: `plain`, `deadline` (`presentDrawable:atTime:`), `after` (minimum duration) |
| | `wait` | The render thread's waits per frame, `commit` + `sync` + `latency`, avg/max |
| `PACE` | `app` | The **game's** Present-to-Present interval on its render thread: p50/p95/p99/max ms (p99 over ~100 frames is noisy) |
| | `slow33`, `slow50`, `n` | Intervals over 33.4 and 50 ms, and how many intervals |
| | `dxmt` | Render-thread time inside DXMT's entry points, ms per frame, then per category `ms/calls` per frame: `present` (Present's own work, without the throttle and the latency wait), `map` (Map and Unmap, **their waits included**), `flush`, `create` (every device `Create*`, any thread), `draw` and `dispatch` (one call in 16 timed and scaled: an estimate). Nested calls count once |
| `GPU` | `gpuspan` | Per frame, the union of DXMT's command buffers' GPU execution spans (`GPUStartTime`..`GPUEndTime`), avg/max ms. A span union, not the GPU's active time; never compare it with Instruments' fragment/vertex busy time |
| | `gpugap` | The gap between one frame's last GPU end and the next one's first GPU start, avg/max ms (frames that overlap don't count) |
| | `frames`, `buffers`, `missing` | Frames collected, command buffers per frame, buffers without timestamps |
| sections | | A feature's own line, `<NAME> k=v …` (see below) |

Sections: a feature registers a function with `gw2::SectionRegistrar` (`src/dxmt/dxmt_gw2_stats.hpp`) that returns its part of the window; each non-empty one is logged as its own line after `GPU`. A feature that adds a section documents it here.

## Other log lines

| Line | When |
|---|---|
| `GW2 fork <commit>: DXMT_…=… WINENCPU=… chunks=… cpus=… refresh=…` | Once, at the first Present: the build, every `DXMT_*` variable set, the chunk count, the CPUs the game sees |
| `HITCH frame=… ms=… commit=… sync=…x… latency=… bgsleep=… dxmt(ms/calls): …` | A Present-to-Present interval of at least `DXMT_HITCH_MS`: that frame's waits and its render-thread time inside DXMT per category |
| `SetMaximumFrameLatency(n)` | Every call; 0 is taken as 3 |
| `Presenter: display sync on/off` | When `DXMT_DISPLAY_SYNC` switches the layer's vsync |
| `Present: <reason>, throttling to N fps (foreground window …, pid …, ours …)` | The background throttle engages |
| `Present: presenting normally again (was: …)` | It lets go |
| `PixelFormatView (no lossless compression): format F bind B WxH` | Once per format and bind flags of a render or depth target that keeps the flag |
| `TextureView: stencil view of a depth-stencil texture without PixelFormatView …` | Up to 8 times, if a stencil view meets a compressed depth-stencil texture |
