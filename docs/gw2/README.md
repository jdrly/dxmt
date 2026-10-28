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
| `DXMT_RENDER_THREAD_PRIORITY` | off | On the first Present of each thread that presents (GW2's render thread): `=1` sets the macOS QoS class user-interactive (`pthread_set_qos_class_self_np` through the bridge), `=2` calls `SetThreadPriority(THREAD_PRIORITY_HIGHEST)`, `=3` both; see [Render thread priority](#render-thread-priority) | unset or `=0`: the thread keeps the priority and QoS it had |
| `DXMT_DEPTH_PFV` | off | `=1` keeps `PixelFormatView` (no lossless compression) on typeless single-plane depth targets (`R32_TYPELESS`, `R16_TYPELESS` bound as depth), the part `DXMT_STENCIL_PFV` doesn't cover (#2) | — |
| `DXMT_BG_POLICY` | `focus` | What "in the background" means for `DXMT_BG_FPS`. `focus`: another process's window is in the foreground, or none is (pid 0, counted as another process). `occlusion`: GW2's window isn't visible on screen (`NSWindow.occlusionState` without `Visible`, read through the bridge at most 10 times a second) or is miniaturized (`DXMT_BG_FPS_MINIMIZED`), whoever has the focus, as d9mt does. A minimized window (`IsIconic`) throttles under both | `focus` |

Not switchable: `SetMaximumFrameLatency(0)` means the default, 3 (DXGI's definition). Typeless
depth targets without stencil (`R32_TYPELESS`, `R16_TYPELESS` as depth) get `PixelFormatView` only
with `DXMT_DEPTH_PFV=1`.

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
