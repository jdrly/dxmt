# My fork as a patch series

Every commit of my fork on top of 3Shain's `main`, sorted into topics: what each topic does, its
switches, what the GW2 on Mac app does with them, the evidence that it pays, and whether I keep it.
The workspace's ADR 0015 makes each of my forks such a series. DXMT moves by merging 3Shain's
`main` ([upstream-sync.md](../agents/upstream-sync.md)), so the series branch is the inventory by
commit, not a rebase tool.

- **Base:** `faa73e4a0686e432ce5d3bb99a02b5363d76885c`, 3Shain's `main` at my last sync (#355).
  The base is `git merge-base gw2-on-mac upstream/main`, and each sync moves it. Upstream's
  commits below it belong to no topic.
- **Head of the topic table:** `795f0f9` (#359's merge). The commit that adds this file is placed
  by the `docs` rule.
- **Series branch:** `series/2026-11-26`. Its tree equals `gw2-on-mac`'s at the commit after
  `795f0f9` that adds this file.

## Merges

- The series skips merges. It is the commits that aren't merges, picked one by one onto the base.
  They were written on four bases: `3b25259`, `647b002` (the sync `adca1fd`), `0df8610` (the sync
  `fe0beb9`) and `faa73e4` (the sync `886d698`).
- Two picks meet a hunk that a sync resolved by hand. That is `CreateSwapChain` in
  `src/d3d11/d3d11_swapchain.cpp`, where upstream's `b09855e` added an `E_INVALIDARG` return next
  to my MetalFX swap chain switch. `fe4748f` (#63) takes it with `merge=union`, keeping both
  sides as `fe0beb9` did. `64b3fbb` (#290), which gave that line back to upstream, takes its own
  side. The `series-drift` block holds both rules. Every other pick is clean.
- `adca1fd` and `886d698` were clean merges, and `fe0beb9` had the one resolution above. Each
  pull request's merge (`82218c4`, `6bbb36e`, `795f0f9`) has its second parent's tree. All six
  are in `upstream-sync`.

## How to read the columns

- **Switches.** The fork's switch table in [README.md](README.md#switches) at the head above,
  which has 60 rows. Lint holds that table to the sources (`.github/gw2/switch-table.py`). The
  value in brackets is the fork's default.
- **0.2.1.** What the app's `arm64` branch at `d2f888c` does with the switch in a release build.
  - *Set* means the app writes it, in the game's compatdb rule (`rules_text`) or in the session's
    environment (`game_env`). `game_env` sets `DXMT_CACHE_DIR`, `DXMT_DEPENDENCY_COMPRESSION=1`
    and `DXMT_WARM=all` outside the rule.
  - *Shipped, unset* means the code is in the DLL and the fork's default applies.
  - *Not in play* means a diagnostic nobody turns on, or code that is gone at head.
  - 0.2.1 bundles my fork at `da80816` (#339), by the release notes of `v0.2.1-rc.1`. Of the
    commits after it, #358 and the sync of #355 change the DLLs. #358 adds the only new switch,
    so every switch below is in 0.2.1 except `DXMT_MTL4_PRESENT_FENCES`.
- **App names it.** Where the app's `arm64` branch has the variable's name. *Release* is
  `rules_text` or `game_env`. *Dev list* is `DEV_SWITCHES` or `DEV_ROWS`, which only a
  development build reads. *Gone list* is `DEV_GONE`. Dropping a switch that the release or the
  dev list writes is a break, and those rows say so.
- **Evidence.** Notes named by file are in the GW2 on Mac workspace's `docs/research/`.
- **Verdict.** Keep, drop or decide. A drop is a proposal, and I decide every one. I propose one
  only where a note records that the change doesn't pay. Once I decide it, the verdict says
  "dropped" with the date. Everything undecided is "decide".

## Topics

38 topics, 177 commits.

| Topic | Commits | What it does | Switches (fork default) | 0.2.1 | App names it | Evidence | Verdict |
|---|---|---|---|---|---|---|---|
| `present-pacing` | 9 | The present split, display sync in place of a one-refresh minimum, a present deadline so macOS raises the GPU clock, frame latency 0 read as 3, a frame limiter for caps that aren't a divisor of the refresh, present timing in the log, the screen's EDR values cached, and three later fixes behind switches | `DXMT_PRESENT_SPLIT` (on), `DXMT_DISPLAY_SYNC` (on), `DXMT_PRESENT_DEADLINE` (one refresh), `DXMT_CAP_LIMITER` (on), `DXMT_LAYER_PROPS_POLL_MS` (250), `DXMT_CONTROL` (off), `DXMT_REFRESH_REQUERY` (off), `DXMT_STEADY_SLEEP` (off), `DXMT_PRESENT_SKIP_NIL` (off) | `DXMT_DISPLAY_SYNC=0` set while Vsync is off. The frame cap goes through `d3d11.preferredMaxFrameRate` in `dxmt.conf`. The rest shipped, unset | Release: `DXMT_DISPLAY_SYNC`. Dev list: `DXMT_REFRESH_REQUERY`, `DXMT_STEADY_SLEEP`, `DXMT_PRESENT_SKIP_NIL`. README only: `DXMT_CAP_LIMITER` | The GPU sat at its minimum clock until presents had a deadline and latency 0 stopped meaning "take turns" (`dxmt-trace-2026-10-27.md`). The deadline A/B is in `metal4-present-latency-2026-11-04.md`. The three off switches have no measurement | **keep.** The defaults are how the game reaches 60. The three off switches are for me to decide one by one, and all three are on the dev list. `DXMT_CONTROL` **dropped** on 2026-11-27 (#363, ADR 0020): nothing writes the control file |
| `background-throttle` | 4 | Present sleeps to 30 fps while another process's window is in front, and to 10 while the game is minimized | `DXMT_BG_FPS` (30), `DXMT_BG_FPS_MINIMIZED` (10), `DXMT_BG_POLICY` (`focus`) | shipped, unset | Comment only: `DXMT_BG_POLICY`, as a value the dev list leaves out | No measurement. It is behaviour I chose, not a speed-up | **keep.** Always on |
| `depth-compression` | 1 | Fixes the dark smudges in fire and smoke that the compressed depth targets of `007a574` caused, and adds the `DEPTHVIEW` and `DEPTHSRV` lines. The compression itself came in `9688571` and `007a574`, which are in `present-pacing` | `DXMT_DEPTH_PFV` (off), `DXMT_STENCIL_PFV` (off, read since `007a574`) | shipped, unset | Comment only, as switches the dev list leaves out because they bring upstream's behaviour back | GW2's 4K `R24G8_TYPELESS` depth buffer was uncompressed before (`dxmt-trace-2026-10-27.md`). No number for what the compression saves | **keep.** Always on, and `=1` is the way back for a bisect |
| `stats-and-counters` | 7 | The 2 s `STATS`, `PACE`, `GPU` and `HITCH` lines, long log lines cut for Wine, and four counters: `CREATE`, `WaitForVBlank`, autoreleases on a thread without a pool, biased draws into emulated D24 targets | `DXMT_STATS` (on, read since `9688571`), `DXMT_HITCH_MS` (50) | `DXMT_STATS=1` set with Debug logging. Unset it is on anyway | Release: `DXMT_STATS`. The session report reads `STATS`, `HITCH`, `PRESENT`, `COMPILE`, `METAL4` and `WARM` | Every measurement in `docs/research/` since 2026-10-28 comes from these lines | **keep.** Diagnostics the app reads. The `WaitForVBlank` count **dropped** on 2026-11-27 (#363, ADR 0020): `vblank` was 0.0 in all 11,348 windows that have it |
| `pace-scope` | 6 | The timing scope around each D3D11 call: off emulated TLS, on `QueryPerformanceCounter`, one call in 64 timed, the clock's own cost taken off. Two commits are its native test | none of its own, under `DXMT_STATS` | shipped, on with `DXMT_STATS` | no | #37 took the scope from 0.11 to 0.02 µs a call, about 2.3 ms a frame before (`dxmt-squeeze-2026-11-03.md`) | **keep.** Diagnostic |
| `thread-priorities` | 3 | Three switches for where threads run: the game's render thread (#14), DXMT's encode and finish threads (#167), the compile workers (#170) | `DXMT_RENDER_THREAD_PRIORITY` (off), `DXMT_THREAD_PRIORITY` (`critical`), `DXMT_COMPILE_PRIORITY` (`critical`). Each default is upstream's behaviour | shipped, unset | Comment only. The dev list leaves all three out because lower priorities cost frames in crowds on 0.1.x | The switch table says "no result yet" for #151 and #152. The render thread already runs on the S cores (the workspace's `AGENTS.md`, `HEAT_RENDER` "measured and not worth it") | **dropped** on 2026-11-27 (#363, ADR 0020). None became a default and no session ran another value. Bridge calls 153 and 200 are holes now, and `thread_priority_test` went with the topic |
| `pipeline-lookup` | 2 | The pipeline key hashes the blend state, each context checks its last 4 pipelines first, a topology change of the same class keeps the pipeline (#17), and a draw skips a bind of the pipeline its encoder already has (#36) | `DXMT_PSO_BLEND_HASH` (on), `DXMT_PSO_MRU` (on), `DXMT_PSO_TABLE` (off, #366), `DXMT_PSO_TOPOLOGY_KEEP` (on), `DXMT_SKIP_REBIND` (on) | shipped, unset | no | #36 with #38 removes 55% of the pipeline binds a frame (`dxmt-squeeze-2026-11-03.md`, round 2) | **keep.** Always on but `DXMT_PSO_TABLE`, which came after 0.2.1 and waits for a dev-list row in the app (`DXMT_PSO_TABLE=1`) and #366's session. `DXMT_PSO_TOPOLOGY_KEEP` lost its switch on 2026-11-27 (#363, ADR 0020) and its path stays always on: `topokept` was 0.0 in 21,729 of 21,732 windows, and `PSO topokept` still counts it |
| `chunk-ring` | 1 | The `RING` line, a ring size switch, and a switch that replaces the game's frame latency | `DXMT_CHUNKS` (unset, 32), `DXMT_MAX_FRAME_LATENCY` (unset) | shipped, unset | Comment only: `DXMT_CHUNKS`, "a value only I can choose" | "No evidence for a bigger ring": 0 stalls, occupancy 1.5 to 1.7, peak 3 of 32 (`dxmt-squeeze-2026-11-03.md`, which proposes closing #8) | **keep** the `RING` line, at upstream's 32. Both switches **dropped** on 2026-11-27 (#363, ADR 0020): the ring peaked at 4 to 9 of 32 with 0 stalls in 68 sessions |
| `compile-diagnostics` | 4 | The `COMPILE` section: the encode thread's pipeline waits and what each waited for, a variant's time by stage, the head start from `CreateShader` | `DXMT_COMPILE_WAIT_MS` (2) | shipped, on with `DXMT_STATS` | no. The session report reads `COMPILE` | 188 encode-thread waits, 645 ms, none of them queued, on 2026-11-04 (the workspace's `AGENTS.md`, shader warming). That number is what sent warming after builds, not the queue | **keep.** Diagnostic. `1de145b` also brought the first PSO archive, which `gone-pso-archive` deletes |
| `commits-and-timestamps` | 4 | The `CMDS` line, timestamp queries estimated in place of a commit per query (#26), no flush on a `GetData` poll (#190), and #280, which takes out the deferral and `boundary` mode the first two added | `DXMT_TIMESTAMP_MODE` (`estimate`), `DXMT_GETDATA_SKIP_FLUSH` (on) | shipped, unset | no | 36 command buffers a frame before. With #27, busy Lion's Arch went from about 55 to 60 FPS and saved about one core (`timestamp-map-ab-2026-10-28.md`). All 1,419 `TIMESTAMP` windows of the long dev.23 session say `mode=estimate` (`upstream-watch-triage-2026-11-25.md`) | **keep.** Always on |
| `map-update-fast` | 4 | The `Map` and `Unmap` fast path for buffers (#27), `UpdateSubresource` in one call (#166, #288), a bump window for command data (#177) | `DXMT_MAP_FAST` (on) | shipped, unset | no | 3.1 ms of `Map` a frame before (`timestamp-map-ab-2026-10-28.md`). #166 and #177 are from the review of 2026-11-10 (`perf-review-2026-11-10.html`), and the encode thread is at 1.36 ms a frame on dev.23 | **keep.** Always on |
| `residency-set` | 1 | One `MTLResidencySet` per device in place of `useResource` per command buffer (#38) | `DXMT_RESIDENCY_SET` (on) | shipped, unset | no | 8,150 `useResource` a frame gone, the encode thread about 1 ms a frame shorter, 0.8 to 1.1 W less (`dxmt-squeeze-2026-11-03.md`, round 2) | **keep.** Always on. Metal System Traces need `=0` |
| `retirement-and-submission` | 10 | DXMT holds what a chunk uses until it retires, so command buffers can be unretained (#42, #45). The retirement ledger (L1 to L3), the submission seam with its Metal 3 and Metal 4 adapters (Q1, Q2), the recovery from a refused retirement, and two tests | `DXMT_UNRETAINED` (on), `DXMT_RETIRE_RECOVERY` (on), `DXMT_LIFETIME_CHECK` (off, a diagnostic build only) | shipped, unset. `DXMT_LIFETIME_CHECK` does nothing without a diagnostic build | no | 20 minutes of busy Lion's Arch with the lifetime check found 0 early releases (the workspace's `AGENTS.md`, #31). Metal 4 depends on it | **keep.** Always on |
| `metal4` | 10 | Metal 4 queues, allocators, encoders and argument tables (#48 to #51), on by default (#54), bounded residency sets for many swap chains (#186), the chunk's set kept empty (#173), no empty command buffer after the present (#189), an allocator pool (#233), and the three opt-ins made defaults (#302) | `DXMT_METAL4` (on), `DXMT_MTL4_ALLOCATOR_POOL` (on), `DXMT_MTL4_DEVICE_RESIDENCY` (on), `DXMT_MTL4_TIMESTAMP_REUSE` (on) | `DXMT_METAL4=1` set by the Metal row at 4, `=0` at 3. The other three shipped, unset | Release: `DXMT_METAL4`. Gone list: the three opt-ins, with `=0` as their off value | Encode thread about 2.1 ms a frame against 4.6 and more on Metal 3 (the workspace's `AGENTS.md`). The three opt-ins became defaults after a 2.6-hour session with their error counts at 0 (the switch table) | **keep.** The app's default |
| `metal4-present` | 5 | On Metal 4 the frame is presented through a Metal 3 command buffer (#229), the next frame waits for it only before it writes the back buffer (#272, the default since #333), the kind of present is picked once (#284), and the present's fences go to the Metal 4 queue behind a switch (#358) | `DXMT_MTL4_PRESENT` (`mtl4`), `DXMT_MTL4_SHOWN_WAIT` (`write`), `DXMT_MTL4_PRESENT_FENCES` (off) | `DXMT_MTL4_PRESENT=mtl3` set at Metal 4. `DXMT_MTL4_SHOWN_WAIT` shipped, unset, and `write` acts only with `mtl3`. `DXMT_MTL4_PRESENT_FENCES` isn't in 0.2.1 | Release: `DXMT_MTL4_PRESENT`. Gone list: the first two. `DXMT_MTL4_PRESENT_FENCES`: not at `d2f888c` | A Metal 4 present saws 16.7 and 33 ms while the pointer moves. `mtl3` is flat and ran about 58 fps median against 50.5 on Metal 3 (`metal4-cursor-stutter-2026-11-12.md`). With the wait at `write`, dev.18 and dev.20 held 60 in combat where dev.17 dipped (ADR 0017). Without the present's fences, `dependency_compression_test` shows missed dependencies against its fence oracle, and none with the switch (ADR 0018) | **keep.** The app's release rule writes it. Dropping it breaks 0.2. `DXMT_MTL4_PRESENT_FENCES` waits for the aerodrome A/B in my dev builds |
| `gone-mtl4-present-tests` | 3 | The two test switches for the same stutter, `DXMT_MTL4_LAYER_SET` (#231) and `DXMT_MTL4_PRESENT=done` (#243), and #278, which removes both | none at head | not in play | Comment only: `DXMT_MTL4_LAYER_SET` | Both lost (`metal4-cursor-stutter-2026-11-12.md`, #278) | **keep as history.** Neither is read at head. The bridge manifest keeps call 202 reserved, with a comment that names `done` |
| `metalfx-subsample` | 1 | Upscales GW2's Subsample 3D pass with MetalFX and keeps the UI native (#59) | `DXMT_SUBSAMPLE_METALFX` (off), `DXMT_SUBSAMPLE_METALFX_PS` (unset) | `DXMT_SUBSAMPLE_METALFX=1` set while the MetalFX row is on. The row is off by default, so unset for most players | Release: `DXMT_SUBSAMPLE_METALFX`. A dev build turns the row on (`DEV_ROWS`) | None in game. It still needs a capture of the pass (`metalfx-subsample-2026-11-04.md`) | **keep.** The app has a row that writes it. Whether the row should exist before the capture is for me to decide in the app |
| `shader-warming` | 10 | Records each pipeline's recipe and its use (W1, #124), and a later session builds the most recently used ones in the background (W2, W3, #128). Fixes to the reader, the cap count and the plan, more than one speculative job on a cold cache (#195), and #286, which removes `DXMT_WARM_ORDER` | `DXMT_PSO_RECORD` (on), `DXMT_PSO_RECORD_USE` (on), `DXMT_WARM` (off), `DXMT_WARM_MAX` (8192), `DXMT_WARM_JOBS` (1) | `DXMT_WARM=all` set, since the Shader warming row is on by default. The others shipped, unset | Release: `DXMT_WARM`. Dev list: `DXMT_WARM_JOBS=2`. The session report reads `WARM` | The first UAT failed on the rowid plan, and #128's plan by recent use hasn't had a UAT, only normal play (`warm-uat-2026-11-04.md`, `warm-miss-2026-11-04.md`). Metal keeps compiled stages on disk itself, so warming can't remove a wait for a shader this Mac never compiled (`perf-review-2026-11-10.html`) | **keep.** The app's default writes it. Whether it pays is still open, and that is a question for the app's row, not for the series |
| `compile-service` | 8 | One owner for compile jobs (C1), the lost wakeup at stop fixed (#72), declared dependencies and a demand lane (C2), one recipe for the recorder and the build (C3), and four test commits | `DXMT_COMPILE_DEMAND` (off) | shipped, unset. Warming turns the declared policy on, so with the default Shader warming row it runs | Dev list: `DXMT_COMPILE_DEMAND=1` | The stop hung about 1 time in 20 before #72 (the workspace's `AGENTS.md`). No measurement of the declared policy alone | **keep.** The service replaced upstream's scheduler, and warming needs it |
| `air-readers` | 2 | A pool of read connections for the AIR cache, and the count on the startup line | `DXMT_AIR_READERS` (1, upstream's one reader) | shipped, unset | no | Natively the shared reader's lock dominates from 4 workers (the workspace's `AGENTS.md`). In play it doesn't: the reader lock took 3.6 ms in all of the 48-minute dev.23 session (`upstream-watch-triage-2026-11-25.md`) | **dropped** on 2026-11-26 (#361, ADR 0019). It never left 1, the app never names it, and my own log says the lock costs nothing in play |
| `gone-pso-archive` | 7 | The PSO archive after its first version: every pipeline kept (#55), generations (#84, #90), the native tool (#112, #142), a test path fix, and #276, which deletes all of it | none at head | not in play | Gone list: `DXMT_PSO_ARCHIVE`, `DXMT_PSO_GENERATIONS` | With it on the game crashed at exit, and Metal's own cache already keeps what it saved (ADR 0011, `perf-review-2026-11-10.html`) | **keep as history.** No file named `pso_archive` is left at head. The first archive came in `1de145b` (`compile-diagnostics`), and `9eb7668` (`cache-dir`) touched its files |
| `bridge` | 5 | The real unix side linked natively for tests (#62), every call number written once in a manifest (B2, B3), a reused visibility slot kept free of stale callbacks (#41), and a release that drains its own autorelease pool (#270) | `DXMT_RELEASE_POOL` (on) | shipped, unset | no | `autorelease_count_test` shows `release 0 in 101` on a real Mac (`dxmt-metal-run-2026-11-12.md`) | **keep.** Always on, and the tests build on it |
| `switch-registry` | 3 | Switches declared in a registry that writes the startup line (#63), a status for every row of the table (#321), and Lint holding the table to the sources (#323) | none of its own | shipped | no | It is what makes this inventory checkable | **keep.** Structure and docs |
| `native-tests` | 9 | The paired Metal 3 and Metal 4 replay test (B4), the native present test with the moving pointer and its variants (#228, #234, #238, #246), and how the tests build and run (#309, #327, #349) | none | not in play | no | The present test ran on a rented M1 only (`native-present-cursor-2026-11-13.md`) | **keep.** Tests |
| `upstream-ports` | 6 | Four fixes I took from others: upstream `d2dd386` (#129), upstream `0df8610` (#165), athei's listener races (#130, with its test fix #141), athei's `Com` self-assignment with my native test | none | shipped | no | The survey of 2026-11-04 (`dxmt-upstream-survey-2026-11-04.md`) | **keep.** Fixes without a switch. Two of the six are in the base already: `478a3b2` is `d2dd386`, and `3205f08` is `0df8610` itself. Of those two, the squash keeps only my test `tests/gw2/airconv_gs_vertex.zsh` and its Meson line. The listener and `Com` fixes stay as they are |
| `teardown-robustness` | 5 | A DXMT DLL pins itself before its first thread (#139), a presenter freed with its last reference (#192), bounds and leaks on error paths (#296, #298), and teardown waits (#317) | `DXMT_PIN_MODULE` (on), `DXMT_PRESENTER_FREE` (off), `DXMT_DRAIN_SPLITS` (off) | shipped, unset | Dev list: `DXMT_PRESENTER_FREE=1`, `DXMT_DRAIN_SPLITS=1` | #137 was a crash at exit. #317's wait is the one athei's `d29d893` adds (`upstream-watch-triage-2026-11-25.md`). The two off switches have no measurement | **keep.** Mostly always on. `DXMT_PRESENTER_FREE` **dropped** on 2026-11-27 (#363, ADR 0020), after the app's `DEV_GONE` row (jdrly/gw2-on-mac #373): GW2 makes one swap chain. `DXMT_DRAIN_SPLITS` is for me to decide, and it is on the dev list |
| `per-call-cuts` | 6 | Per-call cost off the render thread: the array bit in the view key (#168), the stats and PSO switches read without a call (#169), early returns from setters that change nothing (#172, #178), only the slots a call can change visited (#212), no virtual calls for reflection and slot masks (#214) | `DXMT_VIEW_KEY_ARRAY_BIT` (on), `DXMT_SKIP_SAME_STATE` (on), `DXMT_SKIP_EMPTY_SLOTS` (on), `DXMT_SKIP_SAME_SRV` (off) | shipped, unset | Dev list: `DXMT_SKIP_SAME_SRV=1` | The review of 2026-11-10 found 1.5 to 2.4 ms a frame of removable per-call cost, watts only (`perf-review-2026-11-10.html`). After these the encode thread runs 1.36 ms a frame on dev.23 (the workspace's `AGENTS.md`) | **keep.** Always on but `DXMT_SKIP_SAME_SRV`, which waits for a visual check (#147) and is for me to decide |
| `rename-queues` | 2 | Counts the rename queues' pages (#255), and a thread gives back the ones no `Map` has reached for a while (#268) | `DXMT_RENAME_TRIM` (off) | shipped, unset | Dev list: `DXMT_RENAME_TRIM=600` | The queues hold 14 to 18% of the residency set's bytes, 420 to 430 MB in busy play (`upstream-watch-triage-2026-11-15.md`). No session result for the trim itself | **decide.** The count stays as a diagnostic. The trim never became a default, and its case is memory, not frames. It is on the dev list |
| `arm64-build` | 4 | The ARM64X build for Mach9's engine (#183), Apple's cores as the target (#236), the unix side called directly on ARM64EC (#213), and swap chain entries that don't enter each other's patchable entry (#256) | none | shipped | no | It is what 0.2 runs. #256 fixes the Steam overlay's hook running twice a frame | **keep.** Always on |
| `ci` | 12 | The GW2 workflow on GitHub's runners, its publish step, caches, the skip rules and the Mach9 pins | none | not in play | no | n/a | **keep.** CI |
| `docs` | 4 | The fork's workflow rules, first person, the code rules that keep it mergeable, ADRs 0006 to 0014 | none | not in play | no | n/a | **keep.** Docs |
| `upstream-footprint` | 5 | My code moved out of upstream's files and off their ends (#290, #292, #294, #313), and how a new upstream bridge call enters the manifest (#300) | none | shipped, no change in behaviour | no | #290 undoes the one rewrite that conflicted in the second sync | **keep.** It is what makes the next sync cheap |
| `gone-present-queue` | 2 | `DXMT_PRESENT_QUEUE` (#197) and its removal (#282) | none at head | not in play | Gone list. A hand-written `DXMT_PRESENT_QUEUE=two` line in a dev file stays and now does nothing | `two` can latch 30 fps, and `pace` gave up at frame 181 in every session (#282) | **keep as history** |
| `gone-sparse-buffer-tier` | 2 | #342 and its revert #345 | none | not in play | no | The M2 crash was KosmicKrisp's, fixed in my Mesa fork | **keep as history.** It squashes to nothing |
| `cache-dir` | 1 | `DXMT_CACHE_DIR` gives a setup its own cache folder (#135) | `DXMT_CACHE_DIR` (unset) | set, to the app's own cache folder | Release: `DXMT_CACHE_DIR` | It keeps the app's caches apart from 0.1.x's and the engine's DXMT's | **keep.** The app's release writes it |
| `dependency-compression` | 1 | On Metal 4, encoder fences follow a checkpoint chain in place of the per-encoder window (#133, after GameSir `3170d0349a`) | `DXMT_DEPENDENCY_COMPRESSION` (off) | `=1` set in every build, in the session's environment | Release: `DXMT_DEPENDENCY_COMPRESSION` | In the oracle test, 4.80 to 1.31 fence waits a stage with 0 missed dependencies (`mix-2026-11-04.md`). Played since the mix of 2026-11-04, with no A/B of its own | **keep.** The app's release writes it, so it is a default in all but the fork's own value |
| `upstream-sync` | 8 | The three merges of 3Shain's `main` (`adca1fd` at `647b002`, `fe0beb9` at `0df8610`, `886d698` at `faa73e4`), their three pull request merges, and two commits that write the sync's steps down | none | shipped | no | n/a | **keep.** See [Merges](#merges) |
| `build-lists` | 0 | Added by the rules: every `meson.build` line of the topics above | none | shipped | no | n/a | **keep.** Build lists |

Sum: 9 + 4 + 1 + 7 + 6 + 3 + 2 + 1 + 4 + 4 + 4 + 1 + 10 + 10 + 5 + 3 + 1 + 10 + 8 + 2 + 7 + 5 + 3 +
9 + 6 + 5 + 6 + 2 + 4 + 12 + 4 + 5 + 2 + 2 + 1 + 1 + 8 + 0 = 177.

## Dropped

- **`air-readers`** (2 commits, `DXMT_AIR_READERS`), dropped on 2026-11-26 (#361,
  [ADR 0019](adr/0019-the-air-reader-pool-is-dropped.md)). The switch never left upstream's value,
  the app never names it, and the lock it would relieve took 3.6 ms in 48 minutes of play. The
  shader cache files are upstream's text again. The removal commit joins the topic when the series
  is next built, so its code squashes to nothing.
- **`thread-priorities`** (3 commits, `DXMT_RENDER_THREAD_PRIORITY`, `DXMT_THREAD_PRIORITY`,
  `DXMT_COMPILE_PRIORITY`), dropped on 2026-11-27 (#363,
  [ADR 0020](adr/0020-the-switches-gw2-never-exercises-are-dropped.md)). No session ran a value but
  upstream's. Leaving the topic out of the series put 14 of 86 series commits in conflict, so it
  goes as a removal commit like the rest. Bridge calls 153 and 200 stay reserved in the manifest.
- **Inside topics that stay**, by the same removal commit: `DXMT_CONTROL` (`present-pacing`), the
  `WaitForVBlank` count (`stats-and-counters`), `DXMT_CHUNKS` and `DXMT_MAX_FRAME_LATENCY`
  (`chunk-ring`, whose `RING` line stays) and `DXMT_PRESENTER_FREE` (`teardown-robustness`). Each
  default is what the build did before. `DXMT_PSO_TOPOLOGY_KEEP` (`pipeline-lookup`) lost only its
  switch: it was on, so its path stays always on.

In DXMT a drop is a removal commit on `gw2-on-mac`, as #276 to #288 were, because DXMT moves by
merge and its topics share files.

## For me to decide

- **`rename-queues`**: `DXMT_RENAME_TRIM`. Default, or out. It is on the dev list at 600.
- **Single switches inside topics that stay**, each off in the fork:
  - `DXMT_REFRESH_REQUERY`, `DXMT_STEADY_SLEEP`, `DXMT_PRESENT_SKIP_NIL` (`present-pacing`);
  - `DXMT_MTL4_PRESENT_FENCES` (`metal4-present`);
  - `DXMT_DRAIN_SPLITS` (`teardown-robustness`);
  - `DXMT_SKIP_SAME_SRV` (`per-call-cuts`);
  - `DXMT_PSO_TABLE` (`pipeline-lookup`);
  - `DXMT_COMPILE_DEMAND` and `DXMT_WARM_JOBS` (`compile-service`, `shader-warming`).

  ADR 0010 says a tested switch is deleted or becomes the default. None of these has its test
  yet. Taking out one the dev list writes needs an app `DEV_GONE` row first.
- **`metalfx-subsample`**. The code stays while the app's row exists. Whether a release should
  offer a row whose pass was never captured in game is the app's question.

## The series branch

- **How it is built.** Each commit in the `series-topics` block is cut by the `series-rules`
  block: a path that matches a rule goes to that rule's topic, whatever its commit's topic. So a
  feature commit's workflow lines, tests, docs and `meson.build` lines land in `ci`,
  `native-tests`, `docs` and `build-lists`, and only its code stays with its topic. The pieces are
  picked in history order with `git merge-tree --write-tree --merge-base=<the commit's parent>`.
  A piece joins its topic's last series commit when no later series commit touches one of its
  files (`mode disjoint`), so the result is the linear replay's tree.
- **Why 86 commits.** `docs/gw2/README.md` is in 137 of the commits,
  `src/dxmt/dxmt_command_queue.cpp` in 39, `src/d3d11/d3d11_swapchain.cpp` in 33,
  `src/dxmt/dxmt_context.cpp` in 32 and `src/dxmt/meson.build` in 25, across most topics. Ten
  coarse groups in place of the topics still take 43 commits. The count falls as my code moves out
  of the shared files (ADR 0012).
- **What each series commit says.** A subject from the `series-titles` block, `Series-Topic:` and
  `Series-Part:` lines, and the `gw2-on-mac` commits it is made of. The commits carry the head's
  commit date, so a rebuild from the same input gives the same ids.
- **Topics that lean on each other, oldest first.** `present-pacing` before `stats-and-counters`
  and `pace-scope` (`9688571` reads `DXMT_STATS` and brings the first depth compression).
  `retirement-and-submission` before `metal4`, and `metal4` before `metal4-present`,
  `dependency-compression` and `gone-mtl4-present-tests`. `compile-diagnostics`, then
  `compile-service`, then `shader-warming`. `bridge` before `metal4`'s later commits and before
  `upstream-footprint`. `switch-registry` (`fe4748f`) rewrote how every earlier switch is
  declared.
- **Delete commits in live topics.** `8759a52` (#280) in `commits-and-timestamps`, `707135a` (#286)
  in `shader-warming` and `c4577cd` (#288) in `map-update-fast` each remove a switch their own
  topic added, so the topic squashes to its form at head.

## Rules

- A commit needs no line in `series-topics` when the rules place every path it changes. That covers
  the commit that adds this file, which can't name itself, and later commits that touch only CI,
  tests or docs.
- Any other commit gets its line when the series is next built, and the build refuses one without
  it.
- The series branch is built again under a new date, never rewritten.
- `gw2-on-mac` is never rewritten.

## Blocks

The replay script reads these by their fence label. A line is fields split by the first space or
spaces: `series-topics` is `<sha> <topic>`, `series-rules` `<glob> <topic>` (first match wins, `*`
matches `/` too), `series-drift` `<sha> <path> union|theirs` (`union` merges the path with
`merge=union`, `theirs` makes the pick take its own side of a conflicting hunk), `series-titles` `<topic> <subject>`.

```series-topics
9688571b65796d43ba3f25ac16c841e7ee2b41fc present-pacing
f0f8467861829ac3478209cf66ba6e4b1e8bba75 present-pacing
007a574f0f3bd6122bc92d97c9289d04abc4d82b present-pacing
a6047fefbe1ce38f292b79ee993120966ed0e79a background-throttle
e8c1d626e92be1312e7612ed5b5032e9d4e75764 background-throttle
493b47db56d76897e15375bd2ae66ebd884f17ef docs
d80654da8c0d78fded02967b9160fd9157372dbb stats-and-counters
dda4d0b3b1c227b625882d42fc8a4d65a975771d thread-priorities
b1c7c50c007874a9a58725c3d7c5375f50c2bd4c depth-compression
a7666f6875eaf35811bfbc88d39908121b470864 background-throttle
220ef1db2669eeb9a2b4e8753abf506feb118340 pipeline-lookup
6b913de8a25d08c9f53ab441b15c0ac498830aaf chunk-ring
29505622cf225b0990aaf76d945431c6b64c4ccc present-pacing
7d1f50497d9114b23332bac40ff8796ffc9e3540 stats-and-counters
1de145b496aaed37910c4db6e321b30b487a5fcd compile-diagnostics
f7f8d8baf12d5e038389c1b87471dd0c1700e859 commits-and-timestamps
7dc29ce3334fb0a0db3d841206fc97ec76c031ba commits-and-timestamps
a94a4c108febef65595d51c94aca0acbf7327ab8 map-update-fast
8563ff44f2a2e57243701c117cae6ee2352b8553 pipeline-lookup
dbde5e6b96a448b009c2813bbfb167fc31df59d3 present-pacing
d0f4f452c1dc0a0c17c6994248e932c7d85c6a1d residency-set
1733b5c44bf7391521b9712f83700683410f87a6 pace-scope
f5de8509bc46e05a3a7b07a79a899876f86c0192 background-throttle
6f629639cd11cb4e4f5f560efc406137ebbef995 bridge
16ecb52ea251785dd5eaf5e1e1b4faf83618b5e4 present-pacing
6431bd1fc7ab10ca8acd0ac66c892d833cd865c5 retirement-and-submission
7db0a8aa928fdfb0f2f036330c71ff7a6191abba stats-and-counters
c36dd141fe249eff0bc924d9c7abbececaa7aafc retirement-and-submission
767fed728f0f25af1c274dbd3dc255e5fd839111 compile-diagnostics
385352bff1cf4439ccb1cf6cf19b23ad5e127e3e metal4
8c613315a3f32b1e8c06aa1afe9413b914da151a metal4
302a33302c4b0f035490b76dfd26dfd82f1d90b5 metal4
8edcf50730ed26e8c7c008ca2615de3a5a20d9a2 metal4
8dd4b395d7c6ab893bff5b2fd018dd24505b0272 metal4
02532d0400ddc169896b32feaca3c9ebe163e847 gone-pso-archive
cbb790839a08e9f5865b7e1baa64e00d5c1f1118 metalfx-subsample
3ea524adcfc7c036421849de04e76a902a0678d2 shader-warming
0034ffa944690332990acddd81ea63206edb6ff1 bridge
fe4748fce6f965fff2c184ef61d9f00c5367f2e4 switch-registry
74a78361633898f7b8f1997877d849472ff93281 compile-service
5f16916bd5c57d40c2473e05897e724b3d7a9b8e retirement-and-submission
21522cc29a960ca2a5b3523422b976f78115b098 bridge
13007ad872a3d1420f5ae40e0a0a8d9125e381b6 retirement-and-submission
9ccf75cee35ea229d0ed3c3c3271d497980bd3de compile-service
726fa01fbe2a84e63a177fdeb171162df505a532 retirement-and-submission
741c06074b683e31d89d4abe73e865ba25b30334 bridge
ad4a90a5b593fcca7bfa0f50f368a165df29564f compile-service
fcb56f478d1c6a02e53801341aa7488cb28229fc compile-service
58e9fedcf421ef6204ebdd18041b292ed12c8af8 retirement-and-submission
333ab40887b984f561561d388887340a3beaf8d6 gone-pso-archive
c1b4df9dfda02fd97510a391c1ad9854fe63a51c gone-pso-archive
32f4fb34dec713ffa458f78cc192242d5de97042 shader-warming
7047bf109ccb386c2abcae1adabbe552c62f0f8e pace-scope
92dc6e6b297fd3a71129d09b67fc74c1831017bf compile-diagnostics
c99d46d355aef15adc1df95b66f01390a0a544a4 retirement-and-submission
16548004c38997b0d2a6296cad02b40558806d31 air-readers
8cf99bc33944ba26a65f703b752d647e491e6f98 air-readers
267b5293c8e76f3eeb25c334fe56d2ae53f8ea91 retirement-and-submission
ebba9a7fa3e014ca5923d16c8e7decb41b8ee7bc shader-warming
9bd9de441085a2bdfbc19481635f5bf2b1addafb compile-service
4d70bbc503af52c62c534f5ba2b71a0d4af48548 native-tests
39c9b5dc49cea04aefb0464e8f29eb25d2c8a3ce native-tests
47ed91c2c559632ef42859fc89b4f3fde4db2d39 gone-pso-archive
ef4989fe8aadb7daf43ffbb417b13eaa7f79d18f gone-pso-archive
db6ff5b94d7bb77c02ea60825b1ded6863993db3 shader-warming
40c24322800c233b6e974edff6bc005a5fe4502a retirement-and-submission
1e5f4ef4dd08c90abc7b00ef746b300039bba77d shader-warming
b26bfde24f2e72a8afc3ad7137da4c45da48ec4b shader-warming
d307915be4fe1630de3d0bc79c31c05d61680321 pace-scope
697030c373fd645a31e6d5ad0233a69db50f36b7 shader-warming
478a3b20ec6b3fa16c9742ccf3a7e79fa9cc8c44 upstream-ports
9913b9173cb62ded0c59039c8555f175ca9f7b43 upstream-ports
7550ccd82fca18202320f67093cd1fda3fd379e4 shader-warming
684f4283110cf499b19a9a209955e9d39555aeea dependency-compression
9eb7668fdf6a0c5c5b4bb50afe8f2fc2cf3e8c20 cache-dir
e02a4d6cb5ee4057a8033416ec15b9ba9a29f008 teardown-robustness
1cb30e9371b0e13a4b369fc21e0bbf56ea729245 upstream-ports
47d5ba627e59f4e25a53fc2e666b25a8cc430b8b gone-pso-archive
adca1fda2dddbcf79ad637656c639bb01cdc7212 upstream-sync
82218c4d680a0b7dc7425be5aee6b052c2b50640 upstream-sync
79c1bb3e8677b64739ddfe56cd63573519e16e6b docs
006e81bb36bc2684b3d72dfcb1aaeaec977e60e9 ci
3205f088fa2612ee7f9a008cae0da93a47ab53fc upstream-ports
f491e95215ae6cad16032d45ce6c5b0730f8e140 map-update-fast
d9e29064772dcaed9f827c219ef5b43236ac6d28 thread-priorities
0b303cf3f94bdc4c16476f24ab4c40d6274dd38b per-call-cuts
99b97d167fe634cb246b924a2c66621346c46c78 per-call-cuts
ec86024166614216aafa9fddcfe998cdfec14c6b thread-priorities
3b37acceaa35d6ae4ec797fd6fb3b7f2173a6d34 ci
ec0310fb2470ca9cd8b000a30cea26a13d7afb0e pace-scope
f258e2cf699b57be1371a7440ec9daad2f32a7b5 per-call-cuts
ed0f0aa3884661ab1199d54939f5384f40411093 per-call-cuts
2bdfdf521334c13023cd655d9cfb4058bc40f891 pace-scope
4dd5ece466e53cb8fc79a62fb65bfcf528f2d417 arm64-build
0a257105c057820ea0b23ed1ffddbd7f96459396 map-update-fast
cc14d8645f3652287660a99473fda469b0b59f66 metal4
2e628ee82ae46dd88569f612b7eb0fdeddfd5e7e metal4
64e92eebc4b8cedf4f13c4c3356c634d3dece899 present-pacing
376a1a422221b1a4fd0d95f6813ef63850e55546 commits-and-timestamps
78cf2296b56ea45d58190780bfae881fe440c4e1 teardown-robustness
60bcc9cbe30d2c47e6006593a1d44a0534fa1442 metal4
883f68cfde34dc2b70513cf8ff8e8ecdd2a6360e ci
0335e91a6ffcd3967a3ef30fff3ddbd5297787e9 compile-diagnostics
58a1d48b849959c6b78f7a9a136bccdfdc5be156 gone-present-queue
2483559b9ebe6c4b4e851ddd6df9dc7d603da561 shader-warming
e0e17a7948b504c480e8418dd85ee945dbeef919 per-call-cuts
2edcc1f28f0e34413f7bdbbd752078aaa6bd90d3 per-call-cuts
1dcb12d10f97f3a70e0df701dad78b23e821bc61 stats-and-counters
09a79affb2650b4c28b7e3f2384cd5a8823ace9d arm64-build
215e15a54262a31dba2e3f9d3759343caee27596 ci
adb9afd681c8c91560f7795979392bb0d2f19f05 native-tests
da6f36ac88384427e51ccf97da9a7e0ee33f8fc1 metal4-present
6952de881a6bcf88d345f4d90ad6b67f203e4f72 gone-mtl4-present-tests
bd21e02d463637299d6562e6074c8461f1ce03c5 native-tests
15c9f1fbf7b8877c8c78833d4ea086a1234be131 metal4
fde3dbe11c777cc315ef9fb3dc8a3348c0f286b2 arm64-build
06850671ec7dda38e5ac908dff60d0ed9f1502d7 pace-scope
fd2a772312c052c958d22c71658745f0f79f7cc9 native-tests
b76567388b6171e2ca86855de003354df50b6147 gone-mtl4-present-tests
b2ca4e836dfb002f0c5b7e877ced1e838688a0bb native-tests
fe0beb933dbee9043525b65df83d527db7eb61e2 upstream-sync
c90250a6f74e789a56f8025e4b1185edb0536499 upstream-sync
e03563a2156607a2d8f16ce7b273d2788f4480bf upstream-sync
6bbb36ed008924a6cd352d486efc436f23fc2f0d upstream-sync
96e7c778ba296e2592210c5564c53b70ad42d970 upstream-ports
95919085954a612ddef724e0786625d2d6d605bd upstream-ports
f60b6649b40fd392a8dc0cdd1e0ac9212f5becd8 rename-queues
08fd2d439a5e8f949db1d8b0f6d83e3a9cc7fcd6 retirement-and-submission
6e12dd4191681f0c1f52f5cf29b7a2286efae84b arm64-build
5702b8a9eb7626ba987eb54eda79ee72c1348a33 stats-and-counters
7df014ca6d9cf87228552115c563bdd97c35de09 stats-and-counters
fc15a114713c0b5033515afdd2ad5b99f48acb18 stats-and-counters
8b53fc92f19f001915a09e1f1fdfdd1e79b91699 rename-queues
64eb6824d7b671a0c6ef1b7c796ebaa0b8a2e79f bridge
79e9cea3b6bb155e02e87456df4ce5ed7e61c8a6 metal4-present
34dc411efc35f24ce1eded8f93afc68ec55a1f33 ci
b8cdaa58d4bbc7863b1adb97dfd449881b2f5392 gone-pso-archive
8a02b315d90711d95f0cd003a10ff9cd3f39a77d gone-mtl4-present-tests
8759a529d687a4bf62307a407a3199c997a9130e commits-and-timestamps
4bb399d82d6696ae41ca95a76840400e533b595e gone-present-queue
fe412932fed7e3133fadbd0db25e5839fd72c3a2 metal4-present
707135aac3003107c0dfa1dee7611d4f911e8562 shader-warming
c4577cd9167b15dd35bad905a7dcb67eab7f0c73 map-update-fast
64b3fbbf7d148c3ebb7ba1059f5ef90a18e9b8c3 upstream-footprint
49748bdc5f3a85f8abfc1237b8095588be40d92d upstream-footprint
f231e03aa5ca21a30ee6b65d6a8e46769fc80889 upstream-footprint
19c6e4e0e5ac3992e5638656a80cc991c6936bc1 teardown-robustness
e2ce5ed457b6af3322fd5d2f560546eb3ebec158 teardown-robustness
030fd8472b528e60eb3d2feda2cd0521d287b659 upstream-footprint
5a185fbdc625d6688ec16a59ca60d6b2701f7ac2 metal4
67a9c9ec2a424415ce0a9a94f97aaf69d863bf1b compile-service
9fea15b7bef7004bdf8c5fcc9b06f90f61194497 ci
5f4a03f04f436d475cf92bb2629a396e421dc11c native-tests
43dbef4e73ede009fedb0d3922c8f222e70f6912 ci
e07b2a1ee48a522d2f0523ccf5c5f551463f60ce upstream-footprint
421bc7bcb6c9a1ce90cf8600c95e28119020a64b present-pacing
3479289a07505ac1f9d748bd38809d0cf9648adb teardown-robustness
627a67d942b3ab0119dec949fc5274fb2e275183 docs
0411f4c6534fd050f528941905098c16ec7dcea1 switch-registry
f17a711f2120411f2136f291e40ee0eb94f1fcbf switch-registry
7611df8eed54b2583534f5b8da80f3508f3e9319 present-pacing
d7dbd22b2dab90e1d17a9a29fbceb52ad8053b32 native-tests
73dfb9697fad2829e4ee4e9ae1ce22411f0e2585 ci
940e40e9eb2042056133ad87b1f91c0501af7810 docs
e1e426b341d864de9ed4d82a3ae7257df808bd16 metal4-present
274c4132296fe9992ae0c1c3d91826c82a4eecf7 compile-service
4663a635691e28b74ff6e579c44eddaecf23defd ci
1d385aa83044bb51300680837801dcc3fd52b6ca compile-service
da808169556b0454342da71d1e0ccdc87db0bee0 ci
37c0037edfabbd6c5ed4d2b716c4db4aaf0f573f gone-sparse-buffer-tier
aecb5bda20ff085c6625acbd0a9809447bdb09e1 gone-sparse-buffer-tier
945c2104814f876cfd2fe735a17e354a409f09b0 native-tests
5f5d37f220693c4fe22eb5f7124d23e11c1d8595 ci
9034163ccdfa04eb08895b5e2016f6eca9a558f9 ci
cd6cea30d785311ff848745f9eb6da1698f80447 metal4-present
886d6985a4a6096ddabd332c8b06063f48800dc7 upstream-sync
795f0f95623022045ba1f3077133d7d0e92efd66 upstream-sync
```

```series-rules
docs/* docs
AGENTS.md docs
CLAUDE.md docs
.github/CONTRIBUTING.md docs
.github/ISSUE_TEMPLATE/* docs
.github/PULL_REQUEST_TEMPLATE.md docs
.github/* ci
tests/gw2/* native-tests
*meson.build build-lists
```

```series-order
mode disjoint
ci
build-lists
```

```series-skip
```

```series-drift
fe4748fce6 src/d3d11/d3d11_swapchain.cpp union
64b3fbbf7d src/d3d11/d3d11_swapchain.cpp theirs
```

```series-titles
present-pacing Present pacing: Display sync, a present deadline and frame latency 3, so GW2 reaches 60
background-throttle Background throttle: Present at 30 fps behind another window, and at 10 minimized
depth-compression Depth compression: Compressed depth targets without the smudges in fire and smoke
stats-and-counters Statistics: The STATS, PACE, GPU and HITCH lines, and their counters
pace-scope Pace scope: A timing scope around each D3D11 call that costs 0.02 µs
thread-priorities Thread priorities: Switches for the render, queue and compile threads
pipeline-lookup Pipeline lookup: Hash the blend state, check the last four, and skip a bind the encoder has
chunk-ring Chunk ring: The RING line, and switches for the ring size and the frame latency
compile-diagnostics Compile diagnostics: The COMPILE section, with the encode thread's pipeline waits
commits-and-timestamps Commits: Estimate timestamp queries, and don't flush on a GetData poll
map-update-fast Map and update: A fast path for buffer Map, Unmap and UpdateSubresource
residency-set Residency: One MTLResidencySet per device in place of useResource
retirement-and-submission Retirement: Hold what a chunk uses until it retires, behind one submission seam
metal4 Metal 4: Queues, allocators, encoders and argument tables, on by default
metal4-present Metal 4 present: Present through a Metal 3 command buffer, and wait for it before the first back-buffer write
gone-mtl4-present-tests Metal 4 present tests: The layer set and done tests, and their removal
metalfx-subsample MetalFX: Upscale the Subsample 3D pass and keep the UI native
shader-warming Shader warming: Record each pipeline's recipe, and build the recent ones in a later session
compile-service Compile service: One owner for compile jobs, with declared dependencies and a demand lane
air-readers AIR cache: A pool of read connections
gone-pso-archive PSO archive: Its later versions, and its deletion
bridge Bridge: Call numbers in one manifest, and the unix side linked for tests
switch-registry Switches: A registry that writes the startup line, and Lint holding the table to it
native-tests Native tests: The tests under tests/gw2, and how they run
upstream-ports Ports: Fixes from upstream and athei, with my tests
teardown-robustness Teardown: Pin the DLL, free the presenter, and wait at exit
per-call-cuts Per-call cost: Skip setters that change nothing, and visit only the slots a call can change
rename-queues Rename queues: Count their pages, and trim the ones no Map has reached
arm64-build ARM64X: Build for Mach9's engine, and call the unix side directly
ci CI: The GW2 workflow on GitHub's runners
docs Docs: The fork's rules, ADRs, switch table and this inventory
upstream-footprint Upstream files: Move my code out of upstream's files and off their ends
gone-present-queue Present queue: DXMT_PRESENT_QUEUE, and its removal
gone-sparse-buffer-tier Buffer classes: sparseBufferTier, and its revert
cache-dir Cache folder: DXMT_CACHE_DIR gives a setup its own cache
dependency-compression Dependency compression: Encoder fences on a checkpoint chain on Metal 4
upstream-sync Upstream sync: The sync's steps
build-lists Build lists: The fork's sources in the meson.build lists
```
