# The GW2 fork's switches and log lines

Everything the fork adds on top of upstream DXMT. A pull request that adds or changes a switch
or a log line updates this file in the same PR.

GW2 on Mac sets `DXMT_LOG_PATH` to its `work/logs`, so the lines below land in
`Gw2-64_d3d11.log` there. The file is rewritten each time the game creates its device.

## Switches

| Variable | Default | What it does | Off restores |
|---|---|---|---|
| `DXMT_STATS` | on | The 2 s statistics lines (`STATS`, `STATS2`, `PACE`, `GPU`, the sections), `HITCH` lines, the startup line, and all their measuring (the fork's timers, the `nextDrawable` timer, the present-branch counts, keeping split-off command buffers until they complete) | none of it, and no measuring beyond upstream's frame statistics |
| `DXMT_HITCH_MS` | 50 | A `HITCH` line for a Present-to-Present interval (game's render thread) at least this long; `0` turns them off | — |
| `DXMT_PRESENT_SPLIT` | on | Commit the frame's command buffer before `nextDrawable` and present from a new one | the present in the frame's last buffer |
| `DXMT_DISPLAY_SYNC` | on | A present with a sync interval uses `CAMetalLayer`'s vsync (`displaySyncEnabled`); a minimum duration only for intervals over one refresh, half a refresh short | DXMT's own pacing (minimum duration of exactly one refresh, display sync off) |
| `DXMT_PRESENT_DEADLINE` | one refresh | A present without a minimum duration goes out with `presentDrawable:atTime:` now + the deadline, so macOS can raise the GPU clock; `=<ms>` sets it | `=0`: a plain `presentDrawable:` |
| `DXMT_STENCIL_PFV` | off | `=1` keeps `PixelFormatView` (no lossless compression) on typeless depth-stencil targets with a stencil plane | — |
| `DXMT_BG_FPS` | 30 | Present sleeps to this rate while another process's window is in the foreground | `=0`: no throttle |
| `DXMT_BG_FPS_MINIMIZED` | 10 | The same while GW2's window is minimized | `=0`: no throttle |
| `DXMT_RENDER_THREAD_PRIORITY` | off | On the first Present of each thread that presents (GW2's render thread): `=1` sets the macOS QoS class user-interactive (`pthread_set_qos_class_self_np` through the bridge), `=2` calls `SetThreadPriority(THREAD_PRIORITY_HIGHEST)`, `=3` both; see [Render thread priority](#render-thread-priority) | unset or `=0`: the thread keeps the priority and QoS it had |
| `DXMT_DEPTH_PFV` | off | `=1` keeps `PixelFormatView` (no lossless compression) on typeless single-plane depth targets (`R32_TYPELESS`, `R16_TYPELESS` bound as depth), the part `DXMT_STENCIL_PFV` doesn't cover (#2) | — |
| `DXMT_BG_POLICY` | `focus` | What "in the background" means for `DXMT_BG_FPS`. `focus`: another process's window is in the foreground, or none is (pid 0, counted as another process). `occlusion`: GW2's window isn't visible on screen (`NSWindow.occlusionState` without `Visible`, read through the bridge at most 10 times a second) or is miniaturized (`DXMT_BG_FPS_MINIMIZED`), whoever has the focus, as d9mt does. A minimized window (`IsIconic`) throttles under both | `focus` |
| `DXMT_PSO_BLEND_HASH` | on | The pipeline key's hash includes a fingerprint of the blend state (exactly what the key's equality compares: disabled fields left out, one render target without independent blend, else one per color attachment) and `RasterizationEnabled`; computed once per blend state object. The equality rejects keys whose fingerprints differ without reading both blend descriptions | `=0`: the upstream hash (no blend, no `RasterizationEnabled`) and equality |
| `DXMT_PSO_MRU` | on | Each context keeps its last 4 plain (VS/PS) pipelines with their keys and checks them, with the cache's own equality, before the device's pipeline cache (its lock, hash and bucket) | `=0`: every pipeline change looks up the cache |
| `DXMT_PSO_TOPOLOGY_KEEP` | on | `IASetPrimitiveTopology` keeps the current pipeline when the new topology has the same class (point, line, triangle) and the pipeline is a plain one (no GS, HS or DS bound); adjacency and patch lists always invalidate | `=0`: every topology change invalidates the pipeline |
| `DXMT_CHUNKS` | unset (32) | The chunk ring's size, `32`, `64` or `128` (anything else: 32, with a warning). The render thread blocks in `CommitCurrentChunk` at size − 1 chunks in flight. Set (even to 32), the Metal queue's bound on uncompleted command buffers is the size + 16 (headroom for the present split's extra buffer, one per frame in flight); unset, it stays 32. An experiment (#8) | unset: upstream's ring of 32 and Metal bound of 32 |
| `DXMT_MAX_FRAME_LATENCY` | unset | `=1`…`16` replaces the frame latency the game sets with `SetMaximumFrameLatency` (and the default of 3 before it calls); out of range: ignored, with a warning. At 15–16 the 16 per-frame statistics slots (STATS, HUD) can mix frames | unset: the game's value, 0 taken as 3 |
| `DXMT_CAP_LIMITER` | on | With display sync, a frame cap (`d3d11.preferredMaxFrameRate`) below the refresh rate that isn't a whole number of refreshes (50 fps on 60 Hz) is kept by a frame limiter on the render thread: Present sleeps to a fixed cadence before it takes the device lock, and the present goes out with display sync and the deadline. Whole numbers (30, 20 fps on 60 Hz) keep the minimum duration, half a refresh short, as before | the cap is dropped (`vsync_duration` 0: uncapped to the refresh rate) |
| `DXMT_COMPILE_WAIT_MS` | 2 | A `COMPILE frame=…` line for a frame whose encode-thread pipeline waits add up to at least this many ms (at most 5 lines a second); `0` turns them off. Needs `DXMT_STATS` | — |
| `DXMT_PSO_ARCHIVE` | off | `=1`: Metal binary archives for pipelines. Pipelines are looked up in the archives of the last 4 sessions, and every render and compute pipeline built is added to this session's archive (see [COMPILE](#compile)) | no archives: no lookup, no capture, no files |
| `DXMT_PSO_ARCHIVE_DIR` | user cache dir | With `DXMT_PSO_ARCHIVE=1`: an absolute unix directory that replaces `$(getconf DARWIN_USER_CACHE_DIR)dxmt/pso` | — |
| `DXMT_PSO_FLUSH_EVERY` | 256 | With `DXMT_PSO_ARCHIVE=1`: serialize this session's archive every this many new pipelines (also every 120 s if any are new; never at exit) | — |
| `DXMT_PROMOTE_FLUSH_MIN` | 0 | `=<n>`: a speculative commit of an occlusion or timestamp query `End` waits until `n` such requests have accumulated, with a progress guarantee (see [CMDS](#cmds)). `1` commits as upstream does | `=0`: upstream (commit at the next pass change) |
| `DXMT_PROMOTE_FLUSH_MS` | 2 | With `DXMT_PROMOTE_FLUSH_MIN`: the longest a held commit waits (the timer at the next pass change, and the watchdog's period) | — |

Not switchable: `SetMaximumFrameLatency(0)` means the default, 3 (DXGI's definition; only
`DXMT_MAX_FRAME_LATENCY` replaces the result). Typeless depth targets without stencil
(`R32_TYPELESS`, `R16_TYPELESS` as depth) get `PixelFormatView` only with `DXMT_DEPTH_PFV=1`.

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
| | `wait` | The render thread's waits per frame, `commit` + `sync` + `latency`, avg/max (per frame-statistics slot, as `STATS`) |
| `PACE` | `app` | The **game's** Present-to-Present interval on its render thread: p50/p95/p99/max ms (p99 over ~100 frames is noisy). An interval runs from the top of one Present to the top of the next |
| | `slow33`, `slow50`, `n` | Intervals over 33.4 and 50 ms, and how many intervals |
| | `dxmt` | The **presenting thread's** time inside DXMT's entry points within those same intervals (each interval is stored with its own time, so calls and durations can't fall into different windows), ms per frame, then per category `ms/calls` per frame: `present` (Present's own work, without the background throttle's and the frame limiter's sleeps and the latency wait), `presentwait` (Present's wait in `PresentBoundary` for frame N − `maxlatency`, the same wait as `STATS`' `latency`; `present` + `presentwait` is the whole Present, they don't overlap), `map` (Map and Unmap, **their waits included**), `flush`, `create` (`Create*` on this thread), `draw` and `dispatch` (each times one in 16 of its **own** calls and scales it: an estimate; all calls are counted). Nested calls count once. A Present is counted in the interval its own work and wait fall into, so a frame's interval holds the previous Present |
| | `other` | Every other thread's time inside DXMT (worker threads' `Create*`, deferred contexts' draws, …), ms per frame of the window, then the non-zero categories `ms/calls` per frame. Not part of `dxmt`, and not paired with intervals: it is drained when the line is written |
| `GPU` | `gpuspan` | Per frame, the union of the GPU execution spans (`GPUStartTime`..`GPUEndTime`) of **all** of the frame's command buffers on this device's queue, the ones the present split committed included, avg/max ms over the complete frames. A span union, not the GPU's active time; never compare it with Instruments' fragment/vertex busy time. `unavailable` when no frame in the window was complete |
| | `gpugap` | The gap between one complete frame's last GPU end and the next complete frame's first GPU start, avg/max ms over the positive gaps (frames that overlap count as no gap); `0.00/0.0` if every pair overlapped, `unavailable` without two consecutive complete frames. Gaps are taken on the finish thread, so the one across two windows counts too |
| | `frames`, `incomplete` | Complete frames (every buffer had timestamps) and frames with a buffer without timestamps; incomplete frames are left out of `gpuspan` and `gpugap` |
| | `buffers`, `missing`, `dropped` | Command buffers per frame (all frames), buffers without timestamps, and (only if non-zero) frames the queue's bounded handoff (1024) dropped before the line was written |
| sections | | A feature's own line, `<NAME> k=v …` (see below) |

Sections: a feature registers a function with `gw2::SectionRegistrar` (`src/dxmt/dxmt_gw2_stats.hpp`) that returns its part of the window; each non-empty one is logged as its own line after `GPU`. A feature that adds a section documents it here.

## Other log lines

| Line | When |
|---|---|
| `GW2 fork <commit>: DXMT_STATS=1 DXMT_HITCH_MS=… DXMT_PRESENT_SPLIT=… DXMT_DISPLAY_SYNC=… DXMT_PRESENT_DEADLINE=… DXMT_CAP_LIMITER=… DXMT_STENCIL_PFV=… DXMT_DEPTH_PFV=… DXMT_BG_FPS=… DXMT_BG_FPS_MINIMIZED=… DXMT_BG_POLICY=… DXMT_RENDER_THREAD_PRIORITY=… DXMT_PSO_BLEND_HASH=… DXMT_PSO_MRU=… DXMT_PSO_TOPOLOGY_KEEP=… DXMT_CHUNKS=… DXMT_MAX_FRAME_LATENCY=… DXMT_COMPILE_WAIT_MS=… DXMT_PSO_ARCHIVE=… DXMT_PSO_ARCHIVE_DIR=… DXMT_PSO_FLUSH_EVERY=… cpus=… refresh=… set: …` | Once per process, at the first Present: the build, the **effective** value of every switch above (defaults included; `DXMT_PRESENT_DEADLINE=refresh` is one refresh, `DXMT_CHUNKS` is the ring size in effect, `DXMT_MAX_FRAME_LATENCY=unset` leaves the game's value, `DXMT_PSO_ARCHIVE` is `1` when the archives are on, `0` when not asked for, `failed` when asked for but they couldn't start, `DXMT_PSO_ARCHIVE_DIR=default` is the user cache dir), the CPUs the game sees, the refresh rate (0 if unknown), then every `DXMT_*`, `WINENCPU` and `WINEMSYNC` variable set (`set: none` if none). A PR that adds a switch adds its effective value here |
| `HITCH frame=… ms=… commit=…x… sync=…x… bgsleep=… dxmt(ms/calls): …` | A Present-to-Present interval of at least `DXMT_HITCH_MS`, logged at the top of the Present that ends it. `frame` is the frame that Present presents (1-based, as the latency fence counts); the interval holds the previous Present and the game's work on this frame. `commit` (waits for a free chunk) and `sync` (Map's waits for the GPU) are ms × count, from the same interval; they overlap the categories they happen in. `bgsleep` is whether the background throttle was on. The categories are as `PACE`'s `dxmt` (`presentwait` is the latency wait) |
| `SetMaximumFrameLatency(n)` | Every call; 0 is taken as 3 |
| `SetMaximumFrameLatency(n): DXMT_MAX_FRAME_LATENCY keeps m` | Every call while `DXMT_MAX_FRAME_LATENCY` is set: the game asked for `n`, `m` stays in effect |
| `CommandQueue: chunks=N (default\|DXMT_CHUNKS) metalbuffers=M maxlatency=L (…)` | Once per device: the ring's size, the Metal queue's bound on command buffers, and the frame latency it starts with (`DXMT_MAX_FRAME_LATENCY`, or 3 until the game sets it) |
| `DXMT_CHUNKS=… is not 32, 64 or 128; using 32`, `DXMT_MAX_FRAME_LATENCY=… is not 1-16; ignored` | Once, at device creation, for a value out of range (warnings) |
| `Presenter: display sync on/off` | When `DXMT_DISPLAY_SYNC` switches the layer's vsync |
| `Present: background throttle policy P (DXMT_BG_POLICY), N fps in the background (DXMT_BG_FPS), M fps minimized (DXMT_BG_FPS_MINIMIZED)` | Once, at the first Present (a warning before it if `DXMT_BG_POLICY` is neither `focus` nor `occlusion`) |
| `Present: <reason>, throttling to N fps (policy …, <details>)` | The background throttle engages, or its rate or reason changes. Reasons: `window minimized` (details `IsIconic`); `another process in the foreground` (focus: `foreground window H pid N, ours M`, or `no foreground window (pid 0), counted as another process; ours M`); `window not visible (occluded)` and `window miniaturized` (occlusion: `visibility 0x…: visible= miniaturized= activeSpace=`) |
| `Present: presenting normally again (was: …)` | It lets go |
| `PixelFormatView (no lossless compression): format F bind B WxH` | Once per format and bind flags of a render or depth target that keeps the flag |
| `RenderThreadPriority: thread T QoS OLD -> NEW (pthread_set_qos_class_self_np user-interactive: ok\|error E, left unchanged)` | `DXMT_RENDER_THREAD_PRIORITY` 1 or 3, once per presenting thread: its Win32 thread id, `qos_class_self()` before and after, and the call's result (an errno) |
| `RenderThreadPriority: thread T priority OLD -> NEW (SetThreadPriority THREAD_PRIORITY_HIGHEST: ok\|error E, left unchanged)` | `DXMT_RENDER_THREAD_PRIORITY` 2 or 3, once per presenting thread: `GetThreadPriority` before and after, and `GetLastError` on failure |
| `RenderThreadPriority: thread T: DXMT_RENDER_THREAD_PRIORITY=N isn't 1, 2 or 3, left unchanged` | Any other value |
| `DXMT_DEPTH_PFV=1: keeping PixelFormatView on typeless single-plane depth targets (first: format F bind B WxH)` | Once, the first time `DXMT_DEPTH_PFV` keeps the flag (the `PixelFormatView` line above then follows per format and bind flags) |
| `DEPTHVIEW base=… view=… swizzle=… type=… size=… usage=… pfv=… aspect=… nil=…` | Once per distinct view of a depth/stencil texture, and for any view that came back nil (a warning then); at most 64, then `DEPTHVIEW: more than 64 distinct views, the rest are suppressed`. Replaces the old stencil-only `TextureView` warning. Fields below |
| `DEPTHSRV format=… size=… pfv=… readonly=… stage=… slot=… view=… swizzled=… aspect=…` | Once per texture (with `DXMT_STATS` on) that is a render pass's depth-stencil attachment and is read by a shader of the same pass as a shader resource; at most 64, then `DEPTHSRV: more than 64 textures, the rest are suppressed`. Fields below |
| `PSO: blend hash on/off (DXMT_PSO_BLEND_HASH), MRU on, 4 entries/off (DXMT_PSO_MRU), topology keep on/off (DXMT_PSO_TOPOLOGY_KEEP)` | Once per device: the effective pipeline lookup settings |

| `Present pacing: mode=… rate=… refresh=… cap=… interval=… duration=… ms limiter=… ms deadline=…` | At the first Present and whenever one of its values changes. `mode`: `vsync` (display sync alone), `vsync+after` (display sync and a minimum duration of `duration`), `vsync+limiter` (display sync and the frame limiter at `limiter`, `DXMT_CAP_LIMITER`), `vsync (cap dropped)` (`DXMT_CAP_LIMITER=0` and a cap that isn't a whole number of refreshes), `after` and `plain` (display sync off: DXMT's own minimum duration, or none). `rate`: the frame rate the pacing aims for (`uncapped` without a limit). `refresh`: the display's rate, `(assumed)` when DXMT didn't get one and uses 60. `cap`: `d3d11.preferredMaxFrameRate` (0 = none). `interval`: the game's sync interval. `deadline`: what a present without a minimum duration asks `presentDrawable:atTime:` for (`DXMT_PRESENT_DEADLINE`: one refresh by default, `=8` → `8.0 ms`, `=12` → `12.0 ms`, `=0` → `off`); `- (minimum duration)` when the minimum-duration branch ignores it |
| `Promote flush: a speculative commit (occlusion/timestamp query End) waits for N requests, at most M ms …` | Once per device, when `DXMT_PROMOTE_FLUSH_MIN` is on |
| `Promote flush: watchdog on (multithread-protected device), commits a held commit after M ms` | Once, at the first held commit on a multithread-protected device |
| `Promote flush: no watchdog, the device isn't multithread-protected; …` | Once, at the first held commit on a device without multithread protection |

### Render thread priority

`DXMT_RENDER_THREAD_PRIORITY` (#9) is an experiment: in Lion's Arch GW2's render thread ran 22% of
its time on Performance cores while the main thread and DXMT's encode thread stayed on Super cores.
It acts on the first Present of each thread that presents, which in GW2 is the bgfx render thread,
and never on DXMT's own threads (the encode thread keeps `THREAD_PRIORITY_TIME_CRITICAL`).

- `=1`, **QoS class.** Bridge call 153 (`WMTSetThreadQoSClassSelf`) runs on the calling thread and
  calls `pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0)`. The QoS class is what
  macOS uses to choose between Super and Performance cores. Wine itself never sets a QoS class.
- `=2`, **Win32 priority.** `SetThreadPriority(THREAD_PRIORITY_HIGHEST)`. In this engine's Wine
  (athei/wine `d23b39774528`, `server/thread.c` `apply_thread_priority`) the wineserver maps it
  to Mach policies on the thread: `THREAD_PRECEDENCE_POLICY` importance 9 (normal is 0),
  timeshare kept, and latency and throughput QoS tier 0. These are scheduling policies, not a
  QoS class. (`THREAD_PRIORITY_TIME_CRITICAL`, the encode thread's, turns timeshare off and gives
  importance 32.)
- `=3`, both: `SetThreadPriority` first, then the QoS class, because a `thread_policy_set` from the
  wineserver probably takes a thread out of its QoS class.

There is no restore: the render thread lives as long as the game, and only the thread itself can
set its QoS class. Not verified in GW2 yet: whether macOS accepts the QoS class on a thread the
wineserver has already given Mach policies (every Wine thread gets them at start; the log line
shows the errno if not), and whether a later `SetThreadPriority` by the game undoes it.

### DEPTHVIEW and DEPTHSRV (#2)

They look for what makes fire and smoke draw dark smudges since `007a574`: GW2's soft particles read
the depth buffer, which no longer has `PixelFormatView` (so Metal compresses it).

`DEPTHVIEW` is written where DXMT makes a Metal texture view (`TextureView`, `src/dxmt/dxmt_texture.cpp`),
which happens once per view and allocation, never per draw. A tuple is (base format, view format,
texture type, size, usage, nil).

| Field | Meaning |
|---|---|
| `base` | The texture's Metal pixel format |
| `view` | The view's Metal pixel format, without DXMT's swizzle bits (`R32X8X32` shows as `Depth32Float_Stencil8`) |
| `swizzle` | The view's swizzle (`0`, `1`, `R`, `G`, `B`, `A` per channel): `RGBA` for a plain view, `R001` for a depth view of a depth-stencil buffer (`R24_UNORM_X8_TYPELESS`, `R32_FLOAT_X8X24_TYPELESS`), `0R01` for its stencil view |
| `type` | The view's `MTLTextureType` (2 = 2D, 3 = 2D array, …) |
| `size` | The texture's mip 0 width × height |
| `usage`, `pfv` | The texture's `MTLTextureUsage` bits (1 read, 2 write, 4 render target, 16 pixel format view, 32 atomic), and whether `PixelFormatView` is among them (0: Metal may compress it losslessly) |
| `aspect` | The planes the view covers: `depth`, `stencil`, `depth+stencil`, `none` (not a depth/stencil view) |
| `nil` | 1 if `newTextureView` returned nil: the shader then reads nothing from that view |

`DEPTHSRV` is checked where DXMT encodes a shader's resources into the argument buffer
(`encodeShaderResources`, `src/dxmt/dxmt_context.cpp`): each new render pass marks every SRV dirty,
so each texture a shader of the pass actually uses is encoded there at least once, and only the
shader's own bindings count. The front end notes the pass's depth-stencil texture when it sets up the
pass; the check is one pointer comparison per encoded texture, and it stops for the rest of a pass after
its first hit. With `DXMT_STATS=0` it isn't armed at all. A texture is recognised by its address, so a
new texture at a freed one's address isn't logged again. The check compares textures, not
subresources: a pass that writes one mip or slice while reading another also shows up.

| Field | Meaning |
|---|---|
| `format`, `size`, `pfv` | The depth-stencil texture's Metal format, mip 0 size, and whether it has `PixelFormatView` |
| `readonly` | The pass's read-only planes from the DSV flags (`depth`, `stencil`, `depth+stencil`, `none`). D3D11 only lets a plane be read while it's bound as depth if it's read-only, so `depth` or `depth+stencil` is the soft-particle case |
| `stage`, `slot` | The shader stage and SRV slot that reads it |
| `view`, `swizzled`, `aspect` | The SRV's Metal format (without swizzle bits), whether it's one of DXMT's swizzled formats, and the planes it covers |

### BG

`BG throttled=T/N share=S% bgsleep=avg/max fps=F reason=R policy=P`, only for a 2 s window in
which the background throttle engaged. **A `BG` line marks that window's `STATS`, `PACE` and `GPU`
lines as throttled: they aren't game performance.** From process-wide counters fed by every
swapchain's throttle on the game's render thread.

| Field | Meaning |
|---|---|
| `throttled` | Presents with the throttle engaged / all Presents in the window |
| `share` | The share of the window spent throttled (each Present-to-Present interval counts under the decision of the Present that started it) |
| `bgsleep` | ms the throttle slept per Present (all Presents of the window), avg/max |
| `fps` | The throttle's rate at the window's last Present; `0`: it had let go |
| `reason` | Why it last engaged: `minimized`, `foreground` (another process), `noforeground` (no foreground window, pid 0), `occluded`, `miniaturized` |
| `policy` | `DXMT_BG_POLICY` |

### PSO

`PSO builds= mru= lookups= equal= depth=avg/max new= topokept= lock=wait/held` (jdrly/dxmt #6):
the pipeline lookup on the render thread. Counts are per frame (encode-thread frames of the
window) unless noted; all contexts and all three pipeline caches (plain, geometry,
tessellation) together. Omitted when nothing was looked up.

| Field | Meaning |
|---|---|
| `builds` | Pipeline keys built (`InitializeGraphicsPipelineDesc`): one per pipeline change that reached a draw |
| `mru` | Of those, hits in the context's MRU (`DXMT_PSO_MRU`): no cache lookup |
| `lookups` | Lookups in the device's pipeline caches |
| `equal` | Key equality calls inside the caches (libc++ compares a node only when its full hash matches, so with the upstream hash every blend variant of a key costs one). The MRU's own compares are not counted |
| `depth` | Nodes in the looked-up key's bucket, average/max, over one lookup in 16 per thread |
| `new` | Pipelines created in the window (a total, not per frame) |
| `topokept` | Topology changes that kept the pipeline (`DXMT_PSO_TOPOLOGY_KEEP`) |
| `lock` | The caches' lock: time waiting for it / holding it (unlock included), ms per frame, one lookup in 16 timed and scaled |

With all three switches off the numbers measure the upstream lookup: `mru=0`, `topokept=0`,
`lookups` = `builds`.

### RING

`RING chunks= occupancy=avg/peak stalls= stalled= ms=avg/max` (#8): the chunk ring the render
thread commits into. A chunk is in flight from `CommitCurrentChunk` until the finish thread has
seen its command buffer complete (waiting to encode, encoding, on the GPU, or done but not yet
retired). A line holds whole frames only: a frame's totals join the window at the next frame's
first commit, so it can land one window late, and "per frame" divides by the frames the line
holds, not STATS' `frames`. No line in a window without commits.

| Field | Meaning |
|---|---|
| `chunks` | The ring's size (`DXMT_CHUNKS`); at most `chunks` − 1 are in flight |
| `occupancy` | Chunks in flight right after each commit: the mean over the window's commits, and the peak (high-water mark). A peak of `chunks` − 1 means the ring filled |
| `stalls` | Commits that found the ring full and blocked, per frame |
| `stalled` | Frames in the window with at least one ring stall |
| `ms` | Time blocked on a full ring, ms per frame (avg over the window's frames), and the longest one frame's total. Part of STATS' `commit`, which also counts the non-blocking commits' tiny cost |

### PRESENT

`PRESENT presented=avg/p95/max aftergpu=avg/max nogpu= missed= starved= target=NxR n= dropped= lost=`

When the drawables reached the screen (#7). Just before each present the encode thread adds a
presented handler to the drawable (unix call 148, `MTLDrawable_trackPresented`); the handler
records the drawable's `presentedTime` together with the time the present was encoded (the unix
side's `CACurrentMediaTime()` right before `presentDrawable*`), in a ring of 64 on the unix side.
The encode thread collects the finished records at the next present, without waiting (a trylock;
if a handler holds the ring, they come next time). The line covers the records collected in the
window, so it trails the frames by a present or two. Off with `DXMT_STATS=0` (no handlers then).

| Field | Meaning |
|---|---|
| `presented` | `presentedTime` minus the present's encode time, avg/p95/max ms over the shown frames: the queue to the screen. With `vsync+after` it includes the minimum duration |
| `aftergpu` | `presentedTime` minus the frame's last GPU end that DXMT records (its frame command buffers; the split present buffer's copy isn't among them), avg/max ms: the wait for the vsync and the compositor after the GPU finished. The GPU ends are kept per frame until the frame's record is joined (up to 2048 frames). A record whose frame the finish thread hasn't finished yet is joined in the next window, so a few frames can count in the window after the one they were shown in |
| `nogpu` | Shown frames left out of `aftergpu`: no GPU end recorded for the frame (or only `GPUEndTime` 0), still unfinished a window later, or a GPU end that isn't before `presentedTime` (should stay near 0) |
| `missed` | Refreshes skipped between two consecutive shown frames beyond the `target`, when the later frame had been encoded **before** the earlier one reached the screen: it was queued and still missed the vsync (the GPU or the compositor was late) |
| `starved` | The same skipped refreshes when the later frame was encoded **after** the earlier one reached the screen: the game or the encode thread delivered it late, or its deadline (encode time + `DXMT_PRESENT_DEADLINE`) lay past the next vsync |
| `target` | `N`x`R`: the refreshes a frame is meant to stay on screen times the refresh period in ms. `N` is 1 at a plain vsync and 2 or 3 for a 30 or 20 fps cap on 60 Hz. With the frame limiter (`vsync+limiter`) it is the limiter interval in refreshes, such as `1.20` for a 50 fps cap on 60 Hz, whose frames alternate between one and two refreshes. `missed=- starved=- target=-` with display sync off, where there is no vsync grid |
| `n`, `dropped` | Records collected, and drawables with `presentedTime` 0 (never shown). A dropped frame isn't paired with its neighbours |
| `lost` | Records the ring or the collector dropped because they weren't collected in time (should stay 0) |

With a whole `N`, each gap is rounded to whole refreshes and whatever lies beyond `N` counts. With the
frame limiter, the gaps are counted against its cadence instead: each gap's lateness against the
limiter interval adds up (never below 0), and each time the sum reaches a refresh (less 0.25 ms
for `presentedTime` jitter), that is one skipped refresh. A steady 30 fps under a 50 fps cap on
60 Hz therefore counts about 4 skipped refreshes in 5 frames, and a steady 50 counts none. On a
120 Hz display some caps, such as 103 fps, put a frame's legitimate wait within 0.25 ms of a
refresh, and those can now and then count one skipped refresh too many.
Deadlines aren't anchored to the vsync grid yet;
that comes after the deadline A/B (`DXMT_PRESENT_DEADLINE` default, `=8`, `=12`) is measured with
this line.

### COMPILE

Shader and pipeline compiles (jdrly/dxmt #5). GW2 creates shaders just before it draws with
them; DXMT compiles each shader variant and each pipeline on its compile workers, and the encode
thread waits in `GetPipeline` when the draw that needs a pipeline is encoded before the
pipeline is built. A frame with such waits is a hitch DXMT causes.

`COMPILE variants= air=hit/miss pso=hit/miss built= failed= build=shader:ms/max pipeline:ms/max
queue=now/max threads= wait=Nx ms/max slow= [archive=hit/miss added= queued= unflushed= addfailed= capped=]`

Counts are **totals in the 2 s window**; the `COMPILE frame=` line below has them per frame. Times are ms. Without `DXMT_STATS` nothing is counted or timed (no clock reads), except the count of built pipelines that `DXMT_PSO_FLUSH_EVERY` needs.

| Field | Meaning |
|---|---|
| `variants` | New shader variants (a shader with a new input layout, blend or output setup): each one is a compile task |
| `air` | DXMT's AIR disk cache (`DXMT_SHADER_CACHE`): variants loaded from it / compiled by airconv (a cache entry Metal refuses counts as a miss) |
| `pso` | DXMT's in-memory pipeline caches, at draw time on the render thread: lookups that found a pipeline / new pipelines (each one is a compile task). Hits in a context's MRU (`DXMT_PSO_MRU`) never reach the caches and aren't counted here; the `PSO` section counts them as `mru` |
| `built`, `failed` | Pipelines Metal built (render, compute, geometry and tessellation emulation), and failed builds |
| `build` | Compile-worker time: `shader` = AIR cache lookup or airconv compile plus `newLibrary`/`newFunction`, total/max per variant; `pipeline` = `newRenderPipelineState`/`newComputePipelineState`, total/max per pipeline. Workers run in parallel, so totals can exceed the window |
| `queue` | Compile tasks submitted and not yet finished (variants and pipelines, those waiting for their shaders included): now / max in the window |
| `threads` | Compile workers (the pool starts with 2 and grows while all of them are busy) |
| `wait` | The encode thread's waits in `GetPipeline` for a pipeline that wasn't ready: count, total ms, max ms. A ready pipeline costs no wait and isn't counted |
| `slow` | Frames whose waits reached `DXMT_COMPILE_WAIT_MS` |
| `archive` | Only with `DXMT_PSO_ARCHIVE=1`: pipelines found in earlier sessions' archives / built without a hit in the window, pipelines added to this session's archive in the window, adds still queued, adds not yet serialized, adds that failed and pipelines not added because the session reached 8192 (both since start) |

**`COMPILE frame=N wait=Nx ms/max queue=Q variants= air=hit/miss pso=hit/miss built= failed=
build=shader:ms pipeline:ms [dropped=D]`** (with `DXMT_STATS`): a frame whose encode-thread
waits in `GetPipeline` added up to at least `DXMT_COMPILE_WAIT_MS` (2 ms): their count, total
and longest wait, and the compile queue when the next frame's encoding started. `variants` to
`build` are the section's fields, counted on all threads while the encode thread was on this
frame (from the start of its encoding to the start of the next frame's). The render thread
records ahead of the encode thread, so a variant or lookup can belong to a later frame of the
game; the counts say what was compiling during the hitch, not which draw asked for it. At
most one line per 200 ms; `dropped` = qualifying frames since the last line that weren't logged
(`slow` in the section counts them all). The line is written when the encode thread starts the
next frame. Nothing is skipped: the draw still waits for its pipeline.

**PSO archives** (`DXMT_PSO_ARCHIVE=1`, off by default). Metal binary archives
(`MTLBinaryArchive`), managed on the unix side of the bridge (`src/winemetal/unix/pso_archive.c`,
bridge calls 149–151):

- Files: `$(getconf DARWIN_USER_CACHE_DIR)dxmt/pso/<exe>/os-<OS build>-<GPU>/`
  (`DXMT_PSO_ARCHIVE_DIR` replaces `…/dxmt/pso`), one `<unix time>-<pid>.mtlarchive` per session
  with a `.sum` next to it (size and SHA-256). Another OS build or GPU gets a new directory, and
  the old one is removed.
- At device creation (once per process) the newest 4 sessions whose `.sum` matches are loaded for
  lookup. An archive without a matching `.sum` (an interrupted write), one Metal refuses, and
  older sessions are deleted; `newBinaryArchiveWithDescriptor:` can crash on a bad file, so
  nothing is loaded unchecked.
- Each render and compute pipeline is first built with `MTLPipelineOptionFailOnBinaryArchiveMiss`
  against those archives (a hit), and on a miss built again normally. Then it is added to this
  session's archive on a background queue (utility QoS), hits included, so a session's archive
  holds every pipeline it used. Geometry and tessellation emulation (mesh pipelines) and DXMT's
  tile pipelines are not archived.
- The archive is serialized by a low-priority thread (`dxmt-pso-archive`) every
  `DXMT_PSO_FLUSH_EVERY` (256) new pipelines, every 120 s if any are new, and soon after a
  device is released. Exit never flushes (serializing can take long, and Metal's threads may be
  gone): a session loses at most the pipelines added since the last flush. `MTLBinaryArchive` writes all its contents each
  time: the session's file grows and is rewritten (`.tmp`, then renamed; the `.sum` last).
- Not covered: the archive saves Metal's backend compile only. AIR translation is DXMT's own AIR
  cache, and the draw-time pipeline lookup still happens; the state-cache prewarm (building last
  session's pipelines at startup) is not implemented.

| Line | When |
|---|---|
| `PSO archive: on: N archives for lookup (X MB) in <dir>; this session writes <file> (keep 4 sessions, at most 8192 pipelines each)` | Once, at the first device creation |
| `PSO archive: loaded <file> (X MB)` | Each archive loaded for lookup |
| `PSO archive: deleted <file>: <reason>` | An archive skipped and removed: `no .sum`, `.sum does not match …`, `unreadable`, `Metal refused it: …`, `older than the newest 4 sessions` |
| `PSO archive: removed <os-dir> (another OS build or GPU)`, `… leftover .tmp/.sum files` | Cleanup at start |
| `PSO archive: off: <reason>` | The archives couldn't start (no cache dir, a relative `DXMT_PSO_ARCHIVE_DIR`, …) |
| `PSO archive: flushing every N new pipelines, every 120 s if any are new (not at exit)` | Once, when on |
| `PSO archive: flushed <file>: N pipelines (+M), X MB in T ms` | Each flush; `(session cap reached …)` once 8192 pipelines are in it |
| `PSO archive: flush of <file> failed after T ms: <reason>` | A failed flush; the next one tries again |
| `dxmt: PSO archive: about N pipelines added since the last flush are not saved (no flush at exit)` | At exit, on Wine's stderr (DXMT's log is gone by then), if the last completed flush missed some: adds not in it plus adds still queued (a flush still running counts as not done; an add in progress can count twice) |
| `PSO archive: N pipelines could not be added so far; first error: …` | At the next flush after an add failed |

### CMDS

`CMDS cb= cbwhy=k:v,… close=k:v,… promote=off|min:N,k:v,…` (jdrly/dxmt #4). Why the game's render
thread commits chunks (command buffers) and closes encoder passes. Counted with plain increments in
the immediate context and handed over once per chunk.

| Field | Meaning |
|---|---|
| `cb` | Chunks committed per frame, the sum of `cbwhy` (should match STATS `cb`) |
| `cbwhy` | Commits per frame by reason; reasons that didn't occur are left out, `other` is always shown and should stay 0 |
| `close` | Encoder passes (render, compute, blit) closed per frame, by what closed them |
| `promote` | `off`, or with `DXMT_PROMOTE_FLUSH_MIN`: `min` and what happened at speculative commits, **window totals** |

`cbwhy` reasons. The issue's `query` is `occlusion` + `timestamp`. A commit counts once, under the
outermost reason (a staging `Map` that waits commits through `Flush()` as `map`).

| Key | The commit |
|---|---|
| `flush` | The game's `Flush`/`Flush1` (and D3D10's) |
| `getdata` | `GetData` without `DONOTFLUSH` on a query that isn't ready, or an event query that stalls (after 64 polls `DONOTFLUSH` is ignored) |
| `map` | `Map` of a staging resource that has to wait for the GPU |
| `occlusion`, `timestamp`, `event` | Speculative (`promote_flush`): the next pass change after an occlusion, timestamp (or timestamp-disjoint), event query `End` |
| `staging`, `readback` | Speculative: after a staging → staging copy, after a device → staging copy (readback) |
| `cmdlist` | Speculative: after `ExecuteCommandList` of a list that ended a query or copied to staging |
| `present` | Present (an occluded one too) |
| `fence`, `keyedmutex`, `idle` | `ID3D11Fence` Signal/Wait; `AcquireSync`/`ReleaseSync`; `WaitUntilGPUIdle` (resize, fullscreen switch) |
| `poll`, `timer`, `watchdog` | `DXMT_PROMOTE_FLUSH_MIN` only: a held commit, committed at a poll that isn't ready, at a pass change after `DXMT_PROMOTE_FLUSH_MS`, by the watchdog |
| `other` | None of these: a commit path nobody attributed (a bug in the counting) |

`close` keys: `render` (a draw needs a render pass), `compute` (a dispatch or a UAV clear), `blit`
(a copy or update), `targets` (`OMSetRenderTargets*`), `clear`, `resolve`, `query` (event/timestamp
`End`), `flush` (Flush, fences, keyed mutex, `WaitUntilGPUIdle`, a held commit's commit), `present`,
`state` (`ExecuteCommandList`, `SwapDeviceContextState`), `upscale` (MetalFX through the context
extension), `copy` (depth-stencil copy to or from staging), `other`.

`promote` keys (with `DXMT_PROMOTE_FLUSH_MIN=<n>`), totals in the 2 s window:

| Key | Meaning |
|---|---|
| `skip` | Pass changes where a speculative commit was held back |
| `reached` | Speculative commits made because `n` requests had accumulated (with `n=1`: all of them) |
| `nodefer` | Speculative commits made at once because a request that is never held joined: event query, staging copy, readback, command list |
| `gpuidle` | Speculative commits made at once because no earlier chunk was still on the GPU (it would idle) |
| `present`, `explicit`, `poll`, `timer`, `watchdog` | Held commits ended by Present, by an explicit commit (Flush, fences, keyed mutex, waiting `Map`/`GetData`, `WaitUntilGPUIdle`), by a poll that wasn't ready, by the timer, by the watchdog |

**The rule.** Only an occlusion or timestamp query `End` can be held. Explicit Flush, fences, keyed
mutex, event queries, staging copies, readbacks, and a `Map`/`GetData` that waits are never held. A
held commit is committed at the latest:
- by **Present**, so a chunk never spans frames;
- by **any way the game can see a result**: a waiting `Map`/`GetData` and `Flush` commit as upstream;
  a `GetData(DONOTFLUSH)` that returns `S_FALSE` and a `Map(DO_NOT_WAIT)` that would wait for the
  held chunk commit it first (upstream would already have committed it), so a game that only polls
  still makes progress;
- by the **timer**: the next pass change at least `DXMT_PROMOTE_FLUSH_MS` after the first hold;
- by the **watchdog**, only on a multithread-protected device (`ID3D11Multithread`): a thread waits
  `DXMT_PROMOTE_FLUSH_MS` after a hold and, if it is still pending, commits under the device lock
  every context entry point takes. Without protection that lock is a no-op and no other thread may
  enter the immediate context safely, so there is no watchdog (a log line says so); then the timer
  waits for the next pass change, poll or Present. That leaves only the GPU's time at risk, never a
  result the game waits for.

### ENCODE

`ENCODE draws= dispatches= tile= pso= useres= blitcmds= enc=k:v,… perchunk=avg/max loadMB=… storeMB=…
refuse=k:v,…` (jdrly/dxmt #4). What the encode thread encodes, per frame. The command counts are
plain increments decided at compile time; the encode thread hands its totals over once per chunk.

| Field | Meaning |
|---|---|
| `draws`, `dispatches` | Draw and dispatch commands encoded, DXMT's internal ones (clears, emulation) included |
| `tile` | Tile dispatches (tile barriers inside a render pass) |
| `pso` | Render and compute pipeline binds |
| `useres` | `useResource` commands (render and compute) |
| `blitcmds` | Blit commands |
| `enc` | Metal encoders per frame by type, as encoded after the optimizer's merges: `render`, `compute`, `blit`, `clear`, `resolve`, `present`, `upscale`, `event` (signal/wait), `timestamp` |
| `perchunk` | Encoders per chunk: average, and the most in one chunk |
| `loadMB`, `storeMB` | Attachment bytes per frame (MB = 10⁶) loaded (`Load` action) and stored (`Store`, plus resolved targets), by aspect `color`/`depth`/`stencil`: render area × layers × texel size × samples. Uncompressed, an upper bound (lossless compression moves less); clear and resolve passes included, the present pass not |
| `refuse` | Why the encoder optimizer (`checkEncoderRelation`) didn't merge, per frame. Per render → render pair it tried: `target` (different targets, planes, read-only flags, array length: the usual case), `argbuf` (argument buffers in different blocks), `load` (the later pass clears or doesn't load), `store` (the earlier pass doesn't store), `vfence` (the later pass's vertex stage waits for the earlier one's fragments). For any pair: `dep` (a data dependency ends the search) and `sync` (an event or timestamp encoder ends it) |
