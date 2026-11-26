#!/bin/zsh
# gw2 (#111): builds the native PSO archive tool (tools/gw2/pso_archive_tool.cpp) for macOS.
#
#   tools/gw2/build-pso-archive-tool.zsh <output> [arch] [testing]
#
# arch: x86_64 (default, as the game) or arm64. `testing` (tests only) builds the unix side with
# PSO_ARCHIVE_TESTING, so PSO_ARCHIVE_TEST_FAIL and friends fake Metal's packer failures.
#
# It links what the game runs, from this checkout: the bridge's PE side (winemetal_thunks.c) and its
# real unix side (every source in winemetal_unix_src, src/winemetal/unix/meson.build) with
# DXMT_NATIVE (B1), C3's pipeline recipe and builder, and W2/W3's plan. The shader compiler is not
# linked (tests/gw2/shim/airconv_absent.c): the tool only reads AIR the game already translated.
# Objects go next to the output, in <output>.o/.

emulate -L zsh
setopt err_exit pipefail

if (( $# < 1 )); then
  print -u2 "usage: ${0:t} <output> [x86_64|arm64]"
  exit 2
fi
OUT=${1:A}
ARCH=${2:-x86_64}
TESTING=()
[[ ${3:-} == testing ]] && TESTING=(-DPSO_ARCHIVE_TESTING)
ROOT=${0:A:h:h:h}
cd "$ROOT"

O=$OUT.o
mkdir -p "$O" "${OUT:h}"
U=($(sed -n "/^winemetal_unix_src = files(/,/^)/s/^ *'\([a-z0-9_]*\.c\)'.*/\1/p" src/winemetal/unix/meson.build))
(( ${#U} >= 5 )) || { print -u2 "${0:t}: winemetal_unix_src not found"; exit 1 }
MIN=-mmacosx-version-min=26.0
for f in $U; do
  clang -c -x objective-c -std=gnu17 -fblocks -fno-objc-arc -O2 -arch $ARCH $MIN -DDXMT_NATIVE=1 $TESTING -Wno-unused-function \
    -Isrc/airconv -Isrc/nativemetal -Isrc/winemetal src/winemetal/unix/$f -o $O/u_${f:r}.o
done
clang -c -std=gnu17 -O2 -arch $ARCH $MIN -DDXMT_NATIVE=1 -Isrc/nativemetal -Isrc/winemetal \
  src/winemetal/winemetal_thunks.c -o $O/thunks.o
clang -c -std=gnu17 -O2 -arch $ARCH $MIN -Wno-ignored-attributes -Isrc/airconv tests/gw2/shim/airconv_absent.c \
  -o $O/airconv_absent.o
clang -c -O2 -arch $ARCH $MIN src/util/sha1/sha1.c -o $O/sha1.o
clang++ -std=c++20 -O2 -Wall -Wextra -arch $ARCH $MIN -Itests/gw2/shim -Isrc/winemetal -Isrc/winemetal/unix -Isrc/dxmt \
  -Isrc/util -Isrc/airconv \
  tools/gw2/pso_archive_tool.cpp src/dxmt/dxmt_compile_recipe.cpp src/dxmt/dxmt_compile_builder.cpp \
  src/dxmt/dxmt_gw2_warm.cpp src/dxmt/dxmt_gw2_stats.cpp src/dxmt/dxmt_gw2_switches.cpp src/util/sha1/sha1_util.cpp \
  $O/sha1.o $O/thunks.o $O/u_*.o $O/airconv_absent.o \
  -framework Metal -framework MetalFX -framework QuartzCore -framework Foundation -framework CoreGraphics \
  -framework ColorSync -framework Cocoa -lsqlite3 -o "$OUT"
print "built $OUT ($ARCH${TESTING:+, testing})"
