#!/bin/zsh
# gw2 (#305): ccache for the three builds of my CI (.github/workflows/gw2.yml), so a pull request
# compiles only what it changed.
#
#   ccache.zsh setup <name> <cross file>...
#       put ccache in front of the C and C++ compilers the cross files name (a directory of links,
#       $CCACHE_LINKS, which the Build step puts ahead of PATH), write the cache's settings to
#       $GITHUB_ENV, ccache's directory to $GITHUB_PATH and the cache's keys to $GITHUB_OUTPUT
#       (prefix, key)
#   ccache.zsh stats
#       print the runner's core count, this run's hits and misses (ccache's statistics log,
#       $CCACHE_STATSLOG: the counters inside a restored cache are those of the push that saved it)
#       and the cache's size. A Build step with two trees gives each its own log, named
#       <$CCACHE_STATSLOG without .log>-<tree>.log (#307), and each log is printed under its name
#
# A cache is valid for one toolchain: Xcode and its clang, the macOS floor, the llvm-mingw pin and
# the cross compilers, each by its resolved path and version line. The runner image's version isn't
# in it (#346): a new image with the same compilers keeps the cache. That text is
# in the cache's key and, as CCACHE_COMPILERCHECK, in every object's hash. ccache's own check is the
# size and time of the file it runs. That is a wrapper script for llvm-mingw, a link into the Cellar
# for Homebrew's GCC and the /usr/bin shim for Apple's clang, so it would see none of them change.
# llvm-mingw's path holds no version, and a new pin can keep its clang and change the flags its
# wrapper adds, so the pin is in the text. It is in every job's text, so a new pin also starts new
# caches for the two GCC builds.
emulate -L zsh
setopt err_exit pipe_fail no_unset

ccache=$PWD/toolchains/ccache/ccache

setup() {
  local name=$1 links=$RUNNER_TEMP/ccache-links cc id
  shift
  local -a compilers
  compilers=(${(f)"$(sed -n -E "s/^(c|cpp) = '([^']+)'.*/\2/p" "$@" | LC_ALL=C sort -u)"})
  if (( ${#compilers} < 2 )); then
    print -u2 "ccache.zsh: no c and cpp compiler in $*"
    exit 1
  fi
  for cc in $compilers; do
    if (( ! $+commands[$cc] )); then
      print -u2 "ccache.zsh: $cc is not on PATH"
      exit 1
    fi
  done
  "$ccache" --version | head -1
  {
    xcodebuild -version | tr '\n' ' '
    print
    print -r -- "cc: $(/usr/bin/cc --version | head -1)"
    print -r -- "floor ${MACOSX_DEPLOYMENT_TARGET:-?}"
    print -r -- "llvm-mingw ${LLVM_MINGW_VERSION:-?}"
    for cc in $compilers; do print -r -- "$cc -> ${commands[$cc]:A}: $($cc --version | head -1)"; done
  } >"$RUNNER_TEMP/ccache-toolchain.txt"
  cat "$RUNNER_TEMP/ccache-toolchain.txt"
  id=$(shasum -a 256 <"$RUNNER_TEMP/ccache-toolchain.txt" | cut -c1-16)
  rm -rf "$links"
  mkdir -p "$links"
  for cc in $compilers; do ln -s "$ccache" "$links/$cc"; done
  {
    print -r -- "CCACHE_DIR=$RUNNER_TEMP/ccache"
    print -r -- "CCACHE_MAXSIZE=400M"
    print -r -- "CCACHE_COMPILERCHECK=string:$id"
    print -r -- "CCACHE_STATSLOG=$RUNNER_TEMP/ccache-stats.log"
    print -r -- "CCACHE_LINKS=$links"
  } >>"$GITHUB_ENV"
  # meson finds ccache on PATH by itself for the build machine's cc and c++ (airconv, winemetal.so)
  print -r -- "${ccache:h}" >>"$GITHUB_PATH"
  # one cache per commit (#346): every push saves one, and a run restores the newest by prefix
  {
    print -r -- "prefix=ccache-$name-$id-"
    print -r -- "key=ccache-$name-$id-$COMMIT"
  } >>"$GITHUB_OUTPUT"
  print -r -- "cache key: ccache-$name-$id-$COMMIT"
}

stats() {
  local log
  print -r -- "cores: $(sysctl -n hw.ncpu)"
  for log in ${CCACHE_STATSLOG%.log}*.log(N); do
    print -r -- "${log:t}:"
    CCACHE_STATSLOG=$log "$ccache" --show-log-stats --verbose
  done
  du -sh "$CCACHE_DIR"
}

case ${1:-} in
  setup | stats) "$@" ;;
  *)
    print -u2 "usage: ccache.zsh setup <name> <cross file>... | stats"
    exit 2
    ;;
esac
