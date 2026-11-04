# The GW2 fork's switches and log lines

Everything the fork adds on top of upstream DXMT. A pull request that adds or changes a switch
or a log line updates this file in the same PR.

GW2 on Mac sets `DXMT_LOG_PATH` to its `work/logs`, so the lines below land in
`Gw2-64_d3d11.log` there. The file is rewritten each time the game creates its device.

Every line also goes to Wine's debug output (stderr), cut there into pieces of at most 900 bytes,
because Wine raises `0x80000005` (`STATUS_BUFFER_OVERFLOW`) for a line over its 1020-byte buffer
(#43). A continuation starts with the level prefix and `... `, and there a NUL byte is written as
`\0`; the log file keeps whole lines and their bytes.

## Switches

| Variable | Default | What it does | Off restores |
|---|---|---|---|
| `DXMT_STATS` | on | The 2 s statistics lines (`STATS`, `STATS2`, `PACE`, `GPU`, the sections), `HITCH` lines, the startup line, and all their measuring (the fork's timers, the `nextDrawable` timer, the present-branch counts, the split-off command buffers' GPU timestamps) | none of it, and no measuring beyond upstream's frame statistics |
| `DXMT_HITCH_MS` | 50 | A `HITCH` line for a Present-to-Present interval (game's render thread) at least this long; `0` turns them off | — |
| `DXMT_PRESENT_SPLIT` | on | Commit the frame's command buffer before `nextDrawable` and present from a new one | the present in the frame's last buffer |
| `DXMT_DISPLAY_SYNC` | on | A present with a sync interval uses `CAMetalLayer`'s vsync (`displaySyncEnabled`); a minimum duration only for intervals over one refresh, half a refresh short | DXMT's own pacing (minimum duration of exactly one refresh, display sync off) |
| `DXMT_PRESENT_DEADLINE` | one refresh | A present without a minimum duration goes out with `presentDrawable:atTime:` now + the deadline, so macOS can raise the GPU clock; `=<ms>` sets it. The control file's `present_deadline` replaces it while the game runs ([Control file](#control-file)) | `=0`: a plain `presentDrawable:` |
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
| `DXMT_COMPILE_DEMAND` | off | The compile service's `declared` policy (C2; see [COMPILE](#compile)). `=1` (exactly): a pipeline's shader variants are taken as its dependencies when it is submitted, and it waits for them unqueued instead of running and parking; a demand (the encode thread waiting in `GetPipeline`) moves the pipeline and its queued variants ahead of everything else, once each, and reaches what a demanded job parks on and dependencies submitted later; a speculative lane for warming (W2/W3) admits one job at a time and none while a demand waits, and a parked speculative job resumes through that admission again. Changes the order of compile jobs, not what is built. Parsed once per process | unset or anything but `1`: the `legacy` policy, the scheduler as before (C1) |
| `DXMT_WARM` | off | Shader warming W2/W3 (see [WARM](#warm)). `functions`: at device creation a lowest-priority thread reads the recipes #57 recorded (`recipes_1.db`, first-use order, bridge call 193) and queues a speculative job per recorded vertex and pixel variant that makes its MTLFunction from the AIR in the shader cache (never a translation). `all`: also a job per recipe that builds its pipeline from those functions through the pipeline builder (C3). Results go into shared tables keyed by stable key (C4) that the live variant and pipeline jobs consult when they are created (a done result completes them without a job) and when they run: each key is built once per process, whoever comes first; a live job that finds a warm job building its key waits for it (a demand promotes it). Speculative jobs run one at a time and never while a demand waits. Turns on the declared compile policy (as `DXMT_COMPILE_DEMAND=1`). Pipelines are built with the device's `newRenderPipelineState` on both the Metal 3 and Metal 4 queue paths. Parsed once per process | unset, `off` or anything else: no thread, no tables, nothing read |
| `DXMT_WARM_MAX` | 2048 | With `DXMT_WARM`: recipes read at most (so at most this many pipelines and twice as many functions); the rest is counted as `capped` | — |
| `DXMT_COMPILE_WAIT_MS` | 2 | A `COMPILE frame=…` line for a frame whose encode-thread pipeline waits add up to at least this many ms (at most 5 lines a second); `0` turns them off. Needs `DXMT_STATS` | — |
| `DXMT_PSO_ARCHIVE` | off | `=1`: Metal binary archives for pipelines. Pipelines are looked up in the archives of the last 4 sessions, and every render and compute pipeline built is added to this session's archive (see [COMPILE](#compile)) | no archives: no lookup, no capture, no files |
| `DXMT_PSO_ARCHIVE_DIR` | user cache dir | With `DXMT_PSO_ARCHIVE=1`: an absolute unix directory that replaces `$(getconf DARWIN_USER_CACHE_DIR)dxmt/pso` | — |
| `DXMT_PSO_FLUSH_EVERY` | 256 | With `DXMT_PSO_ARCHIVE=1`: serialize this session's archive every this many new pipelines (also every 30 s if any are new; never at exit) | — |
| `DXMT_PROMOTE_FLUSH_MIN` | 0 | `=<n>`: a speculative commit of an occlusion or timestamp query `End` waits until `n` such requests have accumulated, with a progress guarantee (see [CMDS](#cmds)). `1` commits as upstream does | `=0`: upstream (commit at the next pass change) |
| `DXMT_PROMOTE_FLUSH_MS` | 2 | With `DXMT_PROMOTE_FLUSH_MIN`: the longest a held commit waits (the timer at the next pass change, and the watchdog's period) | — |
| `DXMT_TIMESTAMP_MODE` | `estimate` | How a timestamp query `End` (and a timestamp-disjoint one) is taken. `estimate`: the open pass stays open and nothing is committed; the value is interpolated between the GPU start and end of the command buffer the query lands in, by its position among the chunk's encoders. `boundary`: the pass stays open, but the commit still happens at the next pass change (as `exact`, so `DXMT_PROMOTE_FLUSH_MIN` applies); the value is interpolated the same way. Both keep the values monotonic and make a disjoint query ready only after its chunk's timestamps are resolved; see [TIMESTAMP](#timestamp) | `exact` (or `0`): upstream, a pass split and a commit per timestamp, the value the buffer's GPU end |
| `DXMT_MAP_FAST` | on | The Map/Unmap fast path for buffers (see [MAP](#map)): the immediate context maps a dynamic or default-usage buffer from fields the buffer sets at creation (no virtual lookup, no reference count), `Unmap` of a buffer returns at once, the end of a page renames under one lock, a buffer that uses up its page within one chunk gets pages twice as large (up to 64 KB, at least two suballocations each), and a default-usage buffer's renames (`UpdateSubresource` of the whole buffer) come from CPU-visible suballocated pages, so an update is a `memcpy` instead of a new Metal buffer written through a unix call. `UpdateSubresource` of such a buffer skips its other virtual calls too. `PACE`'s `map` times one call in 16 and scales it, as `draw` does | `=0`: upstream's Map, Unmap and renames, `map` timed on every call |
| `DXMT_SKIP_REBIND` | on | A draw's pipeline bind is left out when its render encoder already has that pipeline bound (see [REBIND](#rebind)). Each encoder tracks the pipeline its own command list bound last; a new encoder starts with none, and any other pipeline bind in it (an internal pass's, the tile barrier's) clears it, so the first draw after those always binds. Only the bind is dropped: depth-stencil, rasterizer, blend color, viewports and scissors are separate commands, emitted as before | `=0`: every pipeline change binds, as upstream |
| `DXMT_CONTROL` | off | `=<path>`: the control file (see [Control file](#control-file)). A thread reads it once a second, never the render thread; its keys change settings while the game runs, for A/B minutes within one start. Today's key: `present_deadline`, which replaces `DXMT_PRESENT_DEADLINE` while the file sets it | unset: no thread, no file access, every setting from its variable |
| `DXMT_RESIDENCY_SET` | on | One `MTLResidencySet` for the Metal device, attached to every DXMT command queue on it (macOS 15+; see [RESIDENCY](#residency)). Buffers and textures enter it when they are created and leave it when they are freed, which happens only after the last chunk that used them has retired; the set's changes are committed right before a command buffer commit, only when it changed. `makeResident` emits no `useResource` for them (`ENCODE useres` near 0). Left out, with `useResource` as before: heap-placed and shared (IOSurface) resources, CPU-placed memory (i386), DXMT's bare Metal buffers (dummy constant buffer, GS marshal). Drawables never enter it. Texture views and buffer views ride on their parent | `=0`: no set, a `useResource` per resource and encoder (upstream) |
| `DXMT_UNRETAINED` | on | A chunk's command buffers (the frame's and the present split's) come from `commandBufferWithUnretainedReferences` (bridge call 154), so Metal retains nothing they use; DXMT holds it until the finish thread retires the chunk (see [LIFECHECK](#lifecheck)): the drawable, the presenter, events and MetalFX scalers are held in the retirement ledger until the chunk retires instead of being released after encoding, and device teardown waits for every committed chunk before the D3D11 device's pipeline, depth-stencil and sampler caches go (the same with `DXMT_LIFETIME_CHECK=1`). Metal 4 plan phase 1 (#31), groundwork for the Metal 4 queue (#32). On by default since the lifetime check (20+ min of busy Lion's Arch with a map change and alt-tabs) found 0 early releases | `=0`: `[queue commandBuffer]`, as upstream |
| `DXMT_LIFETIME_CHECK` | off | **Diagnostic build only** (meson `-Dgw2_lifetime_check=true`; in any other build it is compiled out and `=1` is ignored with a warning). `=1`: as d9mt's `D9MT_LIFETIME_CHECK`. Every object a chunk's command buffers use is stamped with the chunk's sequence on the encode thread; releasing DXMT's last reference to one before the GPU completed that chunk logs a `LIFETIME` line (once per object type) and counts in [LIFECHECK](#lifecheck). Works with `DXMT_UNRETAINED` on or off (it checks DXMT's own bookkeeping, not Metal's retains). It also turns on the ledger's holds and teardown waits that `DXMT_UNRETAINED=1` uses. Costs a lock and a hash lookup per stamp, retain and release: for test runs only | `=0`: no stamps, no counting; a normal build has no code for them at all |
| `DXMT_METAL4` | on | Metal 4 for this process's devices (Metal 4 plan phase 2, #32; see [Metal 4](#metal-4)): an `MTL4CommandQueue` with one `MTL4CommandAllocator` and one residency set per chunk slot, Metal 4 command buffers with commit feedback, today's packets replayed on Metal 4 encoders through argument tables, and present through `waitForDrawable`/`signalDrawable` and the drawable's own present (the same deadline, minimum-duration and present-split behaviour). Decided once per process at the first command queue. Stays on Metal 3, with a warning, on a device without Metal 4 (macOS before 26, or not `MTLGPUFamilyMetal4`), without the residency set (`DXMT_RESIDENCY_SET`), without `DXMT_UNRETAINED=1`, or with `DXMT_METALFX_SPATIAL_SWAPCHAIN=1`. **On by default since 2026-11-04** (#32's follow-up), after Jan's 10-minute in-game session on it: no device errors, no missed resources, no visual problems | `=0`: Metal 3, exactly as before; no bridge call, no log line |
| `DXMT_SUBSAMPLE_METALFX` | off | GW2's Render Sampling: Subsample pass with MetalFX (#56; see [SUBSAMPLE](#subsample)). `log`: capture only, every candidate draw's shape is logged once and nothing changes. `1`: an accepted pass (the full-screen draw that samples the low-resolution 3D scene into a target of the swapchain's size) samples a MetalFX spatial upscale of its source instead, on Metal 3 (`MTLFXSpatialScaler`) and Metal 4 (`MTL4FXSpatialScaler`, bridge calls 190–192); the game's pixel shader still runs, and the UI drawn after it is untouched. Anything that doesn't match runs unchanged | unset or `=0`: no detection, no counting, no bridge call |
| `DXMT_SUBSAMPLE_METALFX_PS` | unset | With `DXMT_SUBSAMPLE_METALFX`: only a pass whose pixel shader's SHA-1 (as the candidate line prints it) starts with this hex prefix is accepted (`no(pin)` otherwise) | any pixel shader that passes the other checks |
| `DXMT_PSO_RECORD` | on | Shader warming W1 (#57; see [RECORD](#record)): records, durably, what each pipeline was built from, so later stages (W2/W3) can prepare shaders and pipelines before GW2 asks for them. Into `$(getconf DARWIN_USER_CACHE_DIR)dxmt/<exe>/recipes_1.db` (its own sqlite file, through the bridge's cache writer): each shader's DXBC by its SHA-1, each variant's translation inputs (VS, PS, plain) by shader SHA-1, variant digest and Metal target, and each plain graphics pipeline's recipe by the SHA-1 of an explicit serialization of the target, the vertex/pixel identities and every field of the descriptor Metal gets. Once per key and process. The callers never wait on the recorder: a fixed lock-free table of key fingerprints (2 MB) and a bounded lock-free queue (16,384 records or 128 MB), then counted drops. One lowest-priority writer thread skips rows the file already has and stops at a 256 MB file, or after 8 failed writes in a row; it never calls the logger (Present logs its lines). Changes nothing that is built or drawn | `=0`: no recording, no thread, no database opened |
| `DXMT_PSO_GENERATIONS` | off | `=1` (exactly), with `DXMT_PSO_ARCHIVE=1` (#75; see [COMPILE](#compile), *Generations*): the archives are published as generations in `…/os-<OS build>-<GPU>/gen1/` instead of one file per session: each flush writes the pipelines added since the last one into an immutable shard of their own (never re-serializing an archive, which Metal fails at), a checksummed manifest lists the shards and their keys, and one index file (`CURRENT`) is replaced atomically, so a kill or a failed write leaves the last good generation. A pipeline Metal's packer names is excluded by its key from capture, in this and later sessions. One process writes a store; another gets lookup only | the session files of #52, unchanged (they and a store sit side by side; neither reads the other) |

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
| `cb` | Chunks committed per frame. A chunk the present split cut has two Metal command buffers, so Metal's count is `cb` + `split`; `GPU buffers` counts every one, the split-off buffers included |
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
| | `dxmt` | The **presenting thread's** time inside DXMT's entry points within those same intervals (each interval is stored with its own time, so calls and durations can't fall into different windows), ms per frame, then per category `ms/calls` per frame: `present` (Present's own work, without the background throttle's and the frame limiter's sleeps and the latency wait), `presentwait` (Present's wait in `PresentBoundary` for frame N − `maxlatency`, the same wait as `STATS`' `latency`; `present` + `presentwait` is the whole Present, they don't overlap), `map` (Map and Unmap, **their waits included**; with `DXMT_MAP_FAST`, the default, it times one in 16 of its calls and scales it, as `draw`), `flush`, `create` (`Create*` on this thread), `draw` and `dispatch` (each times one in 16 of its **own** calls and scales it: an estimate; all calls are counted. The 1 in 16 is a per-thread pseudo-random draw, so it doesn't alias with call sequences that repeat; the per-thread state is found without `thread_local`, #28). Nested calls count once. A Present is counted in the interval its own work and wait fall into, so a frame's interval holds the previous Present |
| | `other` | Every other thread's time inside DXMT (worker threads' `Create*`, deferred contexts' draws, …), ms per frame of the window, then the non-zero categories `ms/calls` per frame. Not part of `dxmt`, and not paired with intervals: it is drained when the line is written |
| `GPU` | `gpuspan` | Per frame, the union of the GPU execution spans (`GPUStartTime`..`GPUEndTime`) of **all** of the frame's command buffers on this device's queue, the ones the present split committed included, avg/max ms over the complete frames. A span union, not the GPU's active time; never compare it with Instruments' fragment/vertex busy time. `unavailable` when no frame in the window was complete |
| | `gpugap` | The gap between one complete frame's last GPU end and the next complete frame's first GPU start, avg/max ms over the positive gaps (frames that overlap count as no gap); `0.00/0.0` if every pair overlapped, `unavailable` without two consecutive complete frames. Gaps are taken on the finish thread, so the one across two windows counts too |
| | `frames`, `incomplete` | Complete frames (every buffer had timestamps) and frames with a buffer without timestamps; incomplete frames are left out of `gpuspan` and `gpugap` |
| | `buffers`, `missing`, `dropped` | Metal command buffers per frame (all frames; the split-off ones included, about STATS `cb` + `split`), buffers without timestamps, and (only if non-zero) frames the queue's bounded handoff (1024) dropped before the line was written |
| sections | | A feature's own line, `<NAME> k=v …` (see below) |

Sections: a feature registers a function with `gw2::SectionRegistrar` (`src/dxmt/dxmt_gw2_stats.hpp`) that returns its part of the window; each non-empty one is logged as its own line after `GPU`. A feature that adds a section documents it here.

## Other log lines

| Line | When |
|---|---|
| `GW2 fork <commit>: DXMT_STATS=1 DXMT_HITCH_MS=… DXMT_PRESENT_SPLIT=… DXMT_DISPLAY_SYNC=… DXMT_PRESENT_DEADLINE=… DXMT_CAP_LIMITER=… DXMT_STENCIL_PFV=… DXMT_DEPTH_PFV=… DXMT_BG_FPS=… DXMT_BG_FPS_MINIMIZED=… DXMT_BG_POLICY=… DXMT_RENDER_THREAD_PRIORITY=… DXMT_PSO_BLEND_HASH=… DXMT_PSO_MRU=… DXMT_PSO_TOPOLOGY_KEEP=… DXMT_CHUNKS=… DXMT_MAX_FRAME_LATENCY=… DXMT_COMPILE_WAIT_MS=… DXMT_PSO_ARCHIVE=… DXMT_PSO_ARCHIVE_DIR=… DXMT_PSO_FLUSH_EVERY=… DXMT_PROMOTE_FLUSH_MIN=… DXMT_PROMOTE_FLUSH_MS=… DXMT_TIMESTAMP_MODE=… DXMT_MAP_FAST=… DXMT_SKIP_REBIND=… DXMT_CONTROL=… DXMT_RESIDENCY_SET=… DXMT_UNRETAINED=… DXMT_LIFETIME_CHECK=… DXMT_METAL4=… DXMT_SUBSAMPLE_METALFX=… DXMT_PSO_RECORD=… DXMT_COMPILE_DEMAND=… DXMT_PSO_GENERATIONS=… DXMT_WARM=… DXMT_WARM_MAX=… cpus=… refresh=…` then `GW2 fork <commit> set: …` | Two lines (#43; one before), once per process, at the first Present: the build, the **effective** value of every switch above (defaults included; `DXMT_PRESENT_DEADLINE=refresh` is one refresh, `DXMT_CHUNKS` is the ring size in effect, `DXMT_MAX_FRAME_LATENCY=unset` leaves the game's value, `DXMT_PSO_ARCHIVE` is `1` when the archives are on, `0` when not asked for, `failed` when asked for but they couldn't start, `DXMT_PSO_ARCHIVE_DIR=default` is the user cache dir, `DXMT_PROMOTE_FLUSH_MIN`/`_MS` as the immediate context parsed them: `MS` is at least 1 and is shown even when `MIN=0` turns it off, `DXMT_TIMESTAMP_MODE` is `exact`, `estimate` or `boundary`, `DXMT_MAP_FAST` and `DXMT_SKIP_REBIND` are `1` or `0`, `DXMT_CONTROL` is the path or `off`, `DXMT_RESIDENCY_SET` is `1` when the set is on, `0` when switched off, `failed` when asked for but unavailable, `DXMT_UNRETAINED` is `1` or `0`, `DXMT_LIFETIME_CHECK` is `1`, `0` or `unbuilt` (asked for in a build without it), `DXMT_METAL4` is `1` (Metal 4 in use, the default), `0` (`DXMT_METAL4=0`), or `unsupported` (on, but the device, macOS or another setting keeps Metal 3; a warning says which), `DXMT_SUBSAMPLE_METALFX` is `0`, `log` or `1`, `DXMT_PSO_RECORD` is `1` or `0`, `DXMT_COMPILE_DEMAND` is `1` or `0`, `DXMT_PSO_GENERATIONS` is `1` (in use), `lookup` (another process writes the store), `unused` (asked for, but the archives are off or failed) or `0`, `DXMT_WARM` is `off`, `functions` or `all`, `DXMT_WARM_MAX` the cap), the CPUs the game sees, the refresh rate (0 if unknown); the second line lists every `DXMT_*`, `WINENCPU` and `WINEMSYNC` variable set (`set: none` if none). Since #60 the line is generated from the switch registry (`src/dxmt/dxmt_gw2_switches.hpp`) in this order, unchanged: a PR that adds a switch declares it with `sw::Switch` (its effective value through `sw::provideEffective` if it differs from the parsed one) and doesn't edit `LogStartup`; such switches follow `DXMT_PSO_RECORD`, sorted by name, before `cpus` and `refresh`. It adds the name here |
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
| `Promote flush: a speculative commit (occlusion/timestamp query End) waits for N requests, at most M ms (a watchdog thread under the device lock, now always on) …` | Once per device, when `DXMT_PROMOTE_FLUSH_MIN` is on |
| `Timestamp queries: exact\|estimate\|boundary (DXMT_TIMESTAMP_MODE): …` | Once per process, when the first immediate context is created: the mode in effect and what it does (a warning before it for a value that isn't `exact`, `estimate`, `boundary` or `0`) |
| `Timestamp queries: the first query End in M mode kept its pass open[ and requested no commit]` | Once per process, the first time `estimate` or `boundary` handles a timestamp or timestamp-disjoint `End` |
| `Map fast path: on (DXMT_MAP_FAST): …` | Once per device, when `DXMT_MAP_FAST` is on (the default) |
| `Metal shader cache: <path> (Metal fixed it before DXMT loaded: …)` | Once per process (`dxgi.dll` load), unless `DXMT_USE_DEFAULT_METAL_CACHE=1` (#46). Replaces upstream's `Failed to set Metal cache path, fallback to system default`. Metal fixes its shader cache path at its first device creation, and Wine's display code (winemac's GPU enumeration) creates Metal devices in every GUI process before `dxgi.dll` loads, so DXMT's own `dxmt/<exe>/com.apple.metal` can't take. The line gives the path Metal really uses (bridge call 152, `MTLGetShaderCachePath`): for GW2 on Mac, `$(getconf DARWIN_USER_CACHE_DIR)com.codeweavers.CrossOver.wineloader/com.apple.metal`, keyed by the Wine loader's bundle ID and shared by every process of the engine. That cache persists across sessions and grows. A build without the private Metal API (`DXMT_NO_PRIVATE_API`) can neither set nor read the path and logs `Metal shader cache: Metal's default (dxmt/<exe>/com.apple.metal didn't take, and this build can't read Metal's path: no private Metal API)` |
| `Pipeline binds: skip rebind on (DXMT_SKIP_REBIND): …`, `Pipeline binds: skip rebind off (DXMT_SKIP_REBIND=0): …` | Once per process, when the first encoding context is created |
| `Pipeline binds: skipped the first bind of a pipeline already bound in its encoder` | Once per process, the first time `DXMT_SKIP_REBIND` leaves a bind out |
| `Residency: one residency set on (DXMT_RESIDENCY_SET=1), attached to this device's queue: N allocations[, X MB] resident. …` | Once per device (command queue): the set is attached; its size at that moment (MB only with `DXMT_STATS` on). When a device's queue is destroyed with the set on, it first waits for its committed command buffers, so their resources stay in the set until the GPU is done with them |
| `Residency: off (DXMT_RESIDENCY_SET=0): a useResource per resource and encoder, as upstream` | Once per device, when switched off |
| `Residency: off: residency sets need macOS 15 …`, `Residency: off: newResidencySet failed (…) …`, `Residency: off for this device: the set belongs to another Metal device …` | Warnings: the set couldn't start (once per process) or a device on another Metal device (once per device); `useResource` as upstream |
| `Device error at frame F (chunk S, split-off buffer i of n, split error k, m not logged since the last line): <error>` | An error (`MTLCommandBufferStatusError`) in a command buffer the present split committed off chunk `S` of frame `F` (#10), found by the finish thread, which checks every split-off buffer when its chunk completes, with `DXMT_STATS` on or off. Followed by the buffer's logs, `Frame F (chunk S, split-off buffer i): <log>`, if it has any. The first 8 are logged, then at most one a second; `k` counts all of them since the device started, `m` those left out since the previous line. The chunk's last buffer keeps upstream's `Device error at frame F: <error>`. On the Metal 4 path (#32) both lines come from the commits' feedback (`split-off buffer i of n` counts the chunk's earlier commits), without the logs (Metal 4 has none per command buffer). Both paths write these lines from the chunk's commit records (#77; [GLOSSARY](GLOSSARY.md)) |
| `Command buffers: unretained references (DXMT_UNRETAINED=1, the default): …`, `Command buffers: retained references (DXMT_UNRETAINED=0), as upstream` | Once per device (command queue), at its creation |
| `Lifetime check: on (DXMT_LIFETIME_CHECK=1): …` | Once per process, at the first command queue, with the check on |
| `Lifetime check: DXMT_LIFETIME_CHECK=1 ignored: this build doesn't have it (…)` | Warning, once per process: the variable is set in a build without `-Dgw2_lifetime_check=true` |
| `LIFETIME <type> 0x<address> released while chunk S of queue Q may still use it (GPU completed C): the first <type> released early, N early releases so far (…)` | An error, with `DXMT_LIFETIME_CHECK=1`: DXMT released its last reference to an object that chunk `S` used, before the GPU completed it (`C` < `S`). Logged once per type; `N` and `LIFECHECK`'s `early` count all of them. See [LIFECHECK](#lifecheck) |
| `Metal 4: unavailable (Metal 4 needs macOS 26 \| the GPU isn't in MTLGPUFamilyMetal4), staying on Metal 3` | Warning, once per process at the first command queue, with `DXMT_METAL4` on (the default) on a device without Metal 4 |
| `Metal 4: needs the residency set (DXMT_RESIDENCY_SET=0\|failed: Metal 4 encoders have no useResource), staying on Metal 3` | Warning, once per process at the first command queue, with `DXMT_METAL4` on (the default) and the residency set off or unavailable (so `DXMT_RESIDENCY_SET=0`, as for a Metal System Trace, also means Metal 3) |
| `Metal 4: DXMT_METALFX_SPATIAL_SWAPCHAIN=1: MetalFX upscaling stays on Metal 3, staying on Metal 3`, `Metal 4: needs DXMT_UNRETAINED=1 (the default): …; staying on Metal 3` | Warnings, once per process at the first command queue, with `DXMT_METAL4` on (the default) |
| `Metal 4: <what> isn't supported on the Metal 4 path; skipped (its fences still pass)` | An error, once per kind (`MetalFX temporal upscale`, `MetalFX spatial upscale`): the Metal 4 path met a MetalFX encoder from DXMT's context extension, which it can't encode; it draws wrong |
| `Metal 4: geometry shader emulation \| tessellation emulation \| deferred command lists (ExecuteCommandList) run on the Metal 4 path through the packet replay, untested there` | Warning, once per kind, the first time a Metal 4 device encodes a GS or tessellation draw or executes a deferred command list (#32: the coordinator's scope decision of 2026-11-04) |
| `Metal 4: the layer has no residency set; drawables may not be resident` | Warning, on the Metal 4 path, at a presenter's first present, if `CAMetalLayer.residencySet` is nil |
| `Metal 4: unavailable (<what> failed), staying on Metal 3` | Warning, once: the Metal 4 queue, its event or allocators couldn't be created, or the residency set couldn't be attached to it |
| `Metal 4: on (DXMT_METAL4=1, the default): this device's chunks go to an MTL4CommandQueue, with N command allocators (one per chunk slot) and commit feedback` | Once per device (command queue), when it takes the Metal 4 path |
| `Metal 4: newCommandBuffer failed after N command buffers; DXMT can't continue on Metal 4, aborting` | Error, on the Metal 4 path, when Metal returns no command buffer after the queue's first ones (one per slot and two spares, made with the queue): the process aborts, since earlier chunks may still be on the GPU |
| `Residency: the residency set is attached to this device's Metal 4 queue too (DXMT_METAL4)` | Once per device, on the Metal 4 path, after the usual `Residency:` line |
| `Metal 4: chunk S's completion event not signalled after 1 s; still waiting` | Warning, on the Metal 4 path: the finish thread waits for a chunk's completion event (after its commits) over a second |
| `Metal 4: chunk S not complete after 1 s (its command buffer's value V, the queue event at E); still waiting` | Warning, on the Metal 4 path, the first 8 times the finish thread waits over a second for a chunk (a GPU hang or a very long frame); counted in [METAL4](#metal4)'s `slow`. Metal 3's `waitUntilCompleted` blocks the same way without a line |

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
lines as throttled: they aren't game performance.** From one process-wide sample fed by every
swapchain's throttle on the game's render thread: each Present adds its counts and its sleep under
one lock after it slept, and the section drains the sample under the same lock (#19), so
`throttled` never exceeds the Presents and `bgsleep` averages over the same window's Presents. A
Present falls into the window in which its sleep ended.

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
| `depth` | Nodes in the looked-up key's bucket, average/max, over one lookup in 16 (counted per pipeline cache, across its threads; #28) |
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
queue=now/max threads= wait=Nx ms/max slow= [waitcause=queued:N/ms,variantq:N/ms,variant:N/ms,building:N/ms[,other:N/ms]
[archbusy=N]] [promoted=N] [archive=hit/miss miss=new:N,known:N added= queued= unflushed= addfailed= capped= skipped= rebuilt= [gen= published= excluded= missexcl=]]`

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
| `promoted` | (C2) Only with `DXMT_COMPILE_DEMAND=1`: compile jobs a demand moved ahead (a waited-for pipeline and its unfinished variants, each once). Under the declared policy `queued` waits also include a pipeline waiting for its variants, unqueued (its state stays `queued` until a worker starts it) |
| `waitcause` | (#46) Only in a window with waits: the same waits by what the waited-for pipeline task was doing when the wait started, count / total ms. `queued`: submitted, no worker had started it (behind other compile tasks). `variantq`: started, parked on its vertex or pixel shader variant, and that variant was still queued. `variant`: parked on a variant that was compiling (or between the two). `building`: inside Metal's pipeline build (`newRenderPipelineState`/`newComputePipelineState`). `other`: not classified (submitted while `DXMT_STATS` was off, or done but not yet flagged); shown only when non-zero. The counts add up to `wait`: both come from one drain of the per-cause counters, so a wait finishing during the drain lands in one window for both |
| `archbusy` | (#46) Only with `DXMT_PSO_ARCHIVE=1`: waits that began while PSO archive adds were still queued on the unix side (each add compiles the pipeline again for the archive) |
| `archive` | Only with `DXMT_PSO_ARCHIVE=1`: pipelines found in earlier sessions' archives / built without a hit in the window, pipelines added to this session's archive in the window, adds still queued, adds not yet serialized, adds that failed and pipelines not added because the session reached 8192 (both since start) |
| `miss` | (#52) With `DXMT_PSO_ARCHIVE=1`: the window's lookup misses, `new` (never in an earlier session's archive) and `known` (an earlier session's `.keys` has the pipeline's key, yet Metal missed it: keying, or a pipeline lost before a flush) |
| `skipped`, `rebuilt` | (#52) Since start: pipelines left out of the archive after Metal's packer named them in a failed flush, and flushes that succeeded only after rebuilding the archive without them |
| `gen`, `published`, `excluded`, `missexcl` | (#75) Only with `DXMT_PSO_GENERATIONS=1`: the generation loaded at start, then the last one published; pipelines in this session's shard of that publish; adds refused since start because their key is an excluded failing shape; the window's lookup misses of an excluded shape (not counted in `new` or `known`) |

**`COMPILE frame=N wait=Nx ms/max queue=Q variants= air=hit/miss pso=hit/miss built= failed=
build=shader:ms pipeline:ms cause=queued:N,variantq:N,variant:N,building:N,other:N longest=<cause>
lead:ms queue:ms parked:ms build:ms varq:ms varc:ms depth:N archq:N [dropped=D]`** (with
`DXMT_STATS`): a frame whose encode-thread
waits in `GetPipeline` added up to at least `DXMT_COMPILE_WAIT_MS` (2 ms): their count, total
and longest wait, and the compile queue when the next frame's encoding started. `variants` to
`build` are the section's fields, counted on all threads while the encode thread was on this
frame (from the start of its encoding to the start of the next frame's). The render thread
records ahead of the encode thread, so a variant or lookup can belong to a later frame of the
game; the counts say what was compiling during the hitch, not which draw asked for it. At
most one line per 200 ms; `dropped` = qualifying frames since the last line that weren't logged
(`slow` in the section counts them all). The line is written when the encode thread starts the
next frame. Nothing is skipped: the draw still waits for its pipeline.

`cause` and `longest` (#46): the frame's waits by cause (as `waitcause`), and the timeline of its
longest wait's pipeline task, ms, `-` where not known:

| Field | Meaning |
|---|---|
| `lead` | From the pipeline's submission (on the render thread, when its draw was recorded) to the start of the wait: how much head start the compile had |
| `queue` | From submission until a worker first started the task |
| `parked` | Time the task spent parked on its shader variant |
| `build` | Metal's pipeline build (0 or close to it when Metal's own shader cache has it) |
| `varq`, `varc` | The variant it was last parked on: its time in the queue, and its compile (AIR cache lookup or airconv, `newLibrary`, `newFunction`) |
| `depth` | Compile tasks submitted and not finished when the wait started |
| `archq` | PSO archive adds queued when the wait started (`DXMT_PSO_ARCHIVE=1` only) |

The stamps (#46) cost one clock read per task submission, start, park, build and completion, and
only with `DXMT_STATS`; a wait also reads the archive's counters once (a unix call) when the
archives are on. Since C1 the compile service (`src/dxmt/dxmt_compile_service.hpp`) owns the tasks'
records: their state (queued, running, parked, building, complete, and what a task is parked on) is
kept with `DXMT_STATS` off too, a few atomic stores per task; the timestamps and the classification
above are unchanged and still need `DXMT_STATS` from the task's submission.

**PSO archives** (`DXMT_PSO_ARCHIVE=1`, off by default). Metal binary archives
(`MTLBinaryArchive`), managed on the unix side of the bridge (`src/winemetal/unix/pso_archive.c`,
bridge calls 149–151):

- Files (with #52 also a `<file>.keys` sidecar per archive: the keys of the pipelines in it, written after each successful flush and removed with the archive): `$(getconf DARWIN_USER_CACHE_DIR)dxmt/pso/<exe>/os-<OS build>-<GPU>/`
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
  `DXMT_PSO_FLUSH_EVERY` (256) new pipelines, every 30 s if any are new, and soon after a
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
| `PSO archive: flushing every N new pipelines, every 30 s if any are new (not at exit)` | Once, when on |
| `PSO archive: flushed <file>: N pipelines (+M), X MB in T ms` | Each flush; `(session cap reached …)` once 8192 pipelines are in it |
| `PSO archive: flush of <file> failed after T ms: <reason>` | A failed flush, also after the rebuild below; the next one tries again |
| `PSO archive: flush of <file> failed: <Metal's error>` | (#52) The first try of a flush failed; the archive is rebuilt and written once more in the same flush |
| `PSO archive: Metal names pipeline no. N: <shape> (left out of the archive from now on)` | (#52) The failed flush's error named a pipeline (`pipeline no. 'N'`, counted in that archive's own order): its shape (`render vs=… fs=…\|none colors=[…] depth= stencil= samples= raster= topology=` or `compute cs=…`; a function made with constant values shows as `<name>#c<hash>`). That pipeline is left out of every later flush (`skipped`); it is still built and used |
| `PSO archive: rebuilding <file> without the pipelines left out after an earlier failure` | (#52) A flush after a failed retry: the current archive still holds skipped pipelines, so it is rebuilt before it is written |
| `PSO archive: the rebuilt archive's pipeline no. N failed too: <shape> (left out from the next flush)` | (#52) The retry failed on another pipeline: it is skipped as well, the current archive is kept unchanged (a partial rebuild never replaces it), and the next flush rebuilds again before writing |
| `PSO archive: flushed <file>: … (rebuilt with N pipelines after a failed flush)` | (#52) The retry into a fresh archive (every recorded pipeline but the skipped ones) succeeded |
| `PSO archive: N pipeline keys from M of the loaded archives classify lookup misses (known/new)` | (#52) Once, at start: the keys read from the loaded archives' `.keys` sidecars |
| `dxmt: PSO archive: about N pipelines added since the last flush are not saved (no flush at exit)` | At exit, on Wine's stderr (DXMT's log is gone by then), if the last completed flush missed some: adds not in it plus adds still queued (a flush still running counts as not done; an add in progress can count twice) |
| `PSO archive: N pipelines could not be added so far; first error: …` | At the next flush after an add failed |

**Generations** (#75, `DXMT_PSO_GENERATIONS=1` with `DXMT_PSO_ARCHIVE=1`, off by default). The same
archives, published by the generation publisher (`src/winemetal/unix/pso_archive_publish.c`) instead of
one rewritten file per session, from the instance's pure policy (`pso_archive_policy.c`):

- Files in `…/os-<OS build>-<GPU>/gen1/`: shards `<sha256>.shard` (an `MTLBinaryArchive`'s
  serialization, named by its content, never rewritten); one manifest per generation,
  `g<16 hex>.manifest` (each shard's session, size, SHA-256 and pipeline keys, newest session first,
  then the excluded shapes, then a SHA-256 over all of it); the index `CURRENT` (the generation and its
  manifest's SHA-256), the only file ever replaced; `LOCK`.
- A flush publishes the pipelines added since the last publish as a new shard (a delta); a session's
  ninth publish merges its eight shards into one. The archive written is always one never serialized
  before: Metal fails to serialize an archive again after more pipelines were added to it, naming a
  good pipeline (`missing 'vertex' stage in pipeline no. N`, reproduced natively by
  `tests/gw2/pso_archive_test.m`), which the session-file mode then leaves out. The shard is written,
  synced and renamed into place, then the manifest, then `CURRENT`; a failure at any step leaves the last
  good generation, and the next flush tries again.
- At start the generation `CURRENT` names is loaded if its manifest verifies, else the newest older
  one that does (the last good one); a shard whose size or SHA-256 doesn't match is dropped. The
  newest 4 sessions' shards are used for lookup. Only the store's writer (an exclusive `flock` on
  `LOCK`) publishes and removes files: everything but the current and the previous generation's.
  Another process at the same time gets lookup only and captures nothing.
- Exact-shape failure policy: a pipeline Metal's packer names in an archive never serialized before is
  a failing shape. Every pipeline of its full key is left out, the shard is rebuilt without them and
  written again (at most 3 tries a flush), and the key is excluded from capture, recorded in the
  manifest for later sessions. Nothing broader is excluded: no class of pipelines (fragment-less or
  any other) is skipped. Excluded shapes are still built and used, only not archived. A flush whose
  new pipelines are all excluded publishes an exclusion-only generation (no new shard, every shard
  carried); until that succeeds the flush counts as failed and the next one tries again.

| Line | When |
|---|---|
| `PSO archive: on (generations[, lookup only]): generation G, N shards for lookup (X MB) in <dir>; this session publishes new generations\|captures nothing (keep 4 sessions, at most 8192 pipelines each)` | Once, at the first device creation |
| `PSO archive: N pipeline keys classify lookup misses (known/new), M failing shapes are excluded from capture` | Once, at start |
| `PSO archive: loaded shard <sha256 prefix> (session <time>-<pid>, X MB, N pipeline keys)` | Each shard used for lookup |
| `PSO archive: shard <prefix> (session …) not used: Metal refused it: …` | A verified shard Metal won't open; left out of the next generation |
| `PSO archive: dropped shard <prefix> (session …) of generation G: <reason>` | At start: `missing or unreadable`, `size or SHA-256 does not match`, `older than the newest sessions kept` |
| `PSO archive: generation G failed verification: <reason>; using the last good generation` (or `…; no earlier generation verifies, starting empty`), `PSO archive: no valid index (CURRENT); using generation G, the newest that verifies` | At start, when the index or its manifest is bad |
| `PSO archive: another process (pid P) writes <dir>: lookup only, this session captures nothing` | At start, when another process holds the store |
| `PSO archive: removed N files no generation since the previous one uses (or leftovers)` | At start (writer only) |
| `PSO archive: published generation G: N pipelines (+M) in shard <prefix>, X MB, S shards, E excluded shapes, in T ms` | Each successful flush: N pipelines in the new shard, M added since the last publish; `(merged this session's 8 shards into one)`, `(after excluding K failing shapes)`, `(session cap reached …)` when they apply |
| `PSO archive: publish of generation G failed: <reason>` | One try of a flush failed (Metal's error, or the step: `cannot write the manifest`, `cannot replace the index`, …) |
| `PSO archive: Metal names pipeline no. N of an archive never serialized before: <shape> (its key is excluded from capture from now on)` | That failure named a pipeline: its shape (as in #52's line); the next try rebuilds without it |
| `PSO archive: publish of generation G failed after T ms: <reason> (generation P stays)` | The flush gave up; `CURRENT` still names generation P |
| `PSO archive: published generation G: no shard, the N new pipelines are all excluded shapes; S shards, E excluded shapes, in T ms` | A flush whose new pipelines were all excluded, with an exclusion the store didn't record yet: the exclusion-only generation |
| `PSO archive: nothing to publish: the N new pipelines are all excluded shapes` | The same, when the store already records every one of those exclusions |

### CMDS

`CMDS cb= cbwhy=k:v,… close=k:v,… promote=off|min:N,k:v,…` (jdrly/dxmt #4). Why the game's render
thread commits chunks (command buffers) and closes encoder passes. Counted with plain increments in
the immediate context and handed over once per chunk; with `DXMT_STATS=0` nothing is counted. `cb`
and `cbwhy` come from one drain of the totals, so they always agree.

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
| `getdata` | `GetData` without `DONOTFLUSH` on a query that isn't ready, or an event query that stalls (after 64 polls `DONOTFLUSH` is ignored), or a timestamp query that stalls the same way in `DXMT_TIMESTAMP_MODE` `estimate`/`boundary` (see [TIMESTAMP](#timestamp)) |
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
- by the **watchdog**, even if the game never calls the context again: a thread waits
  `DXMT_PROMOTE_FLUSH_MS` after a hold and, if it is still pending, commits under the device lock
  every context entry point takes. Upstream's lock is a no-op unless the game turns on multithread
  protection (`ID3D11Multithread`), so `DXMT_PROMOTE_FLUSH_MIN` forces it on for the device's life:
  it then excludes the game's threads whatever the game asks, and `SetMultithreadProtected(FALSE)`
  neither disables it nor drops its owner. The cost of the switch: an uncontended lock and unlock per
  context call, and a game call that meets the watchdog's commit spins until it ends.

### ENCODE

`ENCODE draws= dispatches= tile= pso= useres= blitcmds= enc=k:v,… perchunk=avg/max loadMB=… storeMB=…
refuse=k:v,…` (jdrly/dxmt #4). What the encode thread encodes, per frame. The command counts are
plain increments decided at compile time; the encode thread hands its totals over once per chunk.
With `DXMT_STATS=0` nothing is counted.

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

### TIMESTAMP

`TIMESTAMP mode= ends= disjoint= kept= saved= clamped= tsspan=avg/max` (jdrly/dxmt #24): GW2's GPU
timestamp queries and what `DXMT_TIMESTAMP_MODE` did with them. Counts are per frame (encode-thread
frames of the window). No line in a window without timestamp queries.

DXMT samples no GPU counter for a timestamp: the value is a command buffer's GPU end time. In `exact`
mode (upstream) each `End` therefore closes the open encoder pass and asks for a commit at the next
pass change, so each query gets a buffer of its own (in busy Lion's Arch, CMDS showed
`cbwhy=timestamp:34 close=query:30`). `estimate` and `boundary` leave the pass open and mark the
query at its position in the chunk's encoder list (2 × the encoders closed before it, + 1 inside an
open pass). When the chunk's command buffer completes, the finish thread sets the query to the
buffer's GPU start + (end − start) × the query's share of the buffer's positions, as if each encoder
took the same GPU time. With the present split, a chunk has two buffers, and a query reads the one
its position falls in (at the split point: the first buffer's end). A value is never below one
handed out before it on that queue's finish thread (the rule `exact` already has), so the values are
monotonic. `estimate` asks for no commit: a frame's queries resolve when its last chunk has finished
(at Present, a Flush, or a GetData that flushes), about a frame later than `exact`'s.

Progress for a game that only polls: in `exact` the draw after a timestamp opens a new pass, which
commits the query's chunk. `estimate` and `boundary` keep the pass, so a timestamp query counts its
unresolved `GetData(DONOTFLUSH)` polls while its chunk is still the one being recorded, and after 64
of them (the stall rule event queries already have) such a poll flushes: the chunk is committed and
CMDS counts it as `getdata`. Polls of a query whose chunk is already committed never flush.

A timestamp-disjoint query (`Frequency` 1 GHz, `Disjoint` FALSE, as upstream) signals no event in
`estimate` and `boundary`. It becomes ready when the chunk it ended in has finished on the finish
thread, which resolves that chunk's timestamps first, so the game never sees a frame's disjoint data
before its timestamps. A Flush commits that chunk even without other work in it, and the event
queries' stall rule (64 unresolved polls) makes a polling `GetData` flush.

| Field | Meaning |
|---|---|
| `mode` | `DXMT_TIMESTAMP_MODE` in effect |
| `ends`, `disjoint` | Timestamp and timestamp-disjoint query `End`s (all modes) |
| `kept` | `End`s that found an encoder pass open and left it open (`estimate`, `boundary`; `exact` closes it and CMDS counts `close=query`). A disjoint `End` counts only where `exact` would signal an event (work since the last one) |
| `saved` | `estimate` only: pass changes at which `exact` would have committed for a timestamp and nothing was committed, i.e. command buffers saved. A pass change inside an explicit commit (Flush, Map, GetData, fence, `WaitUntilGPUIdle`) isn't counted, since that commit follows. An estimate: CMDS `cb` in `exact` against `estimate` is the measurement |
| `clamped` | Queries whose value was raised to an earlier one to stay monotonic (finish thread) |
| `tsspan` | Per frame, the last timestamp value minus the first (what the game sees from its first timestamp of the frame to its last), avg/max ms over the frames whose queries resolved in the window, in every mode; `unavailable` if none. Compare with `GPU gpuspan`: `exact` and `estimate` should be close |

Not covered: a deferred context's command list with timestamp queries still commits in `boundary`
and `exact` (as upstream); in `estimate` it no longer asks for a commit for them.

### MAP

`MAP fast= calls= ms= renames= created= grown= dropped= <kind>.<op>=calls/ms …` (jdrly/dxmt #25).
The game's `Map` and `Unmap` calls on the immediate context, per frame, by what they map and how.
Every call is counted. A call on a buffer (dynamic or default usage) is timed one in 16 and scaled
by 16, every other call (textures, staging, unmappable) always, because a timed call costs two clock
reads and in Wine each is a crossing into its unix side (`QueryPerformanceCounter`, about 52 ns
headless under the engine; #28). The 1 in 16 is pseudo-random, so
repeating sequences (Map/Unmap pairs, kinds in turn, a rename every n maps) are sampled evenly; it
reuses the draw `PACE`'s `map` made for the same call with another value, so the two never time the
same call and neither's clock reads land in the other's window. Counted with
plain increments in the immediate context and handed over once per chunk; with `DXMT_STATS=0`
nothing is counted. Deferred contexts' maps aren't in it.

| Field | Meaning |
|---|---|
| `fast` | `DXMT_MAP_FAST` (1 or 0) |
| `calls`, `ms` | All of `MAP`'s calls per frame, and their summed time (ms per frame, an estimate where sampled). The same calls as `PACE`'s `map`, timed after the device lock is taken and without `PACE`'s own timer, so a little lower |
| `renames` | Map(`WRITE_DISCARD`)s per frame that used up the buffer's page and took a new allocation (the others step to the next suballocation of the page) |
| `created` | Of those, the ones that found no free allocation and made a new Metal buffer, per frame (should be near 0 after the first seconds) |
| `grown`, `dropped` | Window totals: renames that doubled a buffer's page (`DXMT_MAP_FAST`), and renames that dropped free allocations instead of reusing them (smaller pages after growth, a default-usage buffer's first allocation) |
| `<kind>.<op>` | A bucket that had calls: calls per frame / ms per frame. Kinds: `cb`, `vb`, `ib`, `buf` (dynamic-usage buffers by bind flag: constant, else vertex, else index, else other), `dcb`, `dvb`, `dib`, `dbuf` (default-usage buffers by the same flags: their maps are `UpdateSubresource`'s, a whole-buffer update is a `discard`), `tex` (dynamic texture), `lintex` (dynamic linear texture), `staging` (staging buffer or texture), `other` (nothing `Map` maps). Ops: `discard` (`WRITE_DISCARD`), `nooverwrite`, `read`, `write`, `readwrite`, `invalid` (another map type), `unmap` |

`UpdateSubresource` of a buffer counts as one `Map` and one `Unmap`, as in `PACE`. Reading it: the
bucket with the most calls is the path to look at. `dcb.discard` + `dcb.unmap` would be bgfx's
uniform upload (`UpdateSubresource` of a default-usage constant buffer per draw); `cb.discard` the
game's own `Map(WRITE_DISCARD)` of a dynamic constant buffer; `staging.read` readbacks, whose GPU waits are in
`STATS`' `sync`. An A/B of `DXMT_MAP_FAST=1` and `=0` compares `MAP`'s `ms` and `PACE`'s `map` per
call; `renames` falls with growth, `created` should stay near 0 in both.

### REBIND

`REBIND skip= binds= same= samepct= skipped=` (jdrly/dxmt #30). The D3D11 draw path's pipeline
binds on the encode thread, per frame. A draw binds its pipeline whenever the pipeline state changed
on the render thread (or a new render encoder started); `same` are the binds whose pipeline the
render encoder already had bound. Each encoder tracks the pipeline its own command list bound last:
a new encoder starts with none, and any other pipeline bind encoded into it (a clear or copy pass's,
the tile barrier's and its restore) clears it. Merging two render passes later only puts the earlier
pass's commands in front of the later one's, which starts with its own bind, so the tracking stays
exact. Counted with plain increments on the encode thread and handed over once per chunk; with
`DXMT_STATS=0` nothing is counted (the skip still acts). Not logged in a window without binds.

| Field | Meaning |
|---|---|
| `skip` | `DXMT_SKIP_REBIND` (1 or 0) |
| `binds` | The draw path's pipeline binds per frame (ordinary, geometry and tessellation pipelines), skipped ones included |
| `same` | Of those, binds of the pipeline already bound in the encoder, per frame, counted with the skip on or off |
| `samepct` | `same` as a percentage of `binds` |
| `skipped` | The binds the skip left out, per frame (`same` with `skip=1`, 0 with `skip=0`) |

Reading it: `ENCODE`'s `pso` counts the binds that reach the command list (internal and compute ones
included), so with `skip=1` it is lower by `skipped` than with `skip=0` at the same spot. An A/B of
`DXMT_SKIP_REBIND=1` and `=0` compares `ENCODE pso` and the encode thread's AGX pipeline rows
(`setRenderPipelineState`) in Time Profiler; nothing should draw differently.

### Control file

`DXMT_CONTROL=<path>` (jdrly/dxmt #7) names a text file that changes settings while the game runs,
so one start can alternate settings minute by minute. The path is opened as `DXMT_LOG_PATH`'s is: a
unix path works. A thread of its own (`dxmt-control`, lowest priority) reads the file when the first
swapchain is created and then once a second; it publishes each key as an atomic that the owning code
reads where it already uses the setting (one relaxed load per Present). The render thread never
touches the file. The poll thread never calls the logger either (it would hold the logger's lock
while Wine writes, and the render thread's own lines would wait for it): it queues its `Control:`
lines, at most 32 between two Presents, and the render thread logs them at its next Present with a
`try_lock` that never waits; a line that misses it goes out at the Present after.

The file is the whole state: one `key=value` per line, spaces around either ignored; blank lines
and lines starting with `#` are skipped; the last valid line of a key wins. A key that is missing,
a value that isn't understood, or a file that can't be read leaves the setting at its environment
value. Replace the file atomically (write a temporary file, then `mv` it over the path): a reader
that catches a half-written file takes the missing key as its default for that second.

| Key | Values | Replaces |
|---|---|---|
| `present_deadline` | `refresh` (one refresh), `<ms>` (0 < ms ≤ 1000, decimals allowed), `0` (off: a plain `presentDrawable:`), `default` (the same as leaving the key out) | `DXMT_PRESENT_DEADLINE` |

For example, the deadline A/B of #7 with one start: `DXMT_CONTROL=/tmp/dxmt-control` set, then each
minute `print present_deadline=8 > /tmp/c.new && mv /tmp/c.new /tmp/dxmt-control` with `default`,
`8`, `12` and `0` in turn.

| Line | When |
|---|---|
| `Control: polling <path> once a second (DXMT_CONTROL); keys: present_deadline=refresh\|<ms>\|0\|default` | Once per process, when the first swapchain is created with `DXMT_CONTROL` set |
| `Control: reading <path>` | The first read that succeeds, and the first one after the file couldn't be read |
| `Control: <path> can't be read; every key at its default` | Warning: the first read that fails, and the first one after a read succeeded |
| `Control: unknown key "<key>" (line "<line>"); ignored` | Warning, once per key |
| `Control: present_deadline=<value> isn't refresh, default, 0 or a number of ms up to 1000; ignored (the line counts as missing)` | Warning, once per value; `Control: present_deadline without =value; ignored` once for a bare key |
| `Control: N lines dropped (more than 32 between two Presents)` | Warning, when the poll thread's queue was full |
| `Present deadline: <new> from frame N (was <old>; <source>)` | At the Present whose deadline setting differs from the previous Present's: `refresh`, `off` or `<ms> ms`. `frame` is the frame that Present presents (1-based, as `HITCH` counts), the first one with the new setting. `source` is `DXMT_CONTROL present_deadline`, or `DXMT_PRESENT_DEADLINE, the control file doesn't set it` when the setting returns to the variable's. The `Present pacing:` line then follows with the new `deadline`, and `STATS`' `deadline` has it from that window |

### RESIDENCY

`RESIDENCY resident= mb= adds= removes= commits= devmb=` (jdrly/dxmt #29). The device's residency
set (`DXMT_RESIDENCY_SET`), logged only while it is on. Counts are window totals (2 s), not per
frame. `mb` and the window totals are counted only with `DXMT_STATS` on; with it off, texture bytes
aren't estimated either.

| Field | Meaning |
|---|---|
| `resident` | DXMT allocations in the set now (a linear texture and its buffer count once) |
| `mb` | Their bytes (MB = 10⁶): buffers by length (a suballocated page whole), linear textures by their buffer, other textures estimated from the descriptor (all mips, layers and samples, uncompressed; BC by block) |
| `adds`, `removes` | Allocations that entered and left the set in the window |
| `commits` | Set commits in the window: at most one per command buffer commit, only when the set changed |
| `devmb` | `MTLDevice currentAllocatedSize`: all the Metal memory of the device, DXMT's own buffers and the drawables included |

Reading it: once warm, `adds` and `removes` stay near 0 (renames reuse pages) and `commits` with
them; a map change shows a burst of both. `resident` and `mb` should follow GW2's own working set
and come back down after a map change; growth over a long session would be a leak (an allocation
that never left). With the set on, `ENCODE useres` counts only the `useResource`s left (the dummy
constant buffer, the GS marshal, resources outside the set). An A/B of `DXMT_RESIDENCY_SET=1` and
`=0` compares `ENCODE useres`, STATS `encode` and the encode thread's resource-list rows
(`IOGPUResourceListAddResource`, `MTLResourceListAddResource`) in Time Profiler.

### RECORD

`RECORD shaders= variants= recipes= unsupported=variants:N,pipelines:N written= present= failed= queued= dropped=queue:N,seen:N,stopped:N [stopped=budget|writer]`
(#57), with `DXMT_PSO_RECORD` on: the 2 s window's recording. Not logged in a window with nothing to
report.

| Field | Meaning |
|---|---|
| `shaders` | Shaders new in this process whose DXBC was queued (once per SHA-1 and process) |
| `variants` | Shader variants whose translation inputs were queued: VS (global flags, GS pass-through, rasterization disabled, the input layout's slot mask and each element's index, slot, offset, format, step function and rate), PS (global flags, sample mask, UNORM output mask, dual-source blending, no depth), plain (global flags) |
| `recipes` | Plain graphics pipelines (a VS, an optional PS) whose recipe was queued, right before Metal builds them |
| `unsupported` | Variants (tessellation, stream-out, geometry) and pipelines (compute, geometry, tessellation, stream-out, and any whose VS or PS variant wasn't admitted) not recorded |
| `written`, `present`, `failed` | Records the writer stored in the window; records it skipped because the file already had them (an earlier session's); writes SQLite refused (they may be queued again if seen again) |
| `queued` | Records waiting for the writer now |
| `dropped` | `queue`: the queue was full (queued again if seen again); `seen`: the fingerprint table's probe run was full; `stopped`: recording had stopped |
| `stopped=budget`, `stopped=writer` | Recording stopped for this process: the file reached 256 MB (delete it to start over), or it couldn't be opened, or 8 writes in a row failed (a warning says which) |

Keys in `recipes_1.db` (table `cache_1`), all content addressed (a key always holds the same value, so a
key the file has is skipped): `D` + shader SHA-1 → DXBC; `V` + shader SHA-1 + variant digest + Metal
target (u32, `SM50_SHADER_METAL_VERSION`) → schema (u32), target (u32), the `AIRCONV_VERSION` it was
first recorded with (u32, provenance only: a warm stage translates the DXBC with its own airconv), kind
(u8: 1 VS, 2 PS, 3 plain) + inputs; `R` + SHA-1 of the recipe → the recipe. A recipe is, in this order,
little-endian: schema and Metal target (u32), VS shader SHA-1 and variant digest, a PS flag (u8) and the PS's SHA-1 and digest, then for each of the 8 color attachments
the pixel format, RGB and alpha blend operations, the four blend factors (u32 each), write mask and
blending (u8), then alpha-to-coverage, logic operation enabled (u8), logic operation (u32),
rasterization, sample count (u8), depth and stencil formats, immutable vertex and fragment buffer
masks, topology class, tessellation partition mode (u32), max tessellation factor (u8), winding and
factor step (u32), indirect command buffer support (u8). Never pointers or padding, so the same pipeline
gets the same key in every process. Since C3 the recipe is `compile::PipelineRecipe` (`src/dxmt/dxmt_compile_recipe.hpp`), built from the
exact descriptor; the recorder and the pipeline build share it, and its serializer
(`compile::serialize`) is the one above, byte for byte (`tests/gw2/pipeline_recipe_test.cpp` pins it
against #57's code and a golden key).

| Line | When |
|---|---|
| `Recipes: recording into $(getconf DARWIN_USER_CACHE_DIR)dxmt/<exe>/recipes_1.db (DXMT_PSO_RECORD; schema 1, airconv N, M of 256 MB)` | Once, when the writer starts (the first record); logged at the next Present |
| `Recipes: can't open …; recording stops (DXMT_PSO_RECORD)` | Warning: the database couldn't be opened |
| `Recipes: … holds N MB, its budget is 256 MB; recording stops. Delete the file to start over` / `… reached its budget …` | Warning: the file is full, at the start or during the session |
| `Recipes: 8 writes in a row into … failed; recording stops (DXMT_PSO_RECORD)` | Warning: SQLite refused the writes (disk full, I/O error); `[CacheWriter] Failed to insert` lines say why |

A recipe refers to `D` and `V` rows by key. A row that was dropped or failed leaves a recipe without
its inputs; a warm stage skips such a recipe.

### WARM

`WARM functions= pipelines= skipped= airmiss= failed= nofunctions= live=variants:N,pipelines:N joins=
[planned=recipes:N,functions:N,pipelines:N read=ms capped= othertarget= malformed=]` (W2/W3), with
`DXMT_WARM` on, every 2 s window.

| Field | Meaning |
|---|---|
| `functions`, `pipelines` | Functions and pipelines the warm jobs made in the window |
| `skipped` | Warm jobs whose key was already done, or being built by a live job |
| `airmiss` | Recorded variants without AIR in the shader cache: left to their first use (which translates them) |
| `failed` | Warm builds Metal refused: left to their first use |
| `nofunctions` | Warm pipelines whose functions couldn't be warmed: left to their first use |
| `live` | Live variant and pipeline jobs that found their result done in the shared tables (each one a build saved): when they were created (then they aren't submitted, and a demand needs no worker), or when they ran |
| `joins` | Live jobs that found a warm job building their key and waited for it instead of building it again |
| `planned` | Once, after the warm thread's read: recipes planned (first-use order, up to `DXMT_WARM_MAX`), functions and pipelines queued, the read's time, and records skipped (`capped`, another Metal target, malformed) |

The warm thread never calls the logger: this section is its report.

### LIFECHECK

`LIFECHECK stamps= objects= queues= early= total=` (jdrly/dxmt #31). Logged only with
`DXMT_LIFETIME_CHECK=1` in a diagnostic build (meson `-Dgw2_lifetime_check=true`); counts are window
totals (2 s). It shows the check is running; the
`LIFETIME` lines are its findings.

| Field | Meaning |
|---|---|
| `stamps` | Uses stamped in the window: each object a chunk's command buffers reference, once per command, attachment or binding that names it (an allocation once per chunk) |
| `objects` | Objects the check tracks now: Metal objects DXMT holds references to, objects a chunk used, and DXMT allocations a chunk held |
| `queues` | Command queues being checked |
| `early`, `total` | Releases of DXMT's last reference to an object before the GPU completed the last chunk that used it: in the window, and since the start. Both should stay 0 |

What a chunk uses and who holds it until the finish thread retires the chunk
(`src/dxmt/dxmt_gw2_lifetime.hpp`). The retirement ledger's holds (`src/dxmt/dxmt_gw2_retirement.hpp`) and the teardown waits act only with
`DXMT_UNRETAINED=1` (the default) or the check; with both off the retained command buffers hold those objects, as
upstream:

| Object | Stamped as | Held by |
|---|---|---|
| Buffers, textures, their views, UAV counters | the allocation (once per chunk), and each buffer and texture a command, attachment or argument names | the chunk's allocation references (`retainAllocation`) |
| What the D3D11 commands captured (resources, the presenter's frame state, scalers) | — | the chunk's command list, destroyed at retire |
| Argument, temporary and staging buffers | `buffer` | the ring allocators, freed by sequence after retire |
| Render and compute pipelines, depth-stencil states | `pipeline`, `depth-stencil state` | the D3D11 device's caches (never evicted) or the queue's encoding context (internal passes) |
| Sampler states (bound by resource ID in argument buffers) | `sampler state` | the D3D11 device's sampler cache; the dummy sampler: the encoding context |
| The drawable, its texture | `drawable`, `drawable texture` | the retirement ledger (the drawable; its texture with it) |
| The present pass's pipelines and gamma table | `pipeline`, `texture` | the presenter, held in the retirement ledger; its pipelines are rebuilt only after every earlier present's chunk retired |
| Events (`ID3D11Fence` Signal and Wait, keyed mutexes, the queue's and the initializer's) | `event` | the retirement ledger; the queue's own: the queue |
| MetalFX scalers and their fences | `MetalFX scaler`, `fence` | the retirement ledger |
| The fence pool, the occlusion and timestamp readbacks | `fence`, `buffer`, `counter sample buffer` | the encoding context; the chunk's readbacks |

Reading it: with `DXMT_UNRETAINED=1` an early release can draw wrong or fault the GPU; with `=0`
Metal still retains what it set directly, so the same line is a bookkeeping gap that phase 2 (the
Metal 4 queue) would hit. The check counts references per module (`d3d11.dll`, where DXMT encodes):
a reference taken in another module (`dxgi.dll`) isn't counted, and such an object is never reported.
A drawable's texture is held by Core Animation, not by DXMT, so it is stamped but never reported.

### Metal 4

`DXMT_METAL4` (jdrly/dxmt #32, the Metal 4 plan's phase 2; GW2 on Mac
`docs/research/dxmt-metal4-plan-2026-11-03.md`), in four pull requests (#48, #49, #50 and part 4).
On by default since 2026-11-04 (#32's follow-up): Jan played 10 minutes on it with no device
errors, no `missed` resources and no visual problems, and waived the separate A/B. `DXMT_METAL4=0`
restores Metal 3 exactly.

**Part 1, the bridge** (`src/winemetal`, unix side `src/winemetal/unix/mtl4.c`, bridge calls
155–178 of #32's 155–189; 179–189 are held for the later parts). DXMT keeps building today's
`wmtcmd_*` packets, and three replays run them on Metal 4 encoders:

| Today's packet | On a Metal 4 encoder |
|---|---|
| `Set{Vertex,Fragment,Object,Mesh}Buffer`, compute `SetBuffer` | `setAddress:` (the buffer's `gpuAddress` + offset) in that stage's argument table; the address is kept per index |
| `Set*BufferOffset` | `setAddress:` at the index's kept address + the new offset |
| `SetFragmentTexture`, compute `SetTexture` | `setTexture:` with the texture's `gpuResourceID` |
| `SetFragmentBytes`, compute `SetBytes` | a copy into the encoder's bytes buffer (256-byte aligned) and `setAddress:` |
| `UseResource` | nothing: residency sets make resources resident. Counted (`use_resource_dropped`): a resource the sets miss would read as zeros or fault. Pipeline states are allocations too and must be in a set on Metal 4 (Apple, WWDC25; d9mt adds every pipeline it builds): part 3 adds DXMT's |
| `WaitForFence`, `UpdateFence` | `waitForFence:beforeEncoderStages:`, `updateFence:afterEncoderStages:` (render stages as given, `Dispatch` for compute, `Blit` for copies) |
| render and compute `MemoryBarrier` | `barrierAfterEncoderStages:beforeEncoderStages:` with device visibility |
| indexed and indirect draws and dispatches | the buffers' GPU addresses (index buffer: address + offset, length − offset) |
| blit packets | the same copies, fills and mipmaps on an `MTL4ComputeCommandEncoder`, with a blit-to-blit barrier between consecutive ones (Metal 4 may overlap them; a Metal 3 blit encoder ran them in order). `ResolveCounters` is not supported (Metal 3 counter sample buffers) |

Everything else (pipelines, depth-stencil state, rasterizer state, viewports, scissors, blend color,
stencil reference, visibility mode, mesh draws of the GS and tessellation emulation, indirect
command buffers) maps one to one. The bridge also has the queue (`commit:count:` with a per-commit
`MTL4CommitOptions` whose feedback handler writes the GPU times and error and then signals a shared
event), allocators, residency on the queue and the command buffer, the layer's residency set,
`waitForDrawable`/`signalDrawable`, and a drawable present without a command buffer.

`tests/gw2/mtl4_bridge_test.m` runs the replays natively on a real Metal 4 queue (build and run
instructions in its header) and probes whether airconv's metallibs (a copy of DXMT's shader cache)
build and bind on Metal 4 encoders.

**Part 2, the queue and submission** (`src/dxmt/dxmt_gw2_mtl4_queue.*`, `CommandQueue`). On the
Metal 4 path a device's command queue gets an `Mtl4Queue`:

- one `MTL4CommandQueue`, with the residency set attached (Metal 4 needs it: no `useResource`);
- one `MTL4CommandAllocator` per chunk slot (the ring's size, `DXMT_CHUNKS`): a chunk's command
  buffers take their memory from their slot's allocator, which the finish thread resets when it
  retires the chunk, after every buffer of it completed;
- per commit, an `Mtl4Commit` record: the command buffer (recycled after completion) and the
  feedback its commit's handler writes (GPU start and end, error) before it raises the queue's shared
  event to the commit's value;
- the encode thread commits a chunk: a queue-level `waitForEvent` for the initializer's uploads
  (Metal 3 encodes it in the command buffer), `begin` on the slot's allocator, the encoders (part 3),
  the residency set's pending changes, `commit` with feedback. The present split (part 4) commits
  and begins again within the chunk;
- the finish thread waits on the event for each of the chunk's commits (no `waitUntilCompleted`),
  logs their errors as the Metal 3 path does, hands their GPU spans to the GPU line and PRESENT's
  `aftergpu`, destroys the chunk's readbacks (the timestamp readbacks read the commits' feedback
  instead of a command buffer's times), then resets the allocator;
- teardown waits for every commit before the chunks and the queue go.

`tests/gw2/mtl4_queue_test.mm` runs `Mtl4Queue` natively through the real PE thunks and the real
unix side (since B3, #76: the production table through `wmt_native_dispatch`, airconv stubbed): a
ring of 4 slots over 24 chunks, split commits, a queue-level event wait.

**Part 3, the encoders and argument tables** (`ArgumentEncodingContext::flushCommands`,
`src/dxmt/dxmt_gw2_mtl4_encoding.*`). `flushCommands` is one template for both paths: on Metal 3 it
encodes into a `WMT::CommandBuffer` as before (upstream's code, only re-indented where Metal 4
branches off), on Metal 4 into a `gw2::Mtl4Encoding` with the same encoder and event calls:

- Command buffers begin when an encoder needs one, on the chunk slot's allocator; a queue-level
  event (`SignalEvent`, `WaitForEvent`, the chunk's own completion event) commits the open buffer
  and signals or waits at the queue, which is where Metal 4 has them. The chunk's completion event
  is its last queue operation, so the finish thread waits for it as well as for every commit's
  feedback before it retires the chunk: a chunk without a command buffer, or with queue waits after
  its last one, is covered too (`Metal 4: chunk S's completion event not signalled after 1 s; still
  waiting` if it takes long).
- Render, compute and blit encoders replay today's packets (part 1) through one binding state: the
  queue's argument tables (render: vertex, fragment, object, mesh; compute: one; created with the
  queue, reused by every encoder), and a bytes arena from the argument-buffer ring for `Set*Bytes`
  (at least 64 KB free before each encoder). A replay that fills the arena stops at the `Set*Bytes`
  that didn't fit; the encoder gets a new arena (at least as large as that packet needs) and the
  replay resumes there, so no update is skipped (a `ClearView` with 1,025 rects, one 256-byte slot
  each, takes several arenas). The arenas stay in the ring until the chunk retires.
- **Residency** (the issue's prerequisite): Metal 4 makes nothing resident by itself, not even what
  an encoder names directly. Besides the device's set (#29), each chunk slot has a residency set,
  committed with every commit and used by its command buffers, emptied when the chunk retires. The
  replays put into it every allocation a packet names: pipelines, bound buffers and textures, index
  and indirect buffers, blit operands, the bytes arena, the attachments and visibility buffer of
  each pass, and every `UseResource`'s resource (a texture view's parent, a buffer-backed texture's
  buffer, a heap-placed resource's heap). So what the device set leaves out (heap-placed, IOSurface,
  CPU-placed, DXMT's bare buffers, the argument-buffer ring, internal pipelines) is resident for the
  chunk that uses it. `METAL4 missed` counts a `UseResource` with no set: it must stay 0.
- The timestamp readbacks read the chunk's commits' feedback (exact: the last buffer's end;
  estimate: each buffer's span from the position it began at).
- MetalFX stays on Metal 3: `DXMT_METALFX_SPATIAL_SWAPCHAIN=1` keeps the device there, and a device
  on Metal 4 doesn't offer the temporal scaler of DXMT's context extension (`CheckFeatureSupport`
  says no; `TemporalUpscale` logs once and does nothing). A MetalFX encoder that still reaches the
  Metal 4 path logs once and is skipped with its fences kept.
- Geometry and tessellation emulation and deferred command lists run on Metal 4 through the same
  replay (mesh draws and object/mesh buffers; a deferred list becomes the same packets), untested
  there, with a warning once per kind. Issue #32 kept them on Metal 3 "at first"; they can't be
  known when a device picks its queue, and the coordinator chose on 2026-11-04 to let them run
  rather than refuse them (GW2 uses none).
- Metal 4 needs `DXMT_UNRETAINED=1` (the default): its command buffers retain nothing, so the
  retirement ledger must hold what it uses.

**Part 4, present** (`Presenter::encodeCommandsMetal4`, the `Present` encoder in `flushCommands`):

- The present split commits the frame's open command buffer before `nextDrawable`, as on Metal 3
  (`DXMT_PRESENT_SPLIT`).
- The queue waits for the drawable (`waitForDrawable`: the command buffers committed after it), the
  present pass is encoded into a command buffer of its own (the drawable's texture stays out of the
  chunk's residency set: a driver-owned set would pin drawables and starve `nextDrawable`, d9mt;
  the layer's own residency set covers them, attached to the queue at a presenter's first present
  and removed when the presenter goes), committed, then `signalDrawable` and the drawable's own
  present: `presentAfterMinimumDuration:` (the cap), `presentAtTime:` now + the deadline
  (`DXMT_PRESENT_DEADLINE`, the control file), or `present`. PRESENT's presented handler, the
  pacing and the background throttle are unchanged (they live in the swapchain and on the
  drawable).
- With it the path is complete: `DXMT_METAL4=1` takes it on a device that has Metal 4.

`tests/gw2/mtl4_bridge_test.m` also checks that an `MTLFence` orders two separately committed
command buffers (the present split and queue-level events depend on it) and runs the present
sequence on a layer without a window.

### METAL4

`METAL4 commits= buffers= allocmb= slow= errors= resadds= missed= unsupported= bytesover= blitbar=` (jdrly/dxmt #32), only on the Metal 4 path, with
`DXMT_STATS`; window totals (2 s).

| Field | Meaning |
|---|---|
| `commits` | Metal 4 commits (command buffers) in the window |
| `buffers` | The most command buffers one chunk committed (1, or 2 with the present split) |
| `allocmb` | The largest command allocator's memory (MB) at its reset: how much command memory a chunk's slot holds |
| `slow` | Chunks the finish thread waited for over a second |
| `errors` | Commits whose feedback had an error (each also logged as a `Device error` line) |
| `resadds` | Allocations the replays added to the chunks' residency sets (repeats within one replay call left out) |
| `missed` | `UseResource` packets with no residency set to take them: a resource the GPU may read as zeros or fault on. Must stay 0 |
| `unsupported` | Packets the replays couldn't encode on Metal 4 (skipped). Must stay 0 |
| `bytesover` | `Set*Bytes` with no bytes arena to go to (skipped). Must stay 0 |
| `blitbar` | Barriers the blit replay put between consecutive copies |

### SUBSAMPLE

`SUBSAMPLE mode= candidates= accepted= upscaled=` (jdrly/dxmt #56), with `DXMT_SUBSAMPLE_METALFX` set
and `DXMT_STATS`; per frame over the window.

GW2's Render Sampling: Subsample draws the 3D scene into a target smaller than the backbuffer, then
one full-screen draw samples it into a target of the swapchain's size, and the UI is drawn on top of
that. The idea is World of Warcraft's render scale: only the 3D scene is below 4K, the UI stays
native. `DXMT_METALFX_SPATIAL_SWAPCHAIN` is not this: it upscales the whole swapchain, UI included.

At `Draw`, `DrawIndexed`, and `DrawInstanced`/`DrawIndexedInstanced` with one instance (bgfx, GW2's renderer, draws single instances through the instanced calls), before the pipeline is set up; never indirect draws:

- **Candidate**: at most 6 vertices or indices (per instance), triangle list or strip, no GS/HS/DS, a pixel shader,
  a render target in slot 0, and among the pixel shader's bound 2D SRVs one at least 1.25× smaller
  than that target on both axes.
- **Accepted** if, in this order (the first failure is the reason): exactly one render target
  (`rtvs`); its size is the swapchain's buffers' (`swapchain`); viewport 0 is the whole target
  (`viewport`); exactly one SRV is smaller like that (`srvs`); no SRV as large as the target, which
  would make it a composite with a native-resolution image (`fullsize`); the SRV is the same fraction
  on both axes (within 2%) and at most 4× smaller (`ratio`); blending and logic ops are off on render
  target 0 (`blend`); `DXMT_SUBSAMPLE_METALFX_PS` matches (`pin`); the shader's DXBC could be read
  (`ps_unparsed`) and has no `ld`/`ld_ms`, which fetch texels at integer coordinates and would read
  the wrong ones from a larger texture (`ps_load`); the SRV is mip 0 of a texture with one mip
  (`mips`); nothing is multisampled (`msaa`); the view has no component swizzle (`swizzle`); the
  texture can be sampled (`usage`); with `=1`, MetalFX takes the format (`metalfx`).
- **With `=1`**, an accepted draw closes the open render pass, encodes a MetalFX spatial upscale of the
  SRV's view into a private texture of the target's size and the view's format (one scaler and
  texture per shape, kept by the context; sRGB and UNORM formats in perceptual mode, which MetalFX
  requires for sRGB; float formats in HDR mode), points the encoder's binding of that pixel shader
  slot at it, and runs the draw in a new pass on the same target (load action Load). When the draw
  call returns, the slot is bound to the game's SRV again. The D3D11 state never changes. The game's
  pixel shader still runs: whatever it does besides sampling (tone mapping, grading) is kept; it now
  samples a texture at the target's resolution with the same normalized coordinates.
- On Metal 4 the scaler is an `MTL4FXSpatialScaler`, built with an `MTL4Compiler` and encoded into the
  chunk's open `MTL4CommandBuffer` between two compute encoders that carry the fences (as Metal 3's
  `BeginScaler`/`EndScaler`); its input and output go into the chunk's residency set.
  `tests/gw2/mtl4fx_test.m` runs the bridge natively on both paths.

| Field | Meaning |
|---|---|
| `mode` | `log` or `1` |
| `candidates` | Candidate draws per frame (accepted or not) |
| `accepted` | Of those, the ones every check passed |
| `upscaled` | MetalFX upscales encoded per frame (0 with `log`) |

| Line | When |
|---|---|
| `SUBSAMPLE: capture only (…)`, `SUBSAMPLE: on (…)` | Once, when the switch is read |
| `SUBSAMPLE: DXMT_SUBSAMPLE_METALFX=… is not 0, log or 1; off` | Once, a warning for any other value |
| `SUBSAMPLE: only a pass whose pixel shader's SHA-1 starts with … (DXMT_SUBSAMPLE_METALFX_PS)` | Once, when the pin is set |
| `SUBSAMPLE candidate: accept=yes\|no(<reason>) draw= n= rtv=WxH f<format> swapchain=WxH viewport=x,y WxH dsv=none\|WxH blend=on\|off ps=<sha1> sample= ld= resinfo= instr= src=t<slot> WxH f<format> ratio=XxY srvs=[t<slot> WxH f<format> m<mips>,…]` | Once per distinct shape (sizes, formats, draw kind and count, pixel shader, outcome), at most 64 per process. Formats are `MTLPixelFormat` numbers (80 BGRA8Unorm, 81 its sRGB, 70 RGBA8Unorm, 71 its sRGB, 92 RG11B10Float, 115 RGBA16Float). `sample`, `ld`, `resinfo`, `instr` are the pixel shader's DXBC counts (`instr` includes declarations) |
| `SUBSAMPLE: upscaling t<slot> WxH -> WxH with MetalFX for pixel shader <sha1>` | With `=1`, after the candidate line of each accepted shape |
| `SUBSAMPLE: MetalFX spatial scaler WxH -> WxH format F (Metal 3\|Metal 4, perceptual\|HDR)` | Once per shape, when its scaler is built |
| `SUBSAMPLE: MetalFX refused format F WxH -> WxH (…); not upscaled`, `SUBSAMPLE: MetalFX spatial scaling isn't supported on this device (…); not upscaled` | Once per shape (errors); that shape is then rejected as `metalfx` |
| `SUBSAMPLE: 64 candidate shapes logged; no more candidate lines` | Once |
| `RetirementLedger: refused a hold\|split hold\|retirement for chunk S (ring slot R, retired up to T, refusal N; only the first 8 are logged)` | An error, never expected (#66): a hold for a chunk that already retired or into a ring slot another unretired chunk still owns, or a retirement out of sequence. A refused hold is kept until the device's queue goes (never released early); a refused retirement releases nothing. The first 8 per command queue are logged |

## Native tests

`tests/gw2/run.zsh [pure|metal|all] [--meson <build dir>]` (X1) builds and runs every registered test
under `tests/gw2/`, on the Mac, with no Wine and no game. A test registers in its header:
`// gw2-test: pure` (no Metal device) or `metal`, one or more `// gw2-build:` lines (zsh, from the
checkout's root, writing `$OUT`) and optional `// gw2-run:` lines (default `"$OUT"`). A failed build or
a nonzero exit fails the run; exit 77 is *skipped*, never counted as passed; a test without the lines
is listed as unregistered. `--meson <build>/x64` adds the meson tests of the `gw2` suite (B1's
`bridge-native-smoke`). Native tests take their threads from `tests/gw2/shim/thread.hpp` (X2):
`dxmt::thread`, `mutex` and `condition_variable` are `std::`'s and priorities are no-ops, so a test
checks coordination, not Wine's scheduling.

`compile_service_test.cpp` (C1) holds the compile service's legacy parity fixture: scripted job graphs
(GW2-shaped bursts of pipelines sharing variants, parking, failures, the spurious-dependency rerun,
growth to 2 × CPUs) run against the service and against the scheduler it replaced
(`tests/gw2/fixtures/legacy_task_scheduler.hpp`, unchanged), with every worker's job held at a gate
and released one at a time; the traces (which jobs run, what they return, the worker count) must be
identical.

`retirement_ledger_test.cpp` (L1, #66) checks the retirement ledger (`src/dxmt/dxmt_gw2_retirement.hpp`)
with counting owners: nothing is released before its chunk retires, today's cross-category release
order, ring wrap at 32, 64 and 128 with a stale generation refused, two queues with the same
sequence, repeated and out-of-order retirements refused, no hold after retirement began (even from a
release), and teardown's slot-by-slot drain. Since L2 (#69) also the retention policy against every
hold call site's former gate in each reachable configuration (Metal 3 retained or unretained, the
lifetime check, the residency set, Metal 4), with the scripted release-trace parity run per
configuration.

The PSO archive (#75): `pso_archive_policy_test.c` (pure) checks keep, skip and classify under the
legacy and the exact-shape policies; `pso_archive_publish_test.c` (pure, a fake shard writer) checks
the generation publisher: round trips, a failure injected at each of its 7 steps leaving the last good
generation, corrupt shards, manifests and index, the writer lock, collection and session retention.
`pso_archive_test.m` (metal, arm64 and x86_64) drives sessions through the bridge's unix entry points:
`pso_archive_parity.zsh` runs each scripted session (Metal's real re-serialization failure, fake packer
failures, a truncated archive, the cap, retention, the exit report) through the instance with the
publisher off and through #55's file (`tests/gw2/fixtures/legacy_pso_archive.c`, unchanged), and
compares reports, counters, files and `.keys` byte for byte; `pso_archive_generations.zsh` checks the
publisher across processes (hits, exclusion, last good generation, lock, a `kill -9`, merging, and
exclusion-only generations: a fresh store's sole failing pipeline, a failing sole delta after a publish,
a failed exclusion-only publish retried).

`clock_bench.cpp` (#28) is not a native test: it is a Windows .exe that measures the clocks PE code
can read (ns per call, step size, backward steps), and `tests/gw2/clock_bench.zsh <engine> <scratch dir>`
runs it headless under an APFS clone of the engine in a throwaway prefix (winemac disabled). It chose
`gw2::nowNs`'s clock, `QueryPerformanceCounter`: 52 ns a read with 100 ns steps, against `steady_clock`'s
55 ns with 1 µs steps on the wall clock. Every clock finer than Wine's server tick is a crossing into
the unix side; `KUSER_SHARED_DATA`'s interrupt time is a 1.4 ns read but changes only when the
wineserver wakes (about every 18 ms in an idle process).
