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
the command line and every header it includes, so a code change can't hit an old object. What it
gets wrong by default is the compiler. It identifies a compiler by the size and time of the file it
runs, and here that file is a wrapper for llvm-mingw, a link into the Cellar for Homebrew's GCC and
the `/usr/bin` shim for Apple's clang. None of them changes when the compiler does. So
`.github/gw2/ccache.zsh` writes the toolchain as text: the runner image and its version, Xcode and
its clang, the macOS floor, the llvm-mingw pin (`LLVM_MINGW_VERSION`), and each cross compiler's
resolved path and version line. A hash of that text is in the `actions/cache` key and, as
`CCACHE_COMPILERCHECK=string:<hash>`, in every object's hash. A new image, Xcode, Homebrew keg or
llvm-mingw pin starts a new cache, and pull requests run cold until the next push saves one.

A push never restores a cache. What `Publish` uploads as `dxmt-arm64-<sha>.tar.xz` is what GW2 on
Mac's dev build installs, and I want each of those archives to be its commit's own compile, with no
object from another run in it. The day's first push (UTC) compiles through an empty ccache and saves
it. A later push that day finds the day's key with a lookup that downloads nothing and compiles with
`CCACHE_DISABLE=1`, as before #305. So push runs take as long as they did. I rejected letting pushes
read the cache for now. It would take a push from about 8.3 to about 4.2 minutes and 19.6 to about
10 runner minutes, but every published archive would then hold cached objects, and I want a few warm
pull request runs with sane statistics before I trust that.

## Consequences

- A pull request's artifacts can hold objects from the cache. I may play such an artifact before the
  merge. The archive the merge publishes is compiled fresh.
- The warnings gate reads `build.log`, and on a hit ccache replays the stderr it stored with the
  object. If it didn't, the gate would pass on an empty log. The first warm run has to print the same
  counts as the cold run, 11 for x86_64 and 1 for i386. Fewer with no source change to explain it
  means the gate is blind, and then pull requests go back to `CCACHE_DISABLE=1` until that's
  understood.
- llvm-mingw's resolved path holds no version, and its clang version line stays the same when a new
  release keeps the LLVM commit. Such a release can still change the flags its wrapper adds, which
  ccache never sees. That is why the pin itself is in the toolchain text. The text is the same for
  every job, so a new pin also starts new caches for the two GCC builds, which don't use llvm-mingw.
  I take one cold run per job for that over a test that could miss llvm-mingw's path.
- A cache is one job's, one toolchain's and one day's. Only pushes to `gw2-on-mac` save, because a
  pull request can read only caches of the default branch and of its own ref. A cache nobody reads
  for 7 days expires. Each is capped at 400 MB by `CCACHE_MAXSIZE`, next to the LLVM builds in the
  same 10 GB quota.
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
  such an object could only reach a pull request's artifact. The variable covers the whole tree, so
  its native compiles run in depend mode too. Those still hit the objects saved without it.
- Letting pushes read the cache is three edits: drop the `if:` of "Restore the compiler cache", delete
  "Look for today's compiler cache" and "Compile without ccache", and save when the restore had no
  exact hit. That would replace this ADR's third paragraph.

Source: jdrly/dxmt #305 and my plan 012 (GW2 on Mac `advisor-plans/2026-11-17-ci`), with its
review's DXMT section.
