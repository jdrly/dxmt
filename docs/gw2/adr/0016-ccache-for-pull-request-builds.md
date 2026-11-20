# Pull requests compile through a ccache that only pushes save, and pushes don't read it

A pull request run in my fork cost 23.4 runner minutes (median of 13 green runs before #305), and
13.3 of them were the three `Build` steps compiling everything from nothing: 261 s for x86_64,
166 s for i386, 373 s for arm64, which also set the run's wall clock. Most pull requests change a
small part of the tree. For the 14 merged on 2026-11-16 and 17, the sources whose includes reach a
changed file were none to 67% of the 146 the build compiles, median 30%. So the three builds now
compile through ccache, and a pull request reads the newest cache a push to `gw2-on-mac` saved for
the same toolchain. I estimated 14 to 20 runner minutes and about 5.5 minutes of wall clock for such
a run, from the logs' phases and an include graph. The first warm run gives the real numbers.

A stale hit would put an object compiled by another compiler into a DLL. ccache hashes the source,
the command line and the headers it recorded at the compile, so a change to any of them can't hit an
old object (two cases where that list falls short are below). What it
gets wrong by default is the compiler. It identifies a compiler by the size and time of the file it
runs, and here that file is a wrapper for llvm-mingw, a link into the Cellar for Homebrew's GCC and
the `/usr/bin` shim for Apple's clang. None of them changes when the compiler does. So
`.github/gw2/ccache.zsh` writes the toolchain as text: Xcode and its clang, the macOS floor, the
llvm-mingw pin (`LLVM_MINGW_VERSION`), and each cross compiler's resolved path and version line. A
hash of that text is in the `actions/cache` key and, as `CCACHE_COMPILERCHECK=string:<hash>`, in
every object's hash. A new Xcode, Homebrew keg or llvm-mingw pin starts a new cache, and the first
run after it compiles cold. Until #346 the text also held the runner image's version, and a new
image started a new cache with every compiler line unchanged: in run 44614350394 the i386 and arm64
jobs landed on image `20261119.0378.1`, their caches were saved on `20261104.0337.1`, and both found
nothing, beside x86_64 at 171 hits of 180. The compilers an image brings are in the text by their
version lines, so its version added only misses.

A push never restores a cache. What `Publish` uploads as `dxmt-arm64-<sha>.tar.xz` is what GW2 on
Mac's dev build installs, and each of those archives has to be its commit's own compile. Every push
to `gw2-on-mac` compiles through an empty ccache and saves it under its commit (#346, 2026-11-19).
Until then only the day's first push (UTC) saved, and a later push that day compiled with
`CCACHE_DISABLE=1`. That save ran twice in the fork's history, on 2026-11-17 03:12 and 2026-11-19
20:14, so pull requests compiled against a tree up to two days old. Each merged header change added
misses (2, then 32, 40, 43), and a pull request with only ADRs recompiled 42 files per tree
(44101649506): 97 to 168 s for x86_64's Build step, where a fresh cache takes 30 to 37 s. Now a pull
request run after a push misses only what it changed. Push runs take about as long as they did.

I keep pushes off the cache because a hit doesn't prove the object is what a fresh compile would
make. ccache hits in direct mode, and the ARM64X tree in depend mode. Both trust a list of headers
recorded at an earlier compile. The ARM64X tree's list holds the ARM64EC half's headers only, so a
change to a header that only the AArch64 half includes can hit a stale object (the depend-mode
consequence below). And a new header that shadows one on the include path, for example a
`src/d3d11/dxmt_format.hpp` beside the `src/dxmt/dxmt_format.hpp` that `d3d11_device.cpp` includes
today, changes what a fresh compile reads while the recorded header and the source stay the same.
A published build can't risk either. On 2026-11-17 I estimated that reading the cache would take a
push run from about 8.3 to about 4.2 minutes.

A pull request's restore tries its key, its head commit, first. No push saves that key, so the
restore takes the newest cache of its job and toolchain by prefix. Only pushes save a compiler
cache. GitHub scopes a pull request's caches to its own ref, which the default branch never reads,
and no pull request step saves one. A rerun of a push finds its commit's key taken:
`actions/cache/save` logs that and keeps the first, and the step passes.

## Consequences

- A pull request's artifacts can hold objects from the cache, stale ones included in the two cases
  above. I may play such an artifact before the merge. The archive the merge publishes is compiled
  fresh.
- The warnings gate reads `build.log`, and on a hit ccache replays the stderr it stored with the
  object. If it didn't, the gate would pass on an empty log. A warm run has to print the same count
  as a cold run, 11 for x86_64 (i386's gate runs on pushes, which are cold). Fewer with no source
  change to explain it means the gate is blind, and then pull requests go back to
  `CCACHE_DISABLE=1` until that's understood.
- llvm-mingw's resolved path holds no version, and its clang version line stays the same when a new
  release keeps the LLVM commit. Such a release can still change the flags its wrapper adds, which
  ccache never sees. That is why the pin itself is in the toolchain text. The text is the same for
  every job, so a new pin also starts new caches for the two GCC builds, which don't use llvm-mingw.
  I take one cold run per job for that over a test that could miss llvm-mingw's path.
- A cache is one job's, one toolchain's and one commit's, and a pull request restores the newest of
  its job and toolchain by prefix. Only that one is read, and a cache nobody reads for 7 days
  expires, so the older ones age out by themselves. On 2026-11-19 x86_64's was 8 MB and arm64's 22
  MB, about 30 MB a push. Each is capped at 400 MB by `CCACHE_MAXSIZE`, next to the LLVM builds in
  the same 10 GB quota. Over the quota GitHub deletes the caches read longest ago first, and every
  run reads the LLVM builds.
- `Build i386` runs on pushes only (#347), so no pull request reads its cache, and it saves none.
  It still compiles through an empty ccache, like the other push builds.
- A new meson tree or cross file in a build job has to be named in that job's `ccache.zsh setup`
  call, or its compilers run without ccache. Nothing fails. The statistics show it.
- Each build job prints ccache's statistics for its own run (`CCACHE_STATSLOG`, since a restored cache
  carries the counters of the push that saved it) and the runner's core count into its summary.
- 2026-11-17 (#307): the ARM64X tree (`build-arm64`) compiles in ccache's depend mode. Its cross
  compiles pass `-marm64x`, and clang refuses that option together with `-E` ("unsupported option
  '-marm64x' for target 'arm64ec-w64-mingw32'"). So ccache couldn't run the preprocessor on them,
  counted all 106 as "Preprocessing failed" and cached none, while the i386 tree and the native
  compiles in the same job cached fine. Depend mode never preprocesses. It hashes the headers the
  compile's `-MD` file names. With `-marm64x` clang compiles the source twice, AArch64 then ARM64EC,
  and both write the same `-MD` file, so it lists the ARM64EC half's headers only. A header included
  only under `__aarch64__` would go unhashed, and a change to it could hit a stale native half. The
  architecture checks in `src` guard macros and declarations, never an `#include`, and for the four
  sources I compared (`d3d11_device.cpp`, `dxmt_texture.cpp`, `d3d12_descriptor_heap.cpp`,
  `util_md5.cpp`) the AArch64 list is a subset of the ARM64EC list. Since a push never reads a cache,
  such an object could only reach a pull request's artifact. The variable covers the whole tree,
  so its native compiles run in depend mode too. Those still hit the objects saved without it.
- Letting pushes read the cache is one edit, dropping the `if:` of "Restore the compiler cache", and
  the save would then skip on an exact hit. It needs an answer to the two stale cases above first,
  such as preprocessor mode for every tree that allows it and a fresh compile of the ARM64X tree.

Source: jdrly/dxmt #305 and my plan 012 (GW2 on Mac `advisor-plans/2026-11-17-ci`), with its
review's DXMT section. jdrly/dxmt #346 and recommendation 7 of my CI audit (GW2 on Mac
`advisor-plans/2026-11-19-ci-audit`).
