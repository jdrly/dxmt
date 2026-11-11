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
| `DXMT_PRESENT_QUEUE` | `default` | (#157) An experiment on the present queue at a locked 60, parsed once per process. `two`: the layer's `maximumDrawableCount` is 2 instead of its default 3 (set through bridge call 70, `MetalLayer_setProps`, whose `WMTLayerProps` got a `maximum_drawable_count` field; 0 leaves the layer's count alone). `pace`: when the last frame's `nextDrawable` blocked, Present sleeps that long less 2 ms (at most one refresh less 2 ms) at its end, after the frame's commit and the latency wait, so the game starts its next frame later. Only at one frame per refresh with display sync (pacing mode `vsync`). It gives up for the swap chain's life at the first refresh skipped (`missed` or `starved`) within 500 ms of a sleep, and logs that it did. `pace` needs `DXMT_STATS=1`: it reads the presented records and the `nextDrawable` timer, and counts skipped refreshes per frame with the PRESENT section's own arithmetic (shared functions in `src/dxmt/dxmt_gw2_present_pace.hpp`), so the two can't disagree. With `DXMT_STATS=0` it does nothing and warns once. See [PRESENTQ](#presentq) | `default` (unset or anything else): the layer keeps three drawables, Present doesn't sleep, and nothing is counted per frame |
| `DXMT_STENCIL_PFV` | off | `=1` keeps `PixelFormatView` (no lossless compression) on typeless depth-stencil targets with a stencil plane | — |
| `DXMT_BG_FPS` | 30 | Present sleeps to this rate while another process's window is in the foreground | `=0`: no throttle |
| `DXMT_BG_FPS_MINIMIZED` | 10 | The same while GW2's window is minimized | `=0`: no throttle |
| `DXMT_RENDER_THREAD_PRIORITY` | off | On the first Present of each thread that presents (GW2's render thread): `=1` sets the macOS QoS class user-interactive (`pthread_set_qos_class_self_np` through the bridge), `=2` calls `SetThreadPriority(THREAD_PRIORITY_HIGHEST)`, `=3` both; see [Render thread priority](#render-thread-priority) | unset or `=0`: the thread keeps the priority and QoS it had |
| `DXMT_THREAD_PRIORITY` | `critical` | (#151) The priority DXMT's `dxmt-encode-thread` and `dxmt-finish-thread` set at their start. `critical`: `THREAD_PRIORITY_TIME_CRITICAL`, as upstream. `normal`: `THREAD_PRIORITY_NORMAL`, timeshare and below GW2's game thread. `utility`: `normal`, then Mach base priority 20 (bridge call `WMTSetThreadBasePrioritySelf`), which macOS keeps on the P cores. Read at each queue's creation; anything else is `critical`, with a warning. The startup line shows `utility:ok`, `utility:failed` (a thread didn't get base priority 20) or `utility:pending` (no thread has tried yet); see [Queue thread priority](#queue-thread-priority) | unset or `critical`: `THREAD_PRIORITY_TIME_CRITICAL` |
| `DXMT_COMPILE_PRIORITY` | `critical` | (#152) The priority each compile worker sets at its start (the workers that build shader variants and pipelines, 2 at first and up to 2 × CPUs). The same values and mechanism as `DXMT_THREAD_PRIORITY`: `critical` (`THREAD_PRIORITY_TIME_CRITICAL`, as upstream), `normal` (`THREAD_PRIORITY_NORMAL`, below GW2's game thread) or `utility` (`normal`, then base priority 20). Read at each device's creation; anything else is `critical`, with a warning. The startup line shows `utility:ok`, `utility:failed` or `utility:pending` as for `DXMT_THREAD_PRIORITY`; see [Compile worker priority](#compile-worker-priority) | unset or `critical`: `THREAD_PRIORITY_TIME_CRITICAL` |
| `DXMT_DEPTH_PFV` | off | `=1` keeps `PixelFormatView` (no lossless compression) on typeless single-plane depth targets (`R32_TYPELESS`, `R16_TYPELESS` bound as depth), the part `DXMT_STENCIL_PFV` doesn't cover (#2) | — |
| `DXMT_BG_POLICY` | `focus` | What "in the background" means for `DXMT_BG_FPS`. `focus`: another process's window is in the foreground, or none is (pid 0, counted as another process). `occlusion`: GW2's window isn't visible on screen (`NSWindow.occlusionState` without `Visible`, read through the bridge at most 10 times a second) or is miniaturized (`DXMT_BG_FPS_MINIMIZED`), whoever has the focus, as d9mt does. A minimized window (`IsIconic`) throttles under both | `focus` |
| `DXMT_PSO_BLEND_HASH` | on | The pipeline key's hash includes a fingerprint of the blend state (exactly what the key's equality compares: disabled fields left out, one render target without independent blend, else one per color attachment) and `RasterizationEnabled`; computed once per blend state object. The equality rejects keys whose fingerprints differ without reading both blend descriptions | `=0`: the upstream hash (no blend, no `RasterizationEnabled`) and equality |
| `DXMT_PSO_MRU` | on | Each context keeps its last 4 plain (VS/PS) pipelines with their keys and checks them, with the cache's own equality, before the device's pipeline cache (its lock, hash and bucket) | `=0`: every pipeline change looks up the cache |
| `DXMT_PSO_TOPOLOGY_KEEP` | on | `IASetPrimitiveTopology` keeps the current pipeline when the new topology has the same class (point, line, triangle) and the pipeline is a plain one (no GS, HS or DS bound); adjacency and patch lists always invalidate | `=0`: every topology change invalidates the pipeline |
| `DXMT_SKIP_SAME_STATE` | on | (#146) Setters that change nothing return before they invalidate the pipeline or emit a command: `IASetInputLayout` with the layout already bound (null included), `XXSetShader(NULL)` on a stage that has no shader (any stage, compute included; with class instances it still logs its error), a vertex buffer slot set again with the same buffer, stride and offset (no `bindVertexBufferOffset`), and `IASetIndexBuffer` with the same buffer, format and offset (no index buffer command). The state, its references and the encode thread's bindings are what they were, so later draws bind the same. `IASetInputLayout` with another layout stores it without a `QueryInterface`. The `PSO` line counts these calls either way (`samelayout`, `nullshader`, `samevb`, `sameib`) | `=0`: each of these calls invalidates or emits as upstream |
| `DXMT_SKIP_SAME_SRV` | off | (#147) `=1`: `XXSetShaderResources` leaves a slot whose bound view is the incoming one and whose hazard bit is clear before it validates the view: no view load, no hazard check, no `bind()`. A clear bit means the bound view can't be an output (render target, depth-stencil, UAV or stream output), so validation would pass it at once; a view that can be an output is always validated. That holds only because the switch also sets the bit when a bound slot gets a view that can be an output where it held one that can't: upstream's `BindingSet::bind` sets the bit only when an empty slot is bound, so `ResolveSRVHazard` missed that slot when the texture was then bound as an output, and the view stayed bound until the game set it again (when `ValidateSRVHazard` unbound it). With the switch on, the output's bind unbinds it at once, as D3D11 specifies. Off by default because that changes what is bound in that case; the `PSO` line's `srv` field counts the case (its fifth number) either way. The sampler setters already compare the pointer first and only gain counters | off: the setters and the hazard bits as upstream |
| `DXMT_CHUNKS` | unset (32) | The chunk ring's size, `32`, `64` or `128` (anything else: 32, with a warning). The render thread blocks in `CommitCurrentChunk` at size − 1 chunks in flight. Set (even to 32), the Metal queue's bound on uncompleted command buffers is the size + 16 (headroom for the present split's extra buffer, one per frame in flight); unset, it stays 32. An experiment (#8) | unset: upstream's ring of 32 and Metal bound of 32 |
| `DXMT_MAX_FRAME_LATENCY` | unset | `=1`…`16` replaces the frame latency the game sets with `SetMaximumFrameLatency` (and the default of 3 before it calls); out of range: ignored, with a warning. At 15–16 the 16 per-frame statistics slots (STATS, HUD) can mix frames | unset: the game's value, 0 taken as 3 |
| `DXMT_CAP_LIMITER` | on | With display sync, a frame cap (`d3d11.preferredMaxFrameRate`) below the refresh rate that isn't a whole number of refreshes (50 fps on 60 Hz) is kept by a frame limiter on the render thread: Present sleeps to a fixed cadence before it takes the device lock, and the present goes out with display sync and the deadline. Whole numbers (30, 20 fps on 60 Hz) keep the minimum duration, half a refresh short, as before | the cap is dropped (`vsync_duration` 0: uncapped to the refresh rate) |
| `DXMT_AIR_READERS` | 1 | The AIR cache's read connections (#95; 1–16). `1`: one reader behind one lock, as upstream. `n > 1`: a pool of n read-only sqlite connections (each a bridge `CacheReader`), each behind its own lock; a variant build takes the first free one (try-lock from a rotating start) and waits only when all are busy. The writer keeps its own connection; the database is in WAL mode, so readers never block it or each other. Fewer connections if one can't be opened; the startup line shows the number open. Measure with COMPILE's `variant=lock` (W0) before raising it | unset, `1` or anything outside 1–16: the single reader |
| `DXMT_COMPILE_DEMAND` | off | The compile service's `declared` policy (C2; see [COMPILE](#compile)). `=1` (exactly): a pipeline's shader variants are taken as its dependencies when it is submitted, and it waits for them unqueued instead of running and parking; a demand (the encode thread waiting in `GetPipeline`) moves the pipeline and its queued variants ahead of everything else, once each, and reaches what a demanded job parks on and dependencies submitted later; a speculative lane for warming (W2/W3) admits one job at a time and none while a demand waits, and a parked speculative job resumes through that admission again. Changes the order of compile jobs, not what is built. Parsed once per process | unset or anything but `1`: the `legacy` policy, the scheduler as before (C1) |
| `DXMT_WARM` | off | Shader warming W2/W3 (see [WARM](#warm)). `functions`: at device creation a lowest-priority thread reads the recipes #57 recorded (`recipes_1.db`, first-use order, bridge call 193) and queues a speculative job per recorded vertex and pixel variant that makes its MTLFunction from the AIR in the shader cache (never a translation). `all`: also a job per recipe that builds its pipeline from those functions through the pipeline builder (C3). Results go into shared tables keyed by stable key (C4) that the live variant and pipeline jobs consult when they are created (a done result completes them without a job) and when they run: each key is built once per process, whoever comes first; a live job that finds a warm job building its key waits for it (a demand promotes it). Speculative jobs run one at a time and never while a demand waits. Turns on the declared compile policy (as `DXMT_COMPILE_DEMAND=1`). Pipelines are built with the device's `newRenderPipelineState` on both the Metal 3 and Metal 4 queue paths. Parsed once per process | unset, `off` or anything else: no thread, no tables, nothing read |
| `DXMT_WARM_MAX` | 8192 | With `DXMT_WARM`: recipes planned at most (so at most this many pipelines and twice as many functions); the rest is counted as `capped`. #124 raised it from 2048: GW2 uses 2,700–4,800 pipelines a session, so 8192 is about two sessions (about 290 MB natively for today's 6,062 recipes) | — |
| `DXMT_COMPILE_WAIT_MS` | 2 | A `COMPILE frame=…` line for a frame whose encode-thread pipeline waits add up to at least this many ms (at most 5 lines a second); `0` turns them off. Needs `DXMT_STATS` | — |
| `DXMT_PSO_ARCHIVE` | off | `=1`: Metal binary archives for pipelines. Pipelines are looked up in the archives of the last 4 sessions, and every render and compute pipeline built is added to this session's archive (see [COMPILE](#compile)) | no archives: no lookup, no capture, no files |
| `DXMT_PSO_ARCHIVE_DIR` | user cache dir | With `DXMT_PSO_ARCHIVE=1`: an absolute unix directory that replaces `$(getconf DARWIN_USER_CACHE_DIR)dxmt/pso` (or `<DXMT_CACHE_DIR>/dxmt/pso`, #134) | — |
| `DXMT_PSO_FLUSH_EVERY` | 256 | With `DXMT_PSO_ARCHIVE=1`: serialize this session's archive every this many new pipelines (also every 30 s if any are new; never at exit) | — |
| `DXMT_PROMOTE_FLUSH_MIN` | 0 | `=<n>`: a speculative commit of an occlusion or timestamp query `End` waits until `n` such requests have accumulated, with a progress guarantee (see [CMDS](#cmds)). `1` commits as upstream does | `=0`: upstream (commit at the next pass change) |
| `DXMT_PROMOTE_FLUSH_MS` | 2 | With `DXMT_PROMOTE_FLUSH_MIN`: the longest a held commit waits (the timer at the next pass change, and the watchdog's period) | — |
| `DXMT_TIMESTAMP_MODE` | `estimate` | How a timestamp query `End` (and a timestamp-disjoint one) is taken. `estimate`: the open pass stays open and nothing is committed; the value is interpolated between the GPU start and end of the command buffer the query lands in, by its position among the chunk's encoders. `boundary`: the pass stays open, but the commit still happens at the next pass change (as `exact`, so `DXMT_PROMOTE_FLUSH_MIN` applies); the value is interpolated the same way. Both keep the values monotonic and make a disjoint query ready only after its chunk's timestamps are resolved; see [TIMESTAMP](#timestamp) | `exact` (or `0`): upstream, a pass split and a commit per timestamp, the value the buffer's GPU end |
| `DXMT_GETDATA_SKIP_FLUSH` | on | In `DXMT_TIMESTAMP_MODE` `estimate` and `boundary`: a `GetData` without `DONOTFLUSH` that finds a timestamp or timestamp-disjoint query unresolved flushes only while the query's chunk is still the one being recorded. Once that chunk is committed, the query resolves without help (the GPU, then the finish thread), so the poll goes on as a `DONOTFLUSH` poll: no `Flush` and no `STATS stall`, and it commits only what `DXMT_PROMOTE_FLUSH_MIN` held, as any `DONOTFLUSH` poll does. The first poll of a query in the current chunk still flushes, which commits it, so a poll loop always ends. CMDS `getskip` counts the skipped flushes, `getflush` the ones left (see [CMDS](#cmds)). No effect in `exact` | `=0`: every unresolved poll without `DONOTFLUSH` flushes, as before #158 |
| `DXMT_MAP_FAST` | on | The Map/Unmap fast path for buffers (see [MAP](#map)): the immediate context maps a dynamic or default-usage buffer from fields the buffer sets at creation (no virtual lookup, no reference count), `Unmap` of a buffer returns at once, the end of a page renames under one lock, a buffer that uses up its page within one chunk gets pages twice as large (up to 64 KB, at least two suballocations each), and a default-usage buffer's renames (`UpdateSubresource` of the whole buffer) come from CPU-visible suballocated pages, so an update is a `memcpy` instead of a new Metal buffer written through a unix call. `UpdateSubresource` of such a buffer skips its other virtual calls too. `PACE`'s `map` times one call in 64 and scales it (#150; `draw` one in 16) | `=0`: upstream's Map, Unmap and renames, `map` timed on every call |
| `DXMT_UPDATE_ONE_CALL` | on | (#145) With `DXMT_MAP_FAST`: `UpdateSubresource` of a buffer on the map fast path (a discard or no-overwrite update, see [MAP](#map)) is handled by the immediate context in one call under one device lock: it maps, renames or steps to the next suballocation, and copies, without calling its own `Map` and `Unmap` (two virtual calls, two `PACE` scopes, two `MAP` scopes and two more lock round trips) and without reference counts on the dynamic buffer. The same statements run in the same order. `MAP` and `PACE` still count one `Map` and one `Unmap` per update; one draw decides whether both time it. Read once per process; with `DXMT_MAP_FAST=0` it has nothing to do and the startup line shows `0` | `=0`: the update calls `Map` and `Unmap` as before |
| `DXMT_SKIP_REBIND` | on | A draw's pipeline bind is left out when its render encoder already has that pipeline bound (see [REBIND](#rebind)). Each encoder tracks the pipeline its own command list bound last; a new encoder starts with none, and any other pipeline bind in it (an internal pass's, the tile barrier's) clears it, so the first draw after those always binds. Only the bind is dropped: depth-stencil, rasterizer, blend color, viewports and scissors are separate commands, emitted as before | `=0`: every pipeline change binds, as upstream |
| `DXMT_CONTROL` | off | `=<path>`: the control file (see [Control file](#control-file)). A thread reads it once a second, never the render thread; its keys change settings while the game runs, for A/B minutes within one start. Today's key: `present_deadline`, which replaces `DXMT_PRESENT_DEADLINE` while the file sets it | unset: no thread, no file access, every setting from its variable |
| `DXMT_RESIDENCY_SET` | on | One `MTLResidencySet` for the Metal device, attached to every DXMT command queue on it (macOS 15+; see [RESIDENCY](#residency)). Buffers and textures enter it when they are created and leave it when they are freed, which happens only after the last chunk that used them has retired; the set's changes are committed right before a command buffer commit, only when it changed. `makeResident` emits no `useResource` for them (`ENCODE useres` near 0). Left out, with `useResource` as before: heap-placed and shared (IOSurface) resources, CPU-placed memory (i386), DXMT's bare Metal buffers (dummy constant buffer, GS marshal). Drawables never enter it. Texture views and buffer views ride on their parent | `=0`: no set, a `useResource` per resource and encoder (upstream) |
| `DXMT_UNRETAINED` | on | A chunk's command buffers (the frame's and the present split's) come from `commandBufferWithUnretainedReferences` (bridge call 154), so Metal retains nothing they use; DXMT holds it until the finish thread retires the chunk (see [LIFECHECK](#lifecheck)): the drawable, the presenter, events and MetalFX scalers are held in the retirement ledger until the chunk retires instead of being released after encoding, and device teardown waits for every committed chunk before the D3D11 device's pipeline, depth-stencil and sampler caches go (the same with `DXMT_LIFETIME_CHECK=1`). Metal 4 plan phase 1 (#31), groundwork for the Metal 4 queue (#32). On by default since the lifetime check (20+ min of busy Lion's Arch with a map change and alt-tabs) found 0 early releases | `=0`: `[queue commandBuffer]`, as upstream |
| `DXMT_LIFETIME_CHECK` | off | **Diagnostic build only** (meson `-Dgw2_lifetime_check=true`; in any other build it is compiled out and `=1` is ignored with a warning). `=1`: as d9mt's `D9MT_LIFETIME_CHECK`. Every object a chunk's command buffers use is stamped with the chunk's sequence on the encode thread; releasing DXMT's last reference to one before the GPU completed that chunk logs a `LIFETIME` line (once per object type) and counts in [LIFECHECK](#lifecheck). Works with `DXMT_UNRETAINED` on or off (it checks DXMT's own bookkeeping, not Metal's retains). It also turns on the ledger's holds and teardown waits that `DXMT_UNRETAINED=1` uses. Costs a lock and a hash lookup per stamp, retain and release: for test runs only | `=0`: no stamps, no counting; a normal build has no code for them at all |
| `DXMT_METAL4` | on | Metal 4 for this process's devices (Metal 4 plan phase 2, #32; see [Metal 4](#metal-4)): an `MTL4CommandQueue` with one `MTL4CommandAllocator` and one residency set per chunk slot, Metal 4 command buffers with commit feedback, today's packets replayed on Metal 4 encoders through argument tables, and present through `waitForDrawable`/`signalDrawable` and the drawable's own present (the same deadline, minimum-duration and present-split behaviour). Decided once per process at the first command queue. Stays on Metal 3, with a warning, on a device without Metal 4 (macOS before 26, or not `MTLGPUFamilyMetal4`), without the residency set (`DXMT_RESIDENCY_SET`), without `DXMT_UNRETAINED=1`, or with `DXMT_METALFX_SPATIAL_SWAPCHAIN=1`. **On by default since 2026-11-04** (#32's follow-up), after my 10-minute in-game session on it: no device errors, no missed resources, no visual problems | `=0`: Metal 3, exactly as before; no bridge call, no log line |
| `DXMT_MTL4_DEVICE_RESIDENCY` | off | (#153) Opt-in, until a session with it on has shown `METAL4 missed`, `unsupported` and `errors` at 0. On Metal 4, what the device's residency set holds stays out of each chunk's own set: d3d11's graphics and compute pipelines enter the device's set when they are built (and leave it when the device destroys them, after its last chunk retired), the argument-buffer ring's blocks when the ring makes them (and leave when it frees them), and the packets that name what the set holds say so (`WMT_CMD_RESIDENT`: pipeline binds, argument-buffer binds, indexed draws, which also carry the index buffer's address and length so the replay doesn't ask Metal; the bytes arena and a pass's attachments through the binding state). The chunk's set is committed, used and emptied only when it has allocations (bridge call `MTLResidencySet_allocationCount`, manifest line 201). It adds no residency set to the queue: the queue keeps only the device's set (with #184's layer sets on the present passes' command buffers, not the queue), however many swap chains a program makes. Still added per chunk: `UseResource`, blit operands, the visibility buffer, set textures, internal pipelines (clears, the present pass, GS/TS emulation) and whatever the device's set leaves out. `METAL4 resadds` counts them. A hole here (an allocation marked but in no set) is a GPU read of memory nobody keeps resident; see [Metal 4](#metal-4) for what Metal's validation does and doesn't catch. Read once per process | unset, or anything but `1`: every allocation a packet names goes into the chunk's set, and the set is committed, used and emptied with every command buffer, as before |
| `DXMT_MTL4_TIMESTAMP_REUSE` | off | (#154) Opt-in, until `tests/gw2/run.zsh metal` has passed on my Mac and a session with it on has shown `METAL4 buffers=2`, `errors` at 0 and no `STATS stall` growth. `=1` (exactly): on Metal 4, a chunk's timestamp queries read its last command buffer: after a present, the present pass's, so queries after the present read its times (exact: its GPU end; estimate: the end of its span). The chunk opens a command buffer for its queries only when it has none, or when a queue-level wait (`ID3D11Fence` or the context's barrier) came after its last one, so the queries read a time after the wait. Until #154 a chunk whose last buffer was already committed (the present's, in every GW2 frame, which always has queries pending) began an empty one for them, which the chunk's completion signal committed: three commits a frame. `METAL4 commits` and `buffers` show the difference. Read once per process | unset, or anything but `1`: that empty command buffer after the present, as before |
| `DXMT_SUBSAMPLE_METALFX` | off | GW2's Render Sampling: Subsample pass with MetalFX (#56; see [SUBSAMPLE](#subsample)). `log`: capture only, every candidate draw's shape is logged once and nothing changes. `1`: an accepted pass (the full-screen draw that samples the low-resolution 3D scene into a target of the swapchain's size) samples a MetalFX spatial upscale of its source instead, on Metal 3 (`MTLFXSpatialScaler`) and Metal 4 (`MTL4FXSpatialScaler`, bridge calls 190–192); the game's pixel shader still runs, and the UI drawn after it is untouched. Anything that doesn't match runs unchanged | unset or `=0`: no detection, no counting, no bridge call |
| `DXMT_SUBSAMPLE_METALFX_PS` | unset | With `DXMT_SUBSAMPLE_METALFX`: only a pass whose pixel shader's SHA-1 (as the candidate line prints it) starts with this hex prefix is accepted (`no(pin)` otherwise) | any pixel shader that passes the other checks |
| `DXMT_PSO_RECORD` | on | Shader warming W1 (#57; see [RECORD](#record)): records, durably, what each pipeline was built from, so later stages (W2/W3) can prepare shaders and pipelines before GW2 asks for them. Into `$(getconf DARWIN_USER_CACHE_DIR)dxmt/<exe>/recipes_1.db` (its own sqlite file, through the bridge's cache writer): each shader's DXBC by its SHA-1, each variant's translation inputs (VS, PS, plain) by shader SHA-1, variant digest and Metal target, and each plain graphics pipeline's recipe by the SHA-1 of an explicit serialization of the target, the vertex/pixel identities and every field of the descriptor Metal gets. Once per key and process. The callers never wait on the recorder: a fixed lock-free table of key fingerprints (2 MB) and a bounded lock-free queue (16,384 records or 128 MB), then counted drops. One lowest-priority writer thread skips rows the file already has and stops at a 256 MB file, or after 8 failed writes in a row; it never calls the logger (Present logs its lines). Changes nothing that is built or drawn | `=0`: no recording, no thread, no database opened |
| `DXMT_PSO_GENERATIONS` | off | `=1` (exactly), with `DXMT_PSO_ARCHIVE=1` (#75; see [COMPILE](#compile), *Generations*): the archives are published as generations in `…/os-<OS build>-<GPU>/gen1/` instead of one file per session: each flush writes the pipelines added since the last one into an immutable shard of their own (never re-serializing an archive, which Metal fails at), a checksummed manifest lists the shards and their keys, and one index file (`CURRENT`) is replaced atomically, so a kill or a failed write leaves the last good generation. A pipeline Metal's packer names is excluded by its key from capture, in this and later sessions. One process writes a store; another gets lookup only | the session files of #52, unchanged (they and a store sit side by side; neither reads the other) |
| `DXMT_RETIRE_RECOVERY` | on | A refused retirement (#113; never expected, a bug) recovers instead of holding every later chunk until teardown. The retirement ledger then retires by sequence with the completion the refused token proves: the finish thread waits for every chunk in order, so a token of the queue's submission for chunk `S` means every chunk up to `S` completed, and their holds go in sequence order (the ring resumes at `S + 1`); an empty or another queue's token proves nothing, and the next chunk's token recovers. A refused hold goes once the last chunk that may use it completed (its own chunk while unretired; for a retired sequence, every chunk the ring may still have in flight), not at teardown. A refused submission retirement retires the chunk the submission last waited for. One error line per failure (see *Other log lines*); the normal path is unchanged | `0`: refused holds and retirements are kept until the queue goes, as before |
| `DXMT_WARM_ORDER` | use | (#124) With `DXMT_WARM`: which recipes the plan takes. `use`: the use rows first (`DXMT_PSO_RECORD_USE`), then every recipe of the target; ranked by the newest session that used it, then by the number of sessions, then rowid; the first `DXMT_WARM_MAX` are planned and submitted in first-use order (the character select first), recipes without use rows (recorded before #124, or unused since) after them in rowid order. A file without use rows plans exactly as `rowid` does. Parsed once per process | `rowid`: #88's plan, the oldest recipes first, streamed |
| `DXMT_PSO_RECORD_USE` | on | (#124) With `DXMT_PSO_RECORD`: the recipes' use data, as d9mt's state cache keeps it. One row per recipe ever used, `U` + the recipe's SHA-1 → schema, lastSession, sessionsUsed, firstUseMs (u32 each), rewritten in place; and a session counter, `Ssession` → schema, the last session's number. When it opens the file, the writer reads both (bridge call 193), takes the counter + 1 as this session's number and writes it back. A newer session always has a larger number; no clock is involved. It then merges each recipe this session used (built when its recipe is recorded, or taken from warming's shared results through `warmPipelineHit`) into its row, once per recipe and session. Bounded by the number of recipes; about 3,000–5,000 merges a session on the writer thread (`RECORD uses=`). Warming's plan ranks by them | `=0`: no use data (`DXMT_WARM_ORDER=use` then plans by rowid, as for a file without it) |
| `DXMT_DEPENDENCY_COMPRESSION` | off | `=1` (exactly), Metal 4 only (#132, after GameSir `3170d0349a`; see [DEPS](#deps)): encoder fences follow a **checkpoint chain** instead of the per-encoder weak window. About every 64 encoder IDs one encoder (the checkpoint) waits on every encoder from the previous checkpoint's coverage to 64 IDs back, and on the previous checkpoint; every other encoder depends on the latest checkpoint (simplification usually removes that wait). So when an encoder runs, everything 127+ IDs back has finished, and the trackers keep direct dependencies up to 127 IDs back (legacy: 63). The trackers stay bounded, so the dropped dependencies need this cover (GameSir instead tracks every reader and writer by explicit ID). It also records a pass's fragment read of something the same pass wrote at pre-raster, which the legacy trackers forget. Read once per device; on Metal 3 the startup line says `unsupported` and the legacy policy stays. Encoder merging (the flush-time relation scan) is unchanged | the legacy weak window (each encoder waits on the encoders 64-127 IDs back that simplification can't reach) |
| `DXMT_CACHE_DIR` | unset | (#134) An absolute unix path that replaces `$(getconf DARWIN_USER_CACHE_DIR)` as the base of DXMT's caches, so each setup of the game on one Mac (the GW2 on Mac app's bottle, a development bottle) keeps its own: the AIR cache `<base>/dxmt/<exe>/shaders_*.db`, the recipes `<base>/dxmt/<exe>/recipes_1.db`, Metal's `<base>/dxmt/<exe>/com.apple.metal` (when Metal takes it), and the PSO archive's default `<base>/dxmt/pso/…`. The `dxmt/` level stays, so the layout under the base is unchanged. Read by the bridge's unix side on each cache open (`dxmt_cache_base`, `src/winemetal/unix/cache_dir.h`); an absolute `DXMT_SHADER_CACHE_PATH` or `DXMT_PSO_ARCHIVE_DIR` still wins for its cache. Empty or relative: ignored, with one warning (`Caches:` lines). A value longer than `PATH_MAX` makes the opens fail. The startup line shows the base in effect, `ignored` or `default`; the native PSO archive tool takes the same base | unset: `$(getconf DARWIN_USER_CACHE_DIR)`, byte for byte |
| `DXMT_PIN_MODULE` | on | (#137) A DLL of DXMT (`d3d11.dll` for GW2) pins itself for the process (`GetModuleHandleExW`, `GET_MODULE_HANDLE_EX_FLAG_PIN`) just before it starts its first thread (`dxmt::ThreadFn`). Its threads run its own code and some never end (the recipe writer, the PSO archive's flusher, the control poller), so a `FreeLibrary` that unmapped it crashed them (GW2 at exit: the recipe writer returned from its `Sleep` into freed code). Pinned, `FreeLibrary` only drops a reference: the module and its threads stay until the process exits, and its static destructors run at process exit instead. Read once per DLL | `FreeLibrary` can unload the module while its threads run (upstream) |
| `DXMT_VIEW_KEY_ARRAY_BIT` | on | (#155) For every texture a shader reads, the encode thread checks whether the bound view's type is an array type, as the shader wants it. A texture view's key carries the answer in one bit (taken from the view index, which keeps 27 bits: 134,217,728 views per texture, and creating one more ends the process with an error line), set from the view's type wherever a key is built, so a match returns without the texture's lock. A mismatch still reads the view's descriptor under the lock and binds the array or non-array view of it, as before. Read once per process | `=0`: every check takes the texture's lock and reads the view's descriptor (upstream); the key keeps the bit |
| `DXMT_LAYER_PROPS_POLL_MS` | 250 | (#156) How often Present asks the bridge for the layer's screen values (`WMTQueryDisplaySettingForLayer`: the display setting's version, colour space and HDR metadata, and the screen's two EDR values), in ms. Between asks the last answer stands; a resize (`ResizeBuffers`) asks again at the next Present. The call walks the layer's view, window and screen and builds the screen's device description, about 28 µs under Rosetta, under the device mutex. A display, colour space or EDR change reaches the present pass within one interval | `=0`: every Present asks, as before |
| `DXMT_PRESENTER_FREE` | off | (#185) `=1` (exactly): a presenter (one per swap chain) is deleted when its last reference goes, and its gamma texture and its two present pipelines go with it. That reference is either the swap chain's, after `~MTLD3D11SwapChain` waited for every chunk to retire, or the retirement ledger's, on the finish thread once the chunk that presented with it retired, so the GPU is done with them either way. Each deletion logs `Presenter: freed, N alive`. Off by default until it has run in my sessions. Steam's browser is where it shows: it makes 45 to 47 swap chains per process on the arm64 engine, GW2 one | off: every presenter stays until the process ends, as upstream (`Rc` doesn't delete, and `Presenter` had no `decRef` of its own) |

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
| `stall` | Event stalls in the window: `GetData` calls that found their query unresolved and flushed (CMDS `getflush` breaks them down by query) |
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
| | `dxmt` | The **presenting thread's** time inside DXMT's entry points within those same intervals (each interval is stored with its own time, so calls and durations can't fall into different windows), ms per frame, then per category `ms/calls` per frame: `present` (Present's own work, without the background throttle's and the frame limiter's sleeps and the latency wait), `presentwait` (Present's wait in `PresentBoundary` for frame N − `maxlatency`, the same wait as `STATS`' `latency`; `present` + `presentwait` is the whole Present, they don't overlap), `map` (Map and Unmap, **their waits included**; with `DXMT_MAP_FAST`, the default, it times one in 64 of its calls and scales it, #150), `flush`, `create` (`Create*` on this thread), `draw` and `dispatch` (each times one in 16 of its **own** calls and scales it: an estimate; all calls are counted. The sample is a per-thread pseudo-random draw, so it doesn't alias with call sequences that repeat; the per-thread state is found without `thread_local`, #28). Nested calls count once. Every interval an entry point's scope times (all but `present` and `presentwait`, which Present times itself over milliseconds) loses the clock's own cost, the startup line's `clockfloor` (#150), so an empty call reads 0 on average and a category can read a hair below 0. A Present is counted in the interval its own work and wait fall into, so a frame's interval holds the previous Present |
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
| `GW2 fork <commit>: DXMT_STATS=1 DXMT_HITCH_MS=… DXMT_PRESENT_SPLIT=… DXMT_DISPLAY_SYNC=… DXMT_PRESENT_DEADLINE=… DXMT_CAP_LIMITER=… DXMT_STENCIL_PFV=… DXMT_DEPTH_PFV=… DXMT_BG_FPS=… DXMT_BG_FPS_MINIMIZED=… DXMT_BG_POLICY=… DXMT_RENDER_THREAD_PRIORITY=… DXMT_PSO_BLEND_HASH=… DXMT_PSO_MRU=… DXMT_PSO_TOPOLOGY_KEEP=… DXMT_CHUNKS=… DXMT_MAX_FRAME_LATENCY=… DXMT_COMPILE_WAIT_MS=… DXMT_PSO_ARCHIVE=… DXMT_PSO_ARCHIVE_DIR=… DXMT_PSO_FLUSH_EVERY=… DXMT_PROMOTE_FLUSH_MIN=… DXMT_PROMOTE_FLUSH_MS=… DXMT_TIMESTAMP_MODE=… DXMT_MAP_FAST=… DXMT_SKIP_REBIND=… DXMT_CONTROL=… DXMT_RESIDENCY_SET=… DXMT_UNRETAINED=… DXMT_LIFETIME_CHECK=… DXMT_METAL4=… DXMT_SUBSAMPLE_METALFX=… DXMT_PSO_RECORD=… DXMT_AIR_READERS=… DXMT_CACHE_DIR=… DXMT_COMPILE_DEMAND=… DXMT_COMPILE_PRIORITY=… DXMT_GETDATA_SKIP_FLUSH=… DXMT_LAYER_PROPS_POLL_MS=… DXMT_MTL4_DEVICE_RESIDENCY=… DXMT_MTL4_TIMESTAMP_REUSE=… DXMT_PRESENTER_FREE=… DXMT_PRESENT_QUEUE=… DXMT_PSO_GENERATIONS=… DXMT_PSO_RECORD_USE=… DXMT_RETIRE_RECOVERY=… DXMT_SKIP_SAME_SRV=… DXMT_SKIP_SAME_STATE=… DXMT_THREAD_PRIORITY=… DXMT_UPDATE_ONE_CALL=… DXMT_VIEW_KEY_ARRAY_BIT=… DXMT_WARM=… DXMT_WARM_MAX=… DXMT_WARM_ORDER=… cpus=… refresh=… clockfloor=…` then `GW2 fork <commit> set: …` | Two lines (#43; one before), once per process, at the first Present: the build, the **effective** value of every switch above (defaults included; `DXMT_PRESENT_DEADLINE=refresh` is one refresh, `DXMT_CHUNKS` is the ring size in effect, `DXMT_MAX_FRAME_LATENCY=unset` leaves the game's value, `DXMT_PSO_ARCHIVE` is `1` when the archives are on, `0` when not asked for, `failed` when asked for but they couldn't start, `DXMT_PSO_ARCHIVE_DIR=default` is the user cache dir, `DXMT_PROMOTE_FLUSH_MIN`/`_MS` as the immediate context parsed them: `MS` is at least 1 and is shown even when `MIN=0` turns it off, `DXMT_TIMESTAMP_MODE` is `exact`, `estimate` or `boundary`, `DXMT_MAP_FAST` and `DXMT_SKIP_REBIND` are `1` or `0`, `DXMT_CONTROL` is the path or `off`, `DXMT_RESIDENCY_SET` is `1` when the set is on, `0` when switched off, `failed` when asked for but unavailable, `DXMT_UNRETAINED` is `1` or `0`, `DXMT_LIFETIME_CHECK` is `1`, `0` or `unbuilt` (asked for in a build without it), `DXMT_METAL4` is `1` (Metal 4 in use, the default), `0` (`DXMT_METAL4=0`), or `unsupported` (on, but the device, macOS or another setting keeps Metal 3; a warning says which), `DXMT_SUBSAMPLE_METALFX` is `0`, `log` or `1`, `DXMT_PSO_RECORD` is `1` or `0`, `DXMT_AIR_READERS` the AIR read connections open (`0` with the shader cache off or its reader not opened, `unopened` before any device), `DXMT_CACHE_DIR` the cache base in effect, `ignored` (empty or relative) or `default` (#134), `DXMT_COMPILE_DEMAND` is `1` or `0`, `DXMT_COMPILE_PRIORITY` and `DXMT_THREAD_PRIORITY` are `critical`, `normal`, `utility:ok`, `utility:failed` or `utility:pending`, `DXMT_GETDATA_SKIP_FLUSH` is `1` or `0`, `DXMT_LAYER_PROPS_POLL_MS` the interval in ms, `DXMT_MTL4_DEVICE_RESIDENCY` and `DXMT_MTL4_TIMESTAMP_REUSE` are `1` or `0`, `DXMT_PSO_GENERATIONS` is `1` (in use), `lookup` (another process writes the store), `unused` (asked for, but the archives are off or failed) or `0`, `DXMT_PRESENTER_FREE` is `1` or `0`, `DXMT_PRESENT_QUEUE` is `default`, `two`, `pace` or `unused` (`pace` with `DXMT_STATS=0`), `DXMT_RETIRE_RECOVERY` is `1` or `0`, `DXMT_SKIP_SAME_SRV` and `DXMT_SKIP_SAME_STATE` are `1` or `0`, `DXMT_UPDATE_ONE_CALL` is `1` or `0` (`0` with `DXMT_MAP_FAST=0`), `DXMT_VIEW_KEY_ARRAY_BIT` is `1` or `0`, `DXMT_WARM` is `off`, `functions` or `all`, `DXMT_WARM_MAX` the cap, `DXMT_PSO_RECORD_USE` is `1` or `0`, `DXMT_WARM_ORDER` is `use` or `rowid` (anything but `rowid` plans by use)), the CPUs the game sees, the refresh rate (0 if unknown), the clock's own cost in ns (#150: the median of 9 batches' mean of 256 back-to-back `QueryPerformanceCounter` reads, measured once; every `MAP` interval and every `PACE` interval but `present`'s and `presentwait`'s loses it); the second line lists every `DXMT_*`, `WINENCPU` and `WINEMSYNC` variable set (`set: none` if none). Since #60 the line is generated from the switch registry (`src/dxmt/dxmt_gw2_switches.hpp`) in this order, unchanged: a PR that adds a switch declares it with `sw::Switch` (its effective value through `sw::provideEffective` if it differs from the parsed one) and doesn't edit `LogStartup`; such switches follow `DXMT_PSO_RECORD`, sorted by name, before `cpus`, `refresh` and `clockfloor`. It adds the name here |
| `HITCH frame=… ms=… commit=…x… sync=…x… bgsleep=… dxmt(ms/calls): …` | A Present-to-Present interval of at least `DXMT_HITCH_MS`, logged at the top of the Present that ends it. `frame` is the frame that Present presents (1-based, as the latency fence counts); the interval holds the previous Present and the game's work on this frame. `commit` (waits for a free chunk) and `sync` (Map's waits for the GPU) are ms × count, from the same interval; they overlap the categories they happen in. `bgsleep` is whether the background throttle was on. The categories are as `PACE`'s `dxmt` (`presentwait` is the latency wait) |
| `SetMaximumFrameLatency(n)` | Every call; 0 is taken as 3 |
| `SetMaximumFrameLatency(n): DXMT_MAX_FRAME_LATENCY keeps m` | Every call while `DXMT_MAX_FRAME_LATENCY` is set: the game asked for `n`, `m` stays in effect |
| `CommandQueue: chunks=N (default\|DXMT_CHUNKS) metalbuffers=M maxlatency=L (…)` | Once per device: the ring's size, the Metal queue's bound on command buffers, and the frame latency it starts with (`DXMT_MAX_FRAME_LATENCY`, or 3 until the game sets it) |
| `DXMT_CHUNKS=… is not 32, 64 or 128; using 32`, `DXMT_MAX_FRAME_LATENCY=… is not 1-16; ignored` | Once, at device creation, for a value out of range (warnings) |
| `Presenter: display sync on/off` | When `DXMT_DISPLAY_SYNC` switches the layer's vsync |
| `Presenter: display setting version=V colorspace=C edr=E potential=P` | (#156) When an ask (`DXMT_LAYER_PROPS_POLL_MS`) returns a display setting version, colour space or EDR value other than the last ask's, and once at the first ask. `V` is the HDR setting's version (0: none set), `C` the `WMTColorSpace` number, `E` the screen's current EDR headroom (1 unless the layer wants EDR), `P` its potential headroom |
| `Presenter: maximumDrawableCount 2 (DXMT_PRESENT_QUEUE=two)` | (#157) Once per presenter (one per swap chain) with `DXMT_PRESENT_QUEUE=two` |
| `Present queue: pace gives up at frame F (missed +M, starved +S; last sleep X ms)` | (#157) Once per swap chain with `DXMT_PRESENT_QUEUE=pace`: a refresh was skipped within 500 ms of a pace sleep, and pace doesn't sleep again in that swap chain. `F` is the frame of the Present that noticed it (1-based, as `HITCH` counts; the skip itself was a present or two earlier), `M` and `S` the skipped refreshes since pace started watching, `X` its last sleep |
| `Present queue: DXMT_PRESENT_QUEUE=pace needs DXMT_STATS=1 (…); pace does nothing` | (#157) Once, a warning, with `pace` and `DXMT_STATS=0` |
| `Presenter: freed, N alive` | (#185) `DXMT_PRESENTER_FREE=1`: a presenter was deleted (its swap chain was released and its chunks retired); `N` of this process's presenters are still alive, one per swap chain not yet released |
| `Present: background throttle policy P (DXMT_BG_POLICY), N fps in the background (DXMT_BG_FPS), M fps minimized (DXMT_BG_FPS_MINIMIZED)` | Once, at the first Present (a warning before it if `DXMT_BG_POLICY` is neither `focus` nor `occlusion`) |
| `Present: <reason>, throttling to N fps (policy …, <details>)` | The background throttle engages, or its rate or reason changes. Reasons: `window minimized` (details `IsIconic`); `another process in the foreground` (focus: `foreground window H pid N, ours M`, or `no foreground window (pid 0), counted as another process; ours M`); `window not visible (occluded)` and `window miniaturized` (occlusion: `visibility 0x…: visible= miniaturized= activeSpace=`) |
| `Present: presenting normally again (was: …)` | It lets go |
| `PixelFormatView (no lossless compression): format F bind B WxH` | Once per format and bind flags of a render or depth target that keeps the flag |
| `RenderThreadPriority: thread T QoS OLD -> NEW (pthread_set_qos_class_self_np user-interactive: ok\|error E, left unchanged)` | `DXMT_RENDER_THREAD_PRIORITY` 1 or 3, once per presenting thread: its Win32 thread id, `qos_class_self()` before and after, and the call's result (an errno) |
| `RenderThreadPriority: thread T priority OLD -> NEW (SetThreadPriority THREAD_PRIORITY_HIGHEST: ok\|error E, left unchanged)` | `DXMT_RENDER_THREAD_PRIORITY` 2 or 3, once per presenting thread: `GetThreadPriority` before and after, and `GetLastError` on failure |
| `RenderThreadPriority: thread T: DXMT_RENDER_THREAD_PRIORITY=N isn't 1, 2 or 3, left unchanged` | Any other value |
| `ThreadPriority: NAME normal (SetThreadPriority THREAD_PRIORITY_NORMAL: ok\|error E)` | `DXMT_THREAD_PRIORITY=normal`, once per queue thread at its start (`NAME` is `dxmt-encode-thread` or `dxmt-finish-thread`), or `DXMT_COMPILE_PRIORITY=normal`, once per compile worker at its start (`NAME` is `dxmt-compile-worker`); `E` is `GetLastError` |
| `ThreadPriority: NAME utility, base priority OLD -> NEW (SetThreadPriority THREAD_PRIORITY_NORMAL: ok\|error E; thread_policy_set: ok\|error K)` | `DXMT_THREAD_PRIORITY=utility`, once per queue thread at its start, or `DXMT_COMPILE_PRIORITY=utility`, once per compile worker: the thread's Mach base priority after `THREAD_PRIORITY_NORMAL` and after the precedence call (`-1`: unreadable). `K` is the `kern_return_t`, or `-1` if the task's priority couldn't be read and nothing was set |
| `DXMT_THREAD_PRIORITY=… is not critical, normal or utility; using critical` | Warning, once per queue, for any other value; `DXMT_COMPILE_PRIORITY=…` the same, once per device |
| `DXMT_DEPTH_PFV=1: keeping PixelFormatView on typeless single-plane depth targets (first: format F bind B WxH)` | Once, the first time `DXMT_DEPTH_PFV` keeps the flag (the `PixelFormatView` line above then follows per format and bind flags) |
| `DEPTHVIEW base=… view=… swizzle=… type=… size=… usage=… pfv=… aspect=… nil=…` | Once per distinct view of a depth/stencil texture, and for any view that came back nil (a warning then); at most 64, then `DEPTHVIEW: more than 64 distinct views, the rest are suppressed`. Replaces the old stencil-only `TextureView` warning. Fields below |
| `DEPTHSRV format=… size=… pfv=… readonly=… stage=… slot=… view=… swizzled=… aspect=…` | Once per texture (with `DXMT_STATS` on) that is a render pass's depth-stencil attachment and is read by a shader of the same pass as a shader resource; at most 64, then `DEPTHSRV: more than 64 textures, the rest are suppressed`. Fields below |
| `PSO: blend hash on/off (DXMT_PSO_BLEND_HASH), MRU on, 4 entries/off (DXMT_PSO_MRU), topology keep on/off (DXMT_PSO_TOPOLOGY_KEEP), skip same state on/off (DXMT_SKIP_SAME_STATE), skip same srv on/off (DXMT_SKIP_SAME_SRV)` | Once per device: the effective pipeline lookup settings |
| `Present pacing: mode=… rate=… refresh=… cap=… interval=… duration=… ms limiter=… ms deadline=…` | At the first Present and whenever one of its values changes. `mode`: `vsync` (display sync alone), `vsync+after` (display sync and a minimum duration of `duration`), `vsync+limiter` (display sync and the frame limiter at `limiter`, `DXMT_CAP_LIMITER`), `vsync (cap dropped)` (`DXMT_CAP_LIMITER=0` and a cap that isn't a whole number of refreshes), `after` and `plain` (display sync off: DXMT's own minimum duration, or none). `rate`: the frame rate the pacing aims for (`uncapped` without a limit). `refresh`: the display's rate, `(assumed)` when DXMT didn't get one and uses 60. `cap`: `d3d11.preferredMaxFrameRate` (0 = none). `interval`: the game's sync interval. `deadline`: what a present without a minimum duration asks `presentDrawable:atTime:` for (`DXMT_PRESENT_DEADLINE`: one refresh by default, `=8` → `8.0 ms`, `=12` → `12.0 ms`, `=0` → `off`); `- (minimum duration)` when the minimum-duration branch ignores it |
| `Promote flush: a speculative commit (occlusion/timestamp query End) waits for N requests, at most M ms (a watchdog thread under the device lock, now always on) …` | Once per device, when `DXMT_PROMOTE_FLUSH_MIN` is on |
| `Timestamp queries: exact\|estimate\|boundary (DXMT_TIMESTAMP_MODE): …` | Once per process, when the first immediate context is created: the mode in effect and what it does (a warning before it for a value that isn't `exact`, `estimate`, `boundary` or `0`) |
| `Timestamp queries: the first query End in M mode kept its pass open[ and requested no commit]` | Once per process, the first time `estimate` or `boundary` handles a timestamp or timestamp-disjoint `End` |
| `Map fast path: on (DXMT_MAP_FAST): …` | Once per device, when `DXMT_MAP_FAST` is on (the default) |
| `UpdateSubresource one call: on (DXMT_UPDATE_ONE_CALL): …` | (#145) Once per device, when `DXMT_UPDATE_ONE_CALL` and `DXMT_MAP_FAST` are on (the default) |
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
| `Metal 4: drawables stay resident through their layer's residency set, used by each present's command buffer; the queue holds no layer's set` | Once per device, right after it (#184). A Metal 4 queue takes at most 32 residency sets; the queue holds only the device's (`Residency:`), so a program may make any number of swap chains. Until #184 each presenter added its layer's set to the queue for the rest of the process, and Steam's browser hit the limit at its 32nd swap chain |
| `Metal 4: a chunk with timestamp queries commits an empty command buffer for them after its last one (DXMT_MTL4_TIMESTAMP_REUSE=0, the default)` | Once per device, after it (#154). With `DXMT_MTL4_TIMESTAMP_REUSE=1` it is `Metal 4: timestamp queries read the chunk's last command buffer, the present's after a present (DXMT_MTL4_TIMESTAMP_REUSE=1)` |
| `Metal 4: newCommandBuffer failed after N command buffers; DXMT can't continue on Metal 4, aborting` | Error, on the Metal 4 path, when Metal returns no command buffer after the queue's first ones (one per slot and two spares, made with the queue): the process aborts, since earlier chunks may still be on the GPU |
| `Residency: the residency set is attached to this device's Metal 4 queue too (DXMT_METAL4); every allocation a packet names also goes into its chunk's set` | Once per device, on the Metal 4 path, after the usual `Residency:` line. With `DXMT_MTL4_DEVICE_RESIDENCY=1` (#153) it ends `; pipelines and argument buffers enter it, and what it holds stays out of each chunk's set (DXMT_MTL4_DEVICE_RESIDENCY=1)` instead |
| `Metal 4: chunk S's completion event not signalled after 1 s; still waiting` | Warning, on the Metal 4 path: the finish thread waits for a chunk's completion event (after its commits) over a second |
| `Metal 4: chunk S not complete after 1 s (its command buffer's value V, the queue event at E); still waiting` | Warning, on the Metal 4 path, the first 8 times the finish thread waits over a second for a chunk (a GPU hang or a very long frame); counted in [METAL4](#metal4)'s `slow`. Metal 3's `waitUntilCompleted` blocks the same way without a line |
| `RetirementLedger: refused a hold\|split hold\|retirement\|drain for chunk S (ring slot R, retired up to N, refusal K; only the first 8 are logged)`, `ChunkSubmission: refused a retirement for chunk S (…)` | An error, never expected (a bug), the first 8 per command queue. The retirement ledger (#66) refused a hold (its ring slot belongs to an unretired chunk, or chunk `S` already retired: the object is kept until teardown, never released early; with `DXMT_RETIRE_RECOVERY`, #113, until the last chunk that may use it completed), a retirement (#91: not the next sequence, or not the token of this queue's submission's wait), or teardown's drain (#91: not the submission's drained token; the holds then go with the queue, `S` is 0); or the chunk submission refused to take back a token it didn't issue for its last wait. A refused retirement releases nothing. With `DXMT_RETIRE_RECOVERY=0` the retirement lines are these; with it on (the default), the lines below |
| `RetirementLedger: refused a retirement for chunk S (ring slot R, retired up to N, refusal K; only the first 8 are logged); recovered: retired up to M by sequence (C chunks with holds released), H refused holds released, P still refused[; ChunkSubmission: refused it too, recovered: retired the chunk it last waited for] (DXMT_RETIRE_RECOVERY)` | An error, never expected (#113), with the default `DXMT_RETIRE_RECOVERY`: **one line per failure** (`gw2::retireDiagnostic`). The ledger refused chunk `S`'s retirement and recovered with the completion its token proves: the chunks `N + 1 .. M` retired by sequence (`C` of them had holds), `H` refused holds released because their last chunk completed, `P` refused holds left (their chunks still in flight). `M` = `N` when the token was at or behind the ledger (nothing to release). When the submission refused the same token (an empty one is refused by both), the line ends with the submission's part instead of a second line. Counted with the lines above (the first 8 per queue) |
| `RetirementLedger: refused a retirement for chunk S (…); not recovered: its token proves no completion (empty or another queue's); the next chunk's recovers[; ChunkSubmission: refused it too, …] (DXMT_RETIRE_RECOVERY)` | The same error, when the refused token was empty or another queue's: nothing is known complete, so the ledger releases nothing; the submission's part says whether it retired the chunk its last wait proved complete (Metal 4: the ring slot's allocator resets). The next chunk's retirement recovers the ledger (the line below) |
| `RetirementLedger: chunk S follows the refused retirement before it (retired up to N); recovered: retired up to M by sequence (…), H refused holds released, P still refused (DXMT_RETIRE_RECOVERY)` (or `not recovered: …`) | Info (#113): a refusal while an earlier one still left the ledger behind (the next token catching up after an empty one, each later chunk of a longer failure): part of the same failure, no new error line. A recovery that moves the ledger ends the failure; the next refusal is a new error. Counted with the lines above |
| `RetirementLedger: released H refused holds once chunk S completed (DXMT_RETIRE_RECOVERY; logged once per queue)` | Info (#113), once per command queue: refused holds (logged above as refused holds) were released at a normal retirement, once the last chunk that may use them completed |
| `ChunkSubmission: refused a retirement for chunk S (ring slot R, retired up to N, refusal K; only the first 8 are logged); recovered: retired the chunk it last waited for (DXMT_RETIRE_RECOVERY)` (or `not recovered: it didn't last wait for this chunk, or retired it already`) | An error, never expected (#113): the submission alone refused its token (the ledger took it), and recovered by retiring the chunk its last wait proved complete. With `DXMT_RETIRE_RECOVERY=0`, the ledger's and the submission's refusals are separate lines, as before |
| `Caches: under <dir>/dxmt/ (DXMT_CACHE_DIR, instead of $(getconf DARWIN_USER_CACHE_DIR)dxmt/)` | (#134) Once per process, when `dxgi.dll` loads (before its first cache path resolves), with an absolute `DXMT_CACHE_DIR`. `<dir>` without trailing slashes |
| `Caches: DXMT_CACHE_DIR="<value>" isn't an absolute unix path; ignored, the caches stay under $(getconf DARWIN_USER_CACHE_DIR)dxmt/` | (#134) Warning, once per process, when `dxgi.dll` loads, with an empty or relative `DXMT_CACHE_DIR` |
| `Lifetime: pinned for the process before its first thread; FreeLibrary no longer unloads it (DXMT_PIN_MODULE)` | (#137) Once per DLL, at its first thread |
| `Lifetime: not pinned (DXMT_PIN_MODULE=0); FreeLibrary can unmap code its threads still run` | (#137) Once per DLL, at its first thread, with the pin off |
| `Lifetime: can't pin the module (error E); its threads may outlive FreeLibrary (DXMT_PIN_MODULE)` | (#137) Warning, once per DLL, if `GetModuleHandleExW` fails (`E` is `GetLastError`) |

### Render thread priority

`DXMT_RENDER_THREAD_PRIORITY` (#9) is an experiment: in Lion's Arch GW2's render thread ran 22% of
its time on Performance cores while the main thread and DXMT's encode thread stayed on Super cores.
It acts on the first Present of each thread that presents, which in GW2 is the bgfx render thread,
and never on DXMT's own threads (those follow [`DXMT_THREAD_PRIORITY`](#queue-thread-priority)).

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

### Queue thread priority

`DXMT_THREAD_PRIORITY` (#151) is for watts. Upstream starts DXMT's encode and finish threads at
`THREAD_PRIORITY_TIME_CRITICAL`, which this engine's wineserver turns into timeshare off and Mach
importance 32. That puts both above every GW2 thread (the game thread is `HIGHEST`, base priority 35
in GW2's task, whose priority is 26), and they always get S cores. Neither is on the critical path:
at 60 FPS on Metal 4 the encode thread uses 18 to 25% of a core and blocks 13 ms a frame in
`nextDrawable`, and the finish thread is blocked 98% of the time.

- `critical`, the default: as upstream.
- `normal`: `THREAD_PRIORITY_NORMAL`, Mach importance 0 and timeshare on, so base priority 26 in
  GW2's task, below the game thread and its workers.
- `utility`: `normal` first, then bridge call `WMTSetThreadBasePrioritySelf(20)`. It sets the
  thread's `THREAD_PRECEDENCE_POLICY` importance to 20 less the task's priority (`proc_pidinfo`
  `pti_priority`), so the thread's base priority is 20, the utility band, which macOS keeps on the
  P cores. This is how the heat `ntdll.so` places threads for `HEAT_QOS=1=utility`, except that it
  reads the task's priority directly instead of from the thread's base priority less its
  importance, which a waiter's QoS override would inflate. `pthread_set_qos_class_self_np` is no
  use here: macOS refuses it once the wineserver has set Mach policies on a thread. The native test
  `thread_priority_test` checks that the call leaves base priority 20 from the normal, highest and
  time-critical states.

What to read in a session: the two `ThreadPriority:` lines (base priority `26 -> 20` for `utility`),
the startup line's `DXMT_THREAD_PRIORITY`, and, with the heat ntdll's `HEAT_COUNT=on`, the two
threads' base priority. `STATS encode=` may rise (the P cores run slower); `RING stalls`,
`STATS latency` and `commit` should stay 0, and `PACE` p50 and p99 shouldn't rise. A pipeline wait
blocks the encode thread whatever its priority, so hitches shouldn't change. Below 60 FPS the
queue runs empty, so a slower encode adds to `PRESENT presented`.

### Compile worker priority

`DXMT_COMPILE_PRIORITY` (#152) takes the compile workers off time-critical priority, with the
mechanism of [`DXMT_THREAD_PRIORITY`](#queue-thread-priority) and the same three values. Upstream
starts every compile worker at `THREAD_PRIORITY_TIME_CRITICAL`, and the pool grows to 2 × CPUs (16
workers in a session of 2026-11-08). On an AIR miss each runs airconv, about 2 ms of translated x86
code a variant, so a burst of new shaders puts up to 16 threads above the game thread on the S cores.
With `normal` they are timeshare at base priority 26 in GW2's task, below the game thread (35); with
`utility` at 20. Metal's own compile doesn't slow down off time-critical: natively, a thread without a
QoS class, as Wine's are, builds a new pipeline in 9 to 11 ms, as fast as at user-initiated (only
background is slower). airconv runs on the worker itself, though, and a worker on an E core
translates slower.

What a worker does while the encode thread waits for its job: it runs at its own priority, and the
wait doesn't raise it. The encode thread blocks in `GetPipeline` on the job's done flag (a
`std::atomic` wait), which names no thread, so neither Wine nor macOS can pass the waiter's priority
to the worker that will set it; a Mach priority push needs a lock whose owner the kernel knows. With
the declared policy (`DXMT_COMPILE_DEMAND=1`, or warming on) the demand moves the job and its
unfinished variants to the front of the queue, so the next free worker takes it, but a job already
running finishes at the worker's priority. With the legacy policy the demand changes nothing.

What to read in a session: one `ThreadPriority: dxmt-compile-worker` line per worker, the startup
line's `DXMT_COMPILE_PRIORITY`, and `COMPILE` `wait=` (count, total, max) and `threads=`. A longer wait
or more `slow=` frames than usual means the workers lost time on the slower cores while the encode
thread waited; the game thread's FPS in a busy spot while shaders compile shows what it gained.

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

`PSO builds= mru= lookups= equal= depth=avg/max new= topokept= lock=wait/held samelayout= nullshader= samevb= sameib= srv=calls/slots/same/set/missed smp=calls/slots/set` (jdrly/dxmt #6, #146, #147):
the pipeline lookup on the render thread. Counts are per frame (encode-thread frames of the
window) unless noted; all contexts and all three pipeline caches (plain, geometry,
tessellation) together. Omitted when the window had no builds, lookups, `topokept`, `same…`
counts, shader resource or sampler calls. Each context counts `builds`, `mru`, `topokept`, #146's
four `same…` fields and #147's `srv` and `smp` with plain increments and hands them over once per chunk, a deferred context at `FinishCommandList` (#149).

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
| `samelayout` | (#146) `IASetInputLayout` calls with the layout already bound (null on null included) |
| `nullshader` | (#146) `XXSetShader(NULL)` calls on a stage that has no shader, all stages |
| `samevb` | (#146) Vertex buffer slots set again with the same buffer, stride and offset (one per slot) |
| `sameib` | (#146) `IASetIndexBuffer` calls with the same buffer, format and offset (null on null included) |
| `srv` | (#147) `XXSetShaderResources`, all stages: calls, slots (the sum of their `NumViews`), slots whose bound view was the incoming one with a clear hazard bit, the ones the switch leaves (`same`; with the switch off, a slot in `missed`'s case set again counts too), slots that emitted a bind or an unbind (`set`), each per frame; then `missed`, a **window total**: bound slots given a view that can be an output while the slot's hazard bit was clear (the case `DXMT_SKIP_SAME_SRV` corrects) |
| `smp` | (#147) `XXSetSamplers`, all stages: calls, slots, slots that emitted a bind or an unbind, per frame |

With all three switches off the numbers measure the upstream lookup: `mru=0`, `topokept=0`,
`lookups` = `builds` (give or take the chunk in flight at the window's edge).

The four `same…` fields count with `DXMT_SKIP_SAME_STATE` on or off. With it on, the calls
`samelayout` and `nullshader` count no longer invalidate the pipeline, so `builds` and `REBIND same`
fall by the share of them that a draw followed; with it off, they say how many there are to save.

`srv` and `smp` count with `DXMT_SKIP_SAME_SRV` on or off. Slots a call is `slots` / `calls`. Wide
calls with `same` close to `slots` are what the switch saves; narrow calls that mostly `set` leave it
little to do. `missed` at 0 through play says the hazard-bit correction never acts in GW2.

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

### PRESENTQ

`PRESENTQ pace slept=N/P sleep=avg/max gaveup=0|1`

Only with `DXMT_PRESENT_QUEUE=pace` and `DXMT_STATS=1` (#157), every 2 s window.

| Field | Meaning |
|---|---|
| `slept` | Presents that slept, of the presents in the window |
| `sleep` | The sleeps' length, avg/max ms |
| `gaveup` | `1` once pace has given up in some swap chain (see the `Present queue: pace gives up` line) |

To compare the settings, read `PRESENT presented` and `missed`, `STATS2 nextdrawable` and `PACE app`'s
p99 in each. With `two` a pipeline wait that the three-deep queue hid shows as `missed`.

### COMPILE

Shader and pipeline compiles (jdrly/dxmt #5). GW2 creates shaders just before it draws with
them; DXMT compiles each shader variant and each pipeline on its compile workers, and the encode
thread waits in `GetPipeline` when the draw that needs a pipeline is encoded before the
pipeline is built. A frame with such waits is a hitch DXMT causes.

`COMPILE variants= air=hit/miss pso=hit/miss built= failed= build=shader:ms/max pipeline:ms/max
queue=now/max threads= wait=Nx ms/max slow= [waitcause=queued:N/ms,variantq:N/ms,variant:N/ms,building:N/ms[,other:N/ms]
[archbusy=N]] [variant=lock:N/ms/max,air:…,lib:…,fn:…,xlate:…,store:…] [promoted=N] [archive=hit/miss miss=new:N,known:N added= queued= unflushed= addfailed= capped= skipped= rebuilt= [gen= published= excluded= missexcl=]]`

Counts are **totals in the 2 s window**; the `COMPILE frame=` line below has them per frame. Times are ms. Without `DXMT_STATS` nothing is counted or timed (no clock reads), except the count of built pipelines that `DXMT_PSO_FLUSH_EVERY` needs.

| Field | Meaning |
|---|---|
| `variants` | New shader variants (a shader with a new input layout, blend or output setup): each one is a compile task |
| `air` | DXMT's AIR disk cache (`DXMT_SHADER_CACHE`): variants loaded from it / compiled by airconv (a cache entry Metal refuses counts as a miss) |
| `pso` | DXMT's in-memory pipeline caches, at draw time on the render thread: lookups that found a pipeline / new pipelines (each one is a compile task). Hits in a context's MRU (`DXMT_PSO_MRU`) never reach the caches and aren't counted here; the `PSO` section counts them as `mru` |
| `built`, `failed` | Pipelines Metal built (render, compute, geometry and tessellation emulation), and failed builds |
| `build` | Compile-worker time: `shader` = AIR cache lookup or airconv compile plus `newLibrary`/`newFunction`, total/max per variant; `pipeline` = `newRenderPipelineState`/`newComputePipelineState`, total/max per pipeline. Workers run in parallel, so totals can exceed the window |
| `queue` | Compile tasks submitted and not yet finished (variants and pipelines, those waiting for their shaders included): now / max in the window. A device's teardown takes the tasks it settles unrun out too (#114), so a later device starts from 0 |
| `threads` | Compile workers (the pool starts with 2 and grows while all of them are busy) |
| `wait` | The encode thread's waits in `GetPipeline` for a pipeline that wasn't ready: count, total ms, max ms. A ready pipeline costs no wait and isn't counted |
| `slow` | Frames whose waits reached `DXMT_COMPILE_WAIT_MS` |
| `variant` | (W0) Only in a window with variant builds: their stages, each count / total ms / max ms, elapsed on the compile workers (waits and compiler services included; not CPU time). `lock`: waiting for the AIR cache reader's lock (one reader, shared by all workers); `air`: the AIR read; `lib`: `newLibrary`; `fn`: `newFunction`; `xlate`: airconv's translation, on an AIR miss; `store`: writing the new AIR (its lock included). Warm jobs (#88) aren't counted here |
| `promoted` | (C2) Only with `DXMT_COMPILE_DEMAND=1`: compile jobs a demand moved ahead (a waited-for pipeline and its unfinished variants, each once). Under the declared policy `queued` waits also include a pipeline waiting for its variants, unqueued (its state stays `queued` until a worker starts it) |
| `waitcause` | (#46) Only in a window with waits: the same waits by what the waited-for pipeline task was doing when the wait started, count / total ms. `queued`: submitted, no worker had started it (behind other compile tasks). `variantq`: started, parked on its vertex or pixel shader variant, and that variant was still queued. `variant`: parked on a variant that was compiling (or between the two). `building`: inside Metal's pipeline build (`newRenderPipelineState`/`newComputePipelineState`). `other`: not classified (submitted while `DXMT_STATS` was off, or done but not yet flagged); shown only when non-zero. The counts add up to `wait`: both come from one drain of the per-cause counters, so a wait finishing during the drain lands in one window for both |
| `archbusy` | (#46) Only with `DXMT_PSO_ARCHIVE=1`: waits that began while PSO archive adds were still queued on the unix side (each add compiles the pipeline again for the archive) |
| `archive` | Only with `DXMT_PSO_ARCHIVE=1`: pipelines found in earlier sessions' archives / built without a hit in the window, pipelines added to this session's archive in the window, adds still queued, adds not yet serialized, adds that failed and pipelines not added because the session reached 8192 (both since start) |
| `miss` | (#52) With `DXMT_PSO_ARCHIVE=1`: the window's lookup misses, `new` (never in an earlier session's archive) and `known` (an earlier session's `.keys` has the pipeline's key, yet Metal missed it: keying, or a pipeline lost before a flush) |
| `skipped`, `rebuilt` | (#52) Since start: pipelines left out of the archive after Metal's packer named them in a failed flush, and flushes that succeeded only after rebuilding the archive without them |
| `gen`, `published`, `excluded`, `missexcl` | (#75) Only with `DXMT_PSO_GENERATIONS=1`: the generation loaded at start, then the last one published; pipelines in this session's shard of that publish; adds refused since start because their key is an excluded failing shape; the window's lookup misses of an excluded shape (not counted in `new` or `known`) |

**`COMPILE frame=N wait=Nx ms/max queue=Q variants= air=hit/miss pso=hit/miss built= failed=
build=shader:ms pipeline:ms created=N/min/max cause=queued:N,variantq:N,variant:N,building:N,other:N longest=<cause>
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

`created` (#160): the shaders whose first variant was made while the encode thread was on this
frame (counted like `variants`), and the shortest and longest time, ms, from each one's creation to
that first variant: how much earlier its compile could have started if it began at `CreateShader`
instead of at the first draw that needs it. A shader is created once per device and bytecode (a
later `CreateShader` of the same bytecode gets the same shader), after DXBC parsing. A variant
taken from the shared results (`DXMT_WARM`) counts too. `created=0` when there were none. The
other half of that question, what a Metal 4 specialization of a pipeline built without its color
state costs, is `tests/gw2/mtl4_specialize_test.m`'s benchmark.

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

- Files (with #52 also a `<file>.keys` sidecar per archive: the keys of the pipelines in it, written after each successful flush and removed with the archive): `$(getconf DARWIN_USER_CACHE_DIR)dxmt/pso/<exe>/os-<OS build>-<GPU>/` (#134: `DXMT_CACHE_DIR` replaces `$(getconf DARWIN_USER_CACHE_DIR)`)
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

`CMDS cb= cbwhy=k:v,… close=k:v,… getflush=k:v,… getskip=k:v,… promote=off|min:N,k:v,…` (jdrly/dxmt #4). Why the game's render
thread commits chunks (command buffers) and closes encoder passes. Counted with plain increments in
the immediate context and handed over once per chunk (`getflush` and `getskip`: a relaxed atomic add
per poll); with `DXMT_STATS=0` nothing is counted. `cb`
and `cbwhy` come from one drain of the totals, so they always agree.

| Field | Meaning |
|---|---|
| `cb` | Chunks committed per frame, the sum of `cbwhy` (should match STATS `cb`) |
| `cbwhy` | Commits per frame by reason; reasons that didn't occur are left out, `other` is always shown and should stay 0 |
| `close` | Encoder passes (render, compute, blit) closed per frame, by what closed them |
| `getflush` | `GetData` calls per frame that found their query unresolved and flushed (without `DONOTFLUSH`, or a stalled poll), by query: `event`, `occlusion`, `timestamp`, `disjoint` (#158). The same polls STATS `stall` counts as a window total; a flush commits only when the chunk has work, and those commits are `cbwhy` `getdata` |
| `getskip` | The same polls whose flush `DXMT_GETDATA_SKIP_FLUSH` skipped because the query's chunk was already committed (`timestamp`, `disjoint`) |
| `promote` | `off`, or with `DXMT_PROMOTE_FLUSH_MIN`: `min` and what happened at speculative commits, **window totals** |

`cbwhy` reasons. The issue's `query` is `occlusion` + `timestamp`. A commit counts once, under the
outermost reason (a staging `Map` that waits commits through `Flush()` as `map`).

| Key | The commit |
|---|---|
| `flush` | The game's `Flush`/`Flush1` (and D3D10's) |
| `getdata` | `GetData` without `DONOTFLUSH` on a query that isn't ready (except the polls `DXMT_GETDATA_SKIP_FLUSH` lets go: CMDS `getskip`), or an event query that stalls (after 64 polls `DONOTFLUSH` is ignored), or a timestamp query that stalls the same way in `DXMT_TIMESTAMP_MODE` `estimate`/`boundary` (see [TIMESTAMP](#timestamp)) |
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
CMDS counts it as `getdata`. `DONOTFLUSH` polls of a query whose chunk is already committed never
flush, and with `DXMT_GETDATA_SKIP_FLUSH` (#158) neither do polls without it: a game that polls without
`DONOTFLUSH` flushes once, at its first unresolved poll while the query's chunk is being recorded.

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
Every call is counted. A call on a buffer (dynamic or default usage) is timed when `PACE`'s `map`
times it, one in 64 with `DXMT_MAP_FAST` and scaled by 64, every other call (textures, staging,
unmappable) always, because a timed call costs two clock reads and in Wine each is a crossing into its
unix side (`QueryPerformanceCounter`, about 52 ns headless under the engine; #28). The 1 in 64 is
pseudo-random, so repeating sequences (Map/Unmap pairs, kinds in turn, a rename every n maps) are
sampled evenly. Since #150 the two lines time the same calls: when `PACE` times a call, `MAP` takes
that interval instead of reading the clock again, so it starts before the device lock is taken. Only a
call `PACE` doesn't time (a texture's or staging map the sampler skipped) is timed by `MAP` alone, from
after the lock. Every interval loses the clock's own cost (`clockfloor` on the startup line), so an
empty `Unmap` reads near 0 ms instead of one clock read, and a bucket can read a hair below 0. Counted
with plain increments in the immediate context and handed over once per chunk; with `DXMT_STATS=0`
nothing is counted. Deferred contexts' maps aren't in it.

| Field | Meaning |
|---|---|
| `fast` | `DXMT_MAP_FAST` (1 or 0) |
| `calls`, `ms` | All of `MAP`'s calls per frame, and their summed time (ms per frame, an estimate where sampled). The same calls as `PACE`'s `map`, and for buffers the same interval (#150), so `ms` and `PACE`'s `map` read the same time for them |
| `renames` | Map(`WRITE_DISCARD`)s per frame that used up the buffer's page and took a new allocation (the others step to the next suballocation of the page) |
| `created` | Of those, the ones that found no free allocation and made a new Metal buffer, per frame (should be near 0 after the first seconds) |
| `grown`, `dropped` | Window totals: renames that doubled a buffer's page (`DXMT_MAP_FAST`), and renames that dropped free allocations instead of reusing them (smaller pages after growth, a default-usage buffer's first allocation) |
| `<kind>.<op>` | A bucket that had calls: calls per frame / ms per frame. Kinds: `cb`, `vb`, `ib`, `buf` (dynamic-usage buffers by bind flag: constant, else vertex, else index, else other), `dcb`, `dvb`, `dib`, `dbuf` (default-usage buffers by the same flags: their maps are `UpdateSubresource`'s, a whole-buffer update is a `discard`), `tex` (dynamic texture), `lintex` (dynamic linear texture), `staging` (staging buffer or texture), `other` (nothing `Map` maps). Ops: `discard` (`WRITE_DISCARD`), `nooverwrite`, `read`, `write`, `readwrite`, `invalid` (another map type), `unmap` |

`UpdateSubresource` of a buffer counts as one `Map` and one `Unmap`, as in `PACE`. With
`DXMT_UPDATE_ONE_CALL` (#145, the default) an update on the fast path makes neither call, and the
immediate context counts the two itself. It times the update once, on `PACE`'s draw (one in 64): both
lines take that one interval, from after the device lock to before the copy (the copy was never in
either, and the lock is taken once for the update). `MAP` puts it on the `Map`'s bucket (`dcb.discard`, `dcb.nooverwrite`), so
`dcb.unmap` keeps its count and reads 0 ms for those updates. Its calls and the `discard` counts stay
the same with the switch on and off; with it off, `dcb.unmap`'s ms is again the empty `Unmap`'s scope and
lock, less the clock's floor. Reading it: the
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
| `resident` | DXMT allocations in the set now (a linear texture and its buffer count once); on Metal 4 with `DXMT_MTL4_DEVICE_RESIDENCY=1` (#153) also d3d11's pipelines and the argument-buffer ring's blocks |
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

`RECORD shaders= variants= recipes= unsupported=variants:N,pipelines:N written= present= failed= queued= dropped=queue:N,seen:N,stopped:N [stopped=budget|writer] uses=`
(#57), with `DXMT_PSO_RECORD` on: the 2 s window's recording. Not logged in a window with nothing to
report.

| Field | Meaning |
|---|---|
| `shaders` | Shaders new in this process whose DXBC was queued (once per SHA-1 and process) |
| `variants` | Shader variants whose translation inputs were queued: VS (global flags, GS pass-through, rasterization disabled, the input layout's slot mask and each element's index, slot, offset, format, step function and rate), PS (global flags, sample mask, UNORM output mask, dual-source blending, no depth), plain (global flags) |
| `recipes` | Plain graphics pipelines (a VS, an optional PS) whose recipe was queued, right before Metal builds them |
| `uses` | (#124) Use rows written (`DXMT_PSO_RECORD_USE`): recipes this session used for the first time, built or taken from warming's results, merged into their U row. A use the session already counted, or one while the use data couldn't be read, counts as `present` |
| `unsupported` | Variants (tessellation, stream-out, geometry) and pipelines (compute, geometry, tessellation, stream-out, and any whose VS or PS variant wasn't admitted) not recorded |
| `written`, `present`, `failed` | Records the writer stored in the window; records it skipped because the file already had them (an earlier session's); writes SQLite refused (they may be queued again if seen again) |
| `queued` | Records waiting for the writer now |
| `dropped` | `queue`: the queue was full (queued again if seen again); `seen`: the fingerprint table's probe run was full; `stopped`: recording had stopped |
| `seen` (collisions) | Keys are deduplicated by a 63-bit fingerprint, not the key itself (#103): two different keys with the same fingerprint count as one. At 30,000 keys a session that is about 5×10⁻¹¹; because fingerprints are deterministic, such a pair would collide in every session, so the second key's row is never recorded and its pipeline is built at first use, as without recording. Warming's tables (C4) use full digests |
| `stopped=budget`, `stopped=writer` | Recording stopped for this process: the file reached 256 MB (delete it to start over), or it couldn't be opened, or 8 writes in a row failed (a warning says which) |

Keys in `recipes_1.db` (table `cache_1`), all content addressed (a key always holds the same value, so a
key the file has is skipped): `D` + shader SHA-1 → DXBC; `V` + shader SHA-1 + variant digest + Metal
target (u32, `SM50_SHADER_METAL_VERSION`) → schema (u32), target (u32), the `AIRCONV_VERSION` it was
first recorded with (u32, provenance only: a warm stage translates the DXBC with its own airconv), kind
(u8: 1 VS, 2 PS, 3 plain) + inputs; `R` + SHA-1 of the recipe → the recipe; (#124) `U` + SHA-1 of the recipe → schema, lastSession, sessionsUsed, firstUseMs (u32 each; the one row the writer replaces) and `Ssession` → schema, the last session's number (`src/dxmt/dxmt_gw2_recipe_use.hpp`). A recipe is, in this order,
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
| `Recipes: recording into $(getconf DARWIN_USER_CACHE_DIR)dxmt/<exe>/recipes_1.db (DXMT_PSO_RECORD; schema 1, airconv N, M of 256 MB)` | Once, when the writer starts (the first record); logged at the next Present. With `DXMT_CACHE_DIR` (#134) the path starts with that directory instead (also in the `can't open` and budget lines) |
| `Recipes: can't open …; recording stops (DXMT_PSO_RECORD)` | Warning: the database couldn't be opened |
| `Recipes: session N (M recipes with use data; DXMT_PSO_RECORD_USE)` | (#124) Once, when the writer starts: this session's number and the recipes the file has use data for |
| `Recipes: can't read or start the use data (error E); no use rows this session (DXMT_PSO_RECORD_USE)` | (#124) Warning: the U or S rows couldn't be read (SQLite's code) or the counter not written; recording goes on without use data |
| `Recipes: … holds N MB, its budget is 256 MB; recording stops. Delete the file to start over` / `… reached its budget …` | Warning: the file is full, at the start or during the session |
| `Recipes: 8 writes in a row into … failed; recording stops (DXMT_PSO_RECORD)` | Warning: SQLite refused the writes (disk full, I/O error); `[CacheWriter] Failed to insert` lines say why |

A recipe refers to `D` and `V` rows by key. A row that was dropped or failed leaves a recipe without
its inputs; a warm stage skips such a recipe.

### WARM

`WARM functions= pipelines= skipped= airmiss= failed= nofunctions= live=variants:N,pipelines:N joins=
[planned=recipes:N,functions:N,pipelines:N read=ms capped= othertarget= malformed= readerror= busy= oversized=
buffer=KB order=use used=N/M newest= oldest= useerror=|order=rowid]` (W2/W3), with
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
| `planned` | Once, after the warm thread's read: recipes planned (by use, #124, or rowid order with `DXMT_WARM_ORDER=rowid`; up to `DXMT_WARM_MAX`), functions and pipelines queued, the read's time, and records skipped (`capped`: every recipe over `DXMT_WARM_MAX`, since the read goes on to the end of the file; before, only the rest of the 1 MiB batch that reached the cap was counted, so the 2026-11-04 UAT's `capped=575` was really 1,624 of 3,672; another Metal target; malformed) |
| `readerror` | With `planned` (#114): 0 when the recipes were read to the end; else SQLite's result code of the batch that failed, e.g. `5` (`SQLITE_BUSY`, still locked after the retries), and the read stopped there: the recipes before it are planned, the rest are left to their first use. `1` also when the bridge gave an answer the read couldn't step past. #117: a recipe file that exists but won't open or prepare is reported too, e.g. `26` (`SQLITE_NOTADB`: damaged or not a database), `11` (`SQLITE_CORRUPT`), `14` (`SQLITE_CANTOPEN`); `0` with an empty plan means no recipe file or no recipe table yet (a new or empty file) |
| `busy` | With `planned` (#114): `SQLITE_BUSY` retries bridge call 193 made (2 ms apart, at most 50 per batch) before it read on or gave up |
| `oversized` | With `planned` (#114): rows larger than 16 MiB, skipped (a recipe is about 420 B); the rows after them are still read |
| `buffer` | With `planned` (#114): the read buffer's final size: 1024 KB, or larger when a row didn't fit in 1 MiB (the buffer grows to it, up to 16 MiB) |
| `order` | With `planned` (#124): `use` or `rowid` (`DXMT_WARM_ORDER`) |
| `used` | With `order=use`: planned recipes that have use rows / every recipe of the target that has them |
| `newest` | With `order=use`: the newest session number in the use rows (the file's counter) |
| `oldest` | With `order=use`: how many sessions before `newest` the planned recipe used longest ago was last used (0: all in the newest session); `-` when no planned recipe has use data |
| `useerror` | With `order=use`: the use rows' read, as `readerror`; the plan goes on with the rows read before it |

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
On by default since 2026-11-04 (#32's follow-up): I played 10 minutes on it with no device
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
  chunk that uses it. `METAL4 missed` counts a `UseResource` with no set: it must stay 0. Since
  #153, `DXMT_MTL4_DEVICE_RESIDENCY=1` keeps what the device's set holds out of the chunk's set and
  commits, uses and empties a chunk's set only when it has allocations (opt-in).
- **What validation catches** (#153, my M5 Max, macOS 27, `tests/gw2/mtl4_bridge_test.m`): with an
  allocation marked resident but left out of every set, `MTL_DEBUG_LAYER=1` alone reports nothing.
  A buffer left out (the argument buffer) still drew right under it; `MTL_SHADER_VALIDATION=1`
  caught it (the draw read zeros). A pipeline left out ran right under both, unreported. So a
  residency hole has to be ruled out by how allocations enter and leave the sets, not by the layers.
- The timestamp readbacks read the chunk's commits' feedback (exact: the last buffer's end;
  estimate: each buffer's span from the position it began at). A chunk with queries and no command
  buffer begins one for them, and so does a chunk whose last buffer is already committed (the
  present's, in a GW2 frame): an empty one after it. With `DXMT_MTL4_TIMESTAMP_REUSE=1` (#154) the
  second doesn't, unless a queue-level wait came after its last buffer, so queries after the present
  read the present's.
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
  the layer's own residency set covers them, used by the present pass's command buffer, #184),
  committed, then `signalDrawable` and the drawable's own
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
| `buffers` | The most command buffers one chunk committed (1, or 2 with the present split, more when a queue-level event falls between encoders). A frame with timestamp queries adds an empty one after the present: 3 in GW2. With `DXMT_MTL4_TIMESTAMP_REUSE=1` (#154) it doesn't: 2 |
| `allocmb` | The largest command allocator's memory (MB) at its reset: how much command memory a chunk's slot holds |
| `slow` | Chunks the finish thread waited for over a second |
| `errors` | Commits whose feedback had an error (each also logged as a `Device error` line) |
| `resadds` | Allocations the replays added to the chunks' residency sets (repeats within one replay call left out). About 250,000 a window in GW2 by default. With `DXMT_MTL4_DEVICE_RESIDENCY=1` (#153) only what the device's set doesn't hold: the present pass's pipeline, backbuffer and gamma texture, `UseResource`, blit operands, the visibility buffer, set textures and internal pipelines; I expect a few hundred a window, not 0 (not measured) |
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
| `RetirementLedger: refused a hold\|split hold\|retirement for chunk S (ring slot R, retired up to T, refusal N; only the first 8 are logged)` | An error, never expected (#66): a hold for a chunk that already retired or into a ring slot another unretired chunk still owns, or a retirement out of sequence. A refused hold is kept until the device's queue goes (never released early; with `DXMT_RETIRE_RECOVERY`, #113, until the last chunk that may use it completed); a refused retirement releases nothing (with `DXMT_RETIRE_RECOVERY`, it recovers: see *Other log lines*). The first 8 per command queue are logged |

### DEPS

`DEPS policy=legacy|compressed stages/f= strong/f= full/f= simplified/f= checkpoints/f= gaps= simplify_us/f=
relation_checks/f= relation_us/f= encoders/f= waits/f= updates/f= checkpoint_waits/f=` (#132), after STATS with
`DXMT_STATS` (counting is decided when the device is made), per frame of the 2 s window. Both policies
report every field, so the two can be compared in the same place:

- At `endPass` (API thread), per encoder stage (one per compute/blit/clear/present encoder, two per render
  pass: pre-raster and fragment): `stages`; `strong`, the dependencies the resource trackers reported;
  `full`, after the weak window (legacy) or the checkpoint (compressed) were added; `simplified`, the waits
  left after simplification, before encoder fusion; `checkpoints`, compressed only, checkpoint stages
  (about one per 64 IDs); `gaps` (a count), an encoder ID that didn't follow the previous one (never
  expected; it only widens the next checkpoint's window); `simplify_us`, time in
  `collectAndSimplifyWaits`.
- At `flushCommands` (encode thread), per chunk after the relation scan fused encoders:
  `relation_checks`, `checkEncoderRelation` calls; `relation_us`, time in the relation scan;
  `encoders`, encoders left to encode; `waits` and `updates`, the `waitForFence`/`updateFence` they emit
  (a render pass once per fence of its pre-raster and fragment sets together); `checkpoint_waits`, of
  `waits`, those of encoders holding a checkpoint stage (the checkpoints' cost).

Log lines: `Dependency compression: on (DXMT_DEPENDENCY_COMPRESSION=1): …` once when it acts, or
`Dependency compression: requested but the queue is on Metal 3; …` once.

Tests: `tests/gw2/dependency_compression_test.cpp` drives the production trackers and
`FenceLocalityCheck` under both policies against a hazard oracle that models Metal fences (a wait on a
slot waits for every earlier update of that slot, as Metal 4 documents) and checks every RAW/WAR/WAW pair;
a control without window or checkpoint must fail it. A fusion case (the production
`fuseRenderPassFences`) checks that waits on a fused pass's own updates aren't counted as emitted.

## CI

`.github/workflows/gw2.yml` (#162) checks every pull request into `gw2-on-mac` and every push to it on
GitHub's runners, so nothing has to build on my Mac. The macOS jobs run on the `xcode-27` image (macOS 27)
with Xcode 27.2, the one I build with. The toolchains are cached under their versions,
so only the first run after a version change builds LLVM (about 25 minutes per architecture).

- **Build x86_64, Build i386**: the build `scripts/build-dxmt.zsh` (GW2 on Mac) makes. Release,
  stripped, NVEXT off, Homebrew's mingw-w64 (GCC 16), meson 1.12.1, LLVM 15.0.7 built for macOS 26.0 as
  airconv needs it. Wine's import libraries, `winebuild`, `winemac.so` and `ntdll.so` come from 3Shain's
  Wine 8.16, as in upstream's CI. My CrossOver engine isn't downloadable. The job checks that
  `winemetal.so` still loads both through `@rpath`, so it finds the engine's copies. A compiler warning
  fails the job unless `.github/gw2/warnings-<arch>.txt` lists it. The list keys a warning on its file
  and message without the line, so moved code keeps its warnings, and a second warning of the same kind
  in the same file gets through. The job prints the build's whole list. When a change removes a warning,
  or has to keep a new one, that list replaces the file. x86_64 also runs the meson `gw2` suite without
  `gw2-metal`.
- **Build arm64 (Mach9)** (#182): the build for GW2 on Mac 0.2's engine, Mach9 0.3.1, made the way
  Mach9's `bin/build-dxmt.sh` makes its own DXMT. One meson tree with `build-arm64ec.txt` builds the
  ARM64X DLLs and the arm64 `winemetal.so`, a second with `build-win32.txt` the i386 DLLs, both with
  llvm-mingw 20261104 (Mach9's pin). They link against the engine's own `winemac.so`, `ntdll.so` and
  import libraries, taken from its release tarball (pinned by SHA-256). `winebuild` comes from 3Shain's
  Wine 11.2, since nothing of Mach9's engine is ever run. Then `.github/gw2/check-mach9.zsh` compares the
  result with the engine's DXMT, without Wine: every DLL's machine (ARM64X or i386, as the engine's) and
  builtin marker, every export of the engine's DLL of the same name in each view, `winemetal.so`'s
  architecture, macOS floor and libraries, every symbol it imports from `winemac.so` and `ntdll.so`, and
  `macdrv_functions`. An export counts only with an address. The job then breaks a copy
  (`.github/gw2/zero-export.py` clears two exports' addresses, and an i386 DLL takes an ARM64X one's
  place) and requires the script to fail on exactly those four checks. The engine's table has the 10 entries in the order `winemetal_unix.c` declares,
  and its window data the same first four fields (athei/wine `53def892`,
  `dlls/winemac.drv/window.c`, which asserts both). No warnings list for this job yet: llvm-mingw's clang
  warns differently from GCC.
- **Native tests**: `zsh -n` on the zsh scripts and a self-check of `.github/gw2/warnings.zsh`, then
  `tests/gw2/run.zsh pure` and `metal`. The zsh checks run here because macOS has zsh, so they install
  nothing (#174). The runner's GPU is an Apple Paravirtual device
  with Metal 3 and no Metal 4. There, `commit_record_metal_test`, `pso_archive_test` and
  `shared_event_listener_test` pass. `chunk_submission_test` and `mtl4fx_test` run their Metal 3 half and
  skip, while `mtl4_bridge_test`, `mtl4_queue_test`, `mtl4_specialize_test` and `paired_replay_test` skip
  outright (no Metal 4).
  `pso_archive_tool_test` skips without the AIR copy from my Mac (`GW2_AIR_DB`). `thread_priority_test` skips
  because the runner's jobs run clamped to the utility band (maximum priority 20). A skip shows as `skip`
  with the test's reason. It never counts as a pass. The Metal 4 paths are still tested only on my Mac.
- **Lint**: actionlint on the workflow (`.github/actionlint.yaml` names the `xcode-27` label it doesn't
  know yet).
- **Artifact** `dxmt-<commit>`: the build as `build-dxmt.zsh` stages it (`x86_64-windows`,
  `i386-windows`, `x86_64-unix`, `VERSION`, the licenses), kept 7 days. On a pull request the commit is
  the branch's head, not GitHub's merge commit, so `VERSION` and the startup line name it. To play it,
  unzip it into a directory, then run `scripts/dxmt-use.zsh save <name> <dir>` and
  `scripts/dxmt-use.zsh <name>`.
- **Artifact** `dxmt-mach9-<commit>` (#182): the arm64 build in the layout of the engine's
  `lib/wine/dxgi/dxmt` (`aarch64-windows`, `i386-windows`, `aarch64-unix`), the licenses and `VERSION`:
  its first line is the `git describe` that `d3d11.dll` carries, then `commit:`, `engine:` (the tarball,
  its SHA-256 and the Wine version its `ntdll.so` carries) and `toolchain:`. Kept 7 days. In the engine
  the tree's `aarch64-unix/winemetal.so` is a link to `lib/wine/aarch64-unix/winemetal.so`, which serves
  the i386 DLLs too.
- **Publish** (#194): after every other job is green, packs `dxmt-mach9-<commit>` into
  `dxmt-mach9-<commit>.tar.xz` (the same files at the archive's root, owner 0, directories and `.so`
  755, the rest 644), checks that it unpacks to the artifact's files sha256 for sha256, and prints the
  list and the archive's sha256. For a push to `gw2-on-mac` it adds the archive to the pre-release
  `mach9-builds` of my fork, so a build stays downloadable past the artifact's 7 days, without a
  token:
  `https://github.com/jdrly/dxmt/releases/download/mach9-builds/dxmt-mach9-<sha>.tar.xz`, with the
  full sha. GitHub reports each asset's sha256 as its `digest`
  (`gh release view mach9-builds -R jdrly/dxmt --json assets`). One release, one asset per commit, the 30
  newest kept. An asset that's there already is never replaced. The release is never marked latest, and
  its tag is a lightweight one, which `git describe` and so `VERSION` skip. A run with a red job
  publishes nothing. GW2 on Mac's dev build (jdrly/gw2-on-mac) takes its DXMT from this asset.

## Native tests

`tests/gw2/run.zsh [pure|metal|all] [--meson <build dir>]` (X1) builds and runs every registered test
under `tests/gw2/`, on the Mac, with no Wine and no game. The CI runs it too ([CI](#ci)). A test registers in its header:
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
identical. Since #152 it also checks, for each `DXMT_COMPILE_PRIORITY` setting and both policies, that
every worker, the first two and those a submit adds, applies the service's priority once, on itself,
before its first job.

`retirement_ledger_test.cpp` (L1, #66) checks the retirement ledger (`src/dxmt/dxmt_gw2_retirement.hpp`)
with counting owners: nothing is released before its chunk retires, today's cross-category release
order, ring wrap at 32, 64 and 128 with a stale generation refused, two queues with the same
sequence, repeated and out-of-order retirements refused, no hold after retirement began (even from a
release), and teardown's slot-by-slot drain. Since L2 (#69) also the retention policy against every
hold call site's former gate in each reachable configuration (Metal 3 retained or unretained, the
lifetime check, the residency set, Metal 4), with the scripted release-trace parity run per
configuration. Since L3 (#91) retirement and drain take the completion tokens: an unbound ledger,
another issuer's token (two queues, the same sequence), an empty (moved-from) token and another
issuer's drained token are refused and release nothing.

`retirement_recovery_test.cpp` (#113) checks `DXMT_RETIRE_RECOVERY`'s recovery (`retireOrRecover`): a
scripted queue with Metal 4's holds (drawable, presenter, present split) at rings of 32, 64 and 128,
with one injected refusal (an empty token, a missed retirement, a token behind the ledger), gives one
refused retirement that doesn't follow another (one error line), live holds within the ring's worth
afterwards and nothing left refused, where plain `retire()` piles every later hold up until drain; a
refusal lasting two rings (stale generations refused) releases them as their chunks complete; a hold
refused for a retired sequence goes once every chunk the ring may have in flight completed; every
owner checks that its chunk's completion was proved before its release; without refusals, the release
trace equals `retire()`'s. A threaded case (an encode thread holding under the ring discipline while
the finish thread stalls for three rings and recovers, with a ring slot left free until recovery scans
it) runs a second time under ThreadSanitizer (`${OUT}_tsan`): no data race, no early release, nothing
left (the ring slots' owners are atomic since the review of #116).

`chunk_submission_test.mm` (Q2, #78) runs both production adapters on real Metal through the real
bridge table, and a scripted adapter; since L3 (#91) a retirement ledger bound to each adapter is
retired with its `wait` tokens and drained with its `drain` token, and another issuer's token is
refused by the ledger and by the adapter. Since #86 it checks, for every Metal 4 commit, that the
residency hook runs before the commit is submitted (the open commit's queue value still 0). Since #113
the scripted adapter's `retireWaited` (the recovery after a refused retirement) retires the last wait's
chunk once, with that wait's records, and nothing else; and `gw2::retireStep` as `RetireChunk` runs it,
with empty tokens at a middle chunk and at the last committed one: the CPU fence follows the finish
loop's sequence (so `WaitCommittedRetired` returns), the submission retires every chunk once, and the
ledger releases only with a token's completion.

`paired_replay_test.mm` (B4, #105) replays one frame's packet stream through both interpreters of the
bridge's unix side, through the real table: Metal 3 (`winemetal_unix.c`, on `MTLCommandQueue` command
buffers) and Metal 4 (`mtl4.c`, through the production `gw2::Mtl4Queue` and `gw2::Mtl4Encoding`). The
frame is GW2-shaped: a load chunk retired before it, UpdateSubresource uploads, a scene pass with
argument buffers, `UseResource`, Map'd ring constants moved by `Set*BufferOffset`, `SetFragmentBytes`,
indexed, depth-rejected, instanced and blended draws, 256 KB of `SetFragmentBytes` in one pass (Metal 4's
bytes arena resumes), compute with a barrier, blits, mipmaps, and the present split into a drawable of a
windowless layer. The scene, the rows pass, mip 2, the compute output and the drawable are read back and
compared byte for byte between the backends and with the CPU's values at sample points. The arm64 run
uses `MTL_SHADER_VALIDATION=1`: without it a missing residency doesn't show. A known divergence goes
into the test's `kKnown` list (by readback) with its issue: the comparison and Metal 4's CPU checks of
that readback are reported as XFAIL, Metal 3's still count, and XPASS fails the run (#107). If the layer
gives no drawable, the rest is compared and the test exits 77 (skipped) unless something failed.

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

`shared_event_listener_test.m` (#127) runs the shared-event listener's real unix handlers
(`winemetal_unix.c`'s `#ifndef DXMT_NATIVE` block, cut out of the source by its build lines, with a
stand-in `NtSetEvent`) as the PE side drives them: start on a thread of its own, destroy from another,
then a join under a watchdog. Destroy before start, destroy racing start (3,000 times, 0–200 µs apart),
delivery on the listener thread, a notification before start, and notifications still pending at
destroy (dropped); the arm64 binary runs under AddressSanitizer. Before the port of athei/dxmt
`0cd8b76e9a`, the joins hung. The last three need a Metal device: without one it runs the first two and
exits 77, skipped, not passed (#131); a third run with `LISTENER_TEST_NO_DEVICE=1` checks that path.

`clock_bench.cpp` (#28) is not a native test: it is a Windows .exe that measures the clocks PE code
can read (ns per call, step size, backward steps), and `tests/gw2/clock_bench.zsh <engine> <scratch dir>`
runs it headless under an APFS clone of the engine in a throwaway prefix (winemac disabled). It chose
`gw2::nowNs`'s clock, `QueryPerformanceCounter`: 52 ns a read with 100 ns steps, against `steady_clock`'s
55 ns with 1 µs steps on the wall clock. Every clock finer than Wine's server tick is a crossing into
the unix side; `KUSER_SHARED_DATA`'s interrupt time is a 1.4 ns read but changes only when the
wineserver wakes (about every 18 ms in an idle process).

The native PSO archive tool (#111): `pso_archive_tool_test.mm` (metal) builds the tool for x86_64 and
arm64 and a generator that writes synthetic recipes from real AIR: an isolated copy of DXMT's AIR
cache (`$GW2_AIR_DB`, else lane C's `work/dxmt-squeeze/native/w0/sample.db` in the GW2 on Mac
workspace; skipped without it), copied again into the test's directory. Its recipes go through C3's
serializer in five shapes (full VS/PS, blending, sparse color slots, MSAA, fragment-less), each checked
to build on the device. `pso_archive_tool.zsh` then checks that `run` publishes all of them, that fresh
processes on both architectures hit every one, `status`, the refusal under a held writer lock (the
store unchanged), a second run's newer generation, an injected crash at one recipe isolated by
the bisection, the rest published and hit, and six faked named packer failures (a third build with
`PSO_ARCHIVE_TESTING`) excluded over several flushes, the rest published and the six classified as
excluded misses. First, without AIR or Metal (so even when the rest is skipped), `DXMT_CACHE_DIR`
(#136): empty or relative, `status` warns once and its defaults stay under the user cache dir;
absolute, they are under it; longer than `PATH_MAX`, `status` and `run` exit 1 unless every path is
given.

`cache_dir_test.mm` (pure, #134) checks `DXMT_CACHE_DIR` through the bridge's unix side: absolute, a
cache open (`CacheWriter`, `CacheReader`) creates `<dir>/dxmt/<exe>/<file>` and nothing in the user cache
dir, a trailing `/` changes nothing; unset, the base is `$(getconf DARWIN_USER_CACHE_DIR)`; empty or
relative, it is ignored (no directory created relative to the working directory); longer than the
buffer, the base is refused, not truncated. The PE side's parse (`dxmt_gw2_cache_dir.hpp`, which logs
and shows the base) agrees with the unix side on each value.

`update_scope_test.cpp` (pure, #145) checks `gw2::UpdateScope`, the counting of a one-call update
(`DXMT_UPDATE_ONE_CALL`): `MAP`'s buckets and `PACE`'s `map` calls come out the same as the nested
`Map` and `Unmap` scopes give for the same updates, an update inside another entry point's scope is
left to that scope in `PACE`, and the 1 in 64 timing, scaled, lands near the true time, with `MAP` and
`PACE` taking the same interval. Since #150 it also checks the clock's floor (an empty interval reads 0
on average, though the clock steps 100 ns), that a buffer's `Map` reads the clock twice a timed call
(the shim counts `QueryPerformanceCounter` calls) with `MAP` and `PACE` on the same interval, and that
a texture's `Map` is still timed on every call at scale 1.

`getdata_flush_test.cpp` (pure, #158) checks `gw2::getDataSkipsFlush`, the rule of
`DXMT_GETDATA_SKIP_FLUSH`: a poll of a query whose chunk is still being recorded flushes, a committed
one's skips, and the switch off, `exact` or an unknown chunk flush as before. A loop of polls without
`DONOTFLUSH` on a query of the current chunk flushes once and then skips.

`thread_priority_test.c` (#151, pure, arm64 and x86_64) calls `WMTSetThreadBasePrioritySelf(20)`
through the PE thunk and the production table, linked with the whole unix side as the Metal 4 tests
are. Each call runs on a thread first put into the state the wineserver leaves a Wine thread in for
`THREAD_PRIORITY_NORMAL`, `HIGHEST` or `TIME_CRITICAL` (extended and precedence policies), and the
thread's base priority, read with `proc_pidinfo`, must be 20 afterwards. The main thread waits by
polling, not `pthread_join`, until the thread has measured, because a joiner's QoS override lifts the
joined thread to base priority 31 while the join lasts. It skips when the thread's maximum priority
is 20 or less, as in the CI runner's jobs (`taskpolicy -c utility` gives the same), where every thread
stays at 20 whatever its importance. A task only niced down to 20 keeps maximum priority 63 and runs it.

`texture_view_key_test.cpp` (pure, #155) checks the texture view key's array bit
(`dxmt_texture_view_key.hpp`): for each of Metal's texture types, the bit of a key built from a view of
that type matches the mask `checkViewUseArray` used before, and the key's other fields are unchanged;
the index keeps 27 bits. It then drives `resolveViewUseArray`, the body of `Texture::checkViewUseArray`,
over a fake texture with `createView`'s search, for every type, both of the shader's wishes and both
values of `DXMT_VIEW_KEY_ARRAY_BIT`: a match returns the key, without reading a descriptor when the bit
is trusted; a mismatch returns the counterpart view (type and slice count from the test's own table),
created once and found again; 3D and texture buffers keep their key.

`binding_hazard_test.cpp` (pure, #147) drives the real `BindingSet` (`dxmt_binding_set.hpp`) through
what the context does with a texture bound as a shader resource and then as a render target, with
`DXMT_SKIP_SAME_SRV`'s skip test and `ResolveSRVHazard`'s unbind loop copied from the context (the
setters themselves need the PE toolchain and Metal). A plain view set again is skipped. A view that
can be an output is never skipped, is unbound by the render target, and is bound again afterwards. A
slot that held a plain view and was given one that can be an output keeps upstream's clear hazard bit,
so the render target's unbind misses it and the skip's test would match; with the switch's
`set_hazard`, neither happens.

`emit_window_test.cpp` (pure, #148) checks the emit window (`dxmt_gw2_emit_window.hpp`) over the
fork's own ring allocator and command list, with a fake queue that commits and completes chunks as
`CommandQueue` does and a ring of 64 KB blocks. Nothing the ring hands out may overlap a command of a
chunk that hasn't completed, and each command runs in its own chunk with its payload intact. It covers
a commit in the middle of a run of emits (each side in a slab taken while its own chunk was current),
three chunks sharing a block whose first two complete before another user takes most of a block (run
once more without the reset on commit, where the check must catch the reuse), commands larger than a
slab, and 300,000 random steps of emits, other users' allocations, commits and late completions. It
prints the ns an emit before #148 (an out-of-line ring allocation per command) and through the window.

`layer_poll_test.cpp` (pure, #156) checks when the presenter asks the bridge for its layer's screen
values (`dxmt_gw2_layer_poll.hpp`): at every Present with `DXMT_LAYER_PROPS_POLL_MS=0`; with 250 ms at
the first Present, then not before 250 ms after the last ask (8 asks in 2 s at 60 fps); and at the
next Present after a resize, which restarts the interval.

`present_pace_test.cpp` (pure, #157) checks `DXMT_PRESENT_QUEUE=pace` (`dxmt_gw2_present_pace.hpp`)
with made-up timestamps, no real time: the per-frame skip count gives the PRESENT section's `missed`
and `starved` (nothing across a dropped frame, without a vsync grid or with the frame limiter); a
block is answered once, with a sleep of the block less 2 ms, capped at one refresh less 2 ms; a
missed or a starved refresh within 500 ms of a sleep makes pace give up at once, report it once and
never sleep again; skips while pace hasn't slept for 500 ms (a loading screen) don't count.

## PSO archive tool

`tools/gw2/pso_archive_tool.cpp` (#111) is a macOS executable, not part of the DLL build: it builds
the pipelines W1 recorded (`recipes_1.db`, `DXMT_PSO_RECORD`) after the game exits and publishes them
as one generation of the PSO archive store, which a session with `DXMT_PSO_ARCHIVE=1
DXMT_PSO_GENERATIONS=1` loads for lookup (see [COMPILE](#compile), *Generations*). It runs the game's
own code, linked natively: the recipes are read through the bridge's `CacheEnumerate` and planned by
W2/W3's `warmPlanAdd` (same order and target filter as `DXMT_WARM`); each function comes from the AIR
cache through `CacheReader` (read-only, never a translation); each pipeline is built by C3's
`Metal3PipelineBuilder`, so `_MTLDevice_newRenderPipelineState`'s descriptor conversion and the
archive's lookup and capture (`pso_archive_new_render`) are the bridge's; the store is opened and
published with `WMTPSOArchiveInit`/`WMTPSOArchiveFlush` (generations, the exact-shape policy, its
retries and the last good generation). Build it with `tools/gw2/build-pso-archive-tool.zsh <output>
[x86_64|arm64]`; GW2 on Mac wraps it as `scripts/pso-archive-tool.zsh build|run|status`.

| Command | What it does |
|---|---|
| `run` | Builds every recipe in a child process and publishes one generation (this run is one session of the store: its shard, plus the newest 3 other sessions' shards). A publish failure that names a pipeline excludes its shape (the archive's exact-shape policy, 3 writes a flush); the child flushes again while the failures keep naming shapes (up to 64 flushes). A child that crashes, or whose publish fails without Metal naming a pipeline, is bisected in probe processes, each publishing a range into an empty scratch store no game reads; a recipe that fails alone is left out and the rest published (up to 4 rounds). Then a fresh process verifies the lookup |
| `verify` | A fresh process builds the recipes against the published generation and reports hits and misses; it captures and publishes nothing |
| `status` | Each store's current generation (shards, pipeline keys, size, excluded shapes), whether its writer lock is held, and the recipe file. Opens nothing for writing; probes the lock for an instant |

Options: `--cache-dir` (DXMT's per-exe cache, default `<base>dxmt/<exe>`, where `<base>` is `DXMT_CACHE_DIR` if it's absolute (#134), else `$(getconf DARWIN_USER_CACHE_DIR)`; the archive's default store is `<base>dxmt/pso`. An empty or relative `DXMT_CACHE_DIR` is ignored with one stderr warning worded like the DLL's, `pso_archive_tool: Caches: DXMT_CACHE_DIR="<value>" isn't an absolute unix path; ignored, …` (#136). A `<base>` that can't be resolved, a `DXMT_CACHE_DIR` longer than `PATH_MAX`, is exit 1 when a default needs it, never a path relative to the working directory),
`--recipes`, `--air` (default `<cache-dir>/shaders_<target>.db`), `--archive-dir` (as
`DXMT_PSO_ARCHIVE_DIR`; another directory is a store no game reads), `--exe` (default `Gw2-64.exe`),
`--keep` (sessions a generation keeps, default 4 as the DLL), `--limit`, `--metal-version` (default as
DXMT picks it: 320 on macOS 15+ with an Apple7 GPU), `--quiet`.

Output lines (stdout): `RESULT recipes= selected= built= failed= nofunction= airmiss= fnfailed=
othertarget= malformed= hits= misses= known= new= missexcl= excluded= skipped= generation= published=
flush= mode=` per child (`failed`: Metal refused the pipeline, listed as `BUILDFAILED <index> <recipe key>
<error>` without `--quiet`; `nofunction`: a function missing, its AIR (`airmiss`) or the library
(`fnfailed`); `hits`/`misses` as the archive counts them; `skipped`: pipelines left out after Metal's packer named
their shape; `mode` `writer` or `lookup`), `ISOLATED
<index> exit=<code>` per recipe the bisection left out, `VERIFY …` (the verifying process's result) and
`LEFTOUT <n>`. The archive's own report lines go to stderr.

Exit codes: 0 done; 1 usage or input error (no recipes or AIR file, no device, no `<base>` for a default path); 3 the store is busy:
another process (a running game with generations on) holds its writer lock, and nothing was written;
4 publishing failed; 5 a failure the bisection couldn't isolate; 6 published, but the verifying process
failed (`VERIFY failed exit=<n>`; the generation stays). A failed run leaves the last good
generation. A game started while the tool writes gets the store's lookup-only role for that session.
`PSO_ARCHIVE_TOOL_TEST_CRASH=<index>` (tests only) aborts a child at that recipe; a build with
`build-pso-archive-tool.zsh <output> <arch> testing` also takes the archive's `PSO_ARCHIVE_TEST_*` fakes.
