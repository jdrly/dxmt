#!/bin/zsh
# gw2 (#162): the toolchains of my CI (.github/workflows/gw2.yml). The workflow caches each one
# under its version, so this runs only on a cache miss.
#
#   toolchain.zsh fetch <url> <sha256> <dir> [<tar argument>...]
#       download <url>, check its SHA-256 and unpack it into <dir>
#   toolchain.zsh llvm <arch> <prefix>
#       build LLVM $LLVM_VERSION (source checked against $LLVM_SHA256) for <arch> macOS
#       $MACOSX_DEPLOYMENT_TARGET and install it into <prefix>
#
# The LLVM is the one scripts/build-dxmt.zsh (GW2 on Mac) links airconv with: static libraries
# only, no tools, no targets, no zlib, zstd, libxml2 or terminfo, assertions off.
emulate -L zsh
setopt err_exit pipe_fail no_unset

fetch() {
  local url=$1 sha=$2 dir=$3 file=$RUNNER_TEMP/${1:t}
  shift 3
  curl -fsSL --retry 3 -o "$file" "$url"
  print -r -- "$sha  $file" | shasum -a 256 -c
  mkdir -p "$dir"
  tar -xf "$file" -C "$dir" "$@"
  rm "$file"
}

llvm() {
  local arch=$1 prefix=${2:A} name=llvm-project-$LLVM_VERSION.src
  fetch "https://github.com/llvm/llvm-project/releases/download/llvmorg-$LLVM_VERSION/$name.tar.xz" \
    "$LLVM_SHA256" "$RUNNER_TEMP" "$name/llvm" "$name/cmake"
  cmake -G Ninja -S "$RUNNER_TEMP/$name/llvm" -B "$RUNNER_TEMP/llvm-build-$arch" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$prefix" \
    -DCMAKE_OSX_ARCHITECTURES="$arch" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$MACOSX_DEPLOYMENT_TARGET" \
    -DCMAKE_CXX_FLAGS=-D_LIBCPP_KEEP_TRANSITIVE_INCLUDES_LLVM23 \
    -DLLVM_HOST_TRIPLE="$arch-apple-darwin" \
    -DLLVM_TARGETS_TO_BUILD= \
    -DLLVM_ENABLE_ASSERTIONS=Off \
    -DLLVM_ENABLE_ZLIB=Off \
    -DLLVM_ENABLE_ZSTD=Off \
    -DLLVM_ENABLE_LIBXML2=Off \
    -DLLVM_ENABLE_TERMINFO=Off \
    -DLLVM_BUILD_TOOLS=Off \
    -DLLVM_INCLUDE_BENCHMARKS=Off \
    -DLLVM_INCLUDE_EXAMPLES=Off \
    -DLLVM_INCLUDE_TESTS=Off
  cmake --build "$RUNNER_TEMP/llvm-build-$arch"
  cmake --install "$RUNNER_TEMP/llvm-build-$arch"
}

case ${1:-} in
  fetch | llvm) "$@" ;;
  *) print -u2 "usage: toolchain.zsh fetch <url> <sha256> <dir> [<tar argument>...] | llvm <arch> <prefix>"; exit 2 ;;
esac
