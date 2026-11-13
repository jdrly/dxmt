#!/bin/zsh
# gw2 (X1, architecture council): the fork's native test runner. Not part of the DLL build. The CI
# (.github/workflows/gw2.yml) runs pure and metal on a runner without Metal 4, where the Metal 4 tests
# skip (docs/gw2/README.md, CI). Run it on the Mac from anywhere in the checkout:
#
#   tests/gw2/run.zsh [pure|metal|display|all] [--build-only] [--meson <build dir>]...
#
# A test registers in its own header. Every tests/gw2/*_test.* with these lines is one test:
#
#   // gw2-test: pure          (no Metal device needed) or: metal, or: display (a window on a display in
#                              a GUI session; only by name, never in all)
#   // gw2-build: <command>    zsh, run from the checkout's root; several lines run as one script.
#                              $OUT is the test's output path (a file in the runner's directory; a
#                              test may write $OUT_<suffix> files too), $T that directory
#   // gw2-run: <command>      optional, as gw2-build; default: "$OUT"
#
# A failed build, or a nonzero exit, fails the run. Exit 77 is "skipped" (unsupported hardware or
# OS), and a skipped test is never counted as passed. --build-only builds the selected tests and runs
# none (the CI's check of the display tests). A test without the lines is listed as unregistered.
# Lines of a test's log that start with "RESULT " are printed under its status line.
# --meson <dir> also runs `meson test -C <dir> --suite gw2` (pure: the gw2 suite minus gw2-metal;
# metal: gw2-metal), e.g. B1's bridge-native-smoke in <build>/x64.
#
# The runner's directory is GW2_TEST_DIR, or a new one under $TMPDIR; it's kept for the logs.

emulate -L zsh
setopt pipefail nullglob

ROOT=${0:A:h:h:h}
cd "$ROOT" || exit 2

KIND=all
BUILD_ONLY=0
MESON=()
while (( $# )); do
  case $1 in
    pure|metal|display|all) KIND=$1 ;;
    --meson) shift; MESON+=("$1") ;;
    --build-only) BUILD_ONLY=1 ;;
    -h|--help) sed -n '2,25p' "${0:A}" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) print -u2 "run.zsh: unknown argument $1"; exit 2 ;;
  esac
  shift
done

# macOS's TMPDIR ends in "/": strip it, so no test sees a "//" in its paths
T=${GW2_TEST_DIR:-$(mktemp -d "${${TMPDIR:-/tmp}%/}/gw2-tests.XXXXXX")}
mkdir -p "$T"

typeset -a passed failed skipped built unregistered
for f in tests/gw2/*_test.*; do
  name=${f:t:r}
  kind=$(sed -n 's|^// gw2-test: *\([a-z]*\).*|\1|p' "$f" | head -1)
  build=$(sed -n 's|^// gw2-build: \{0,1\}||p' "$f")
  run=$(sed -n 's|^// gw2-run: \{0,1\}||p' "$f")
  if [[ -z $kind || -z $build ]]; then
    unregistered+=("$name")
    continue
  fi
  [[ $KIND == "$kind" || ( $KIND == all && $kind != display ) ]] || continue
  [[ -n $run ]] || run='"$OUT"'
  OUT=$T/$name
  log=$T/$name.log
  if ! OUT=$OUT T=$T zsh -c "set -e; $build" >"$log" 2>&1; then
    failed+=("$name (build)")
    print "FAIL  $name: build failed ($log)"
    tail -n 15 "$log" | sed 's/^/      /'
    continue
  fi
  if (( BUILD_ONLY )); then
    built+=("$name")
    print "built $name ($kind)"
    continue
  fi
  OUT=$OUT T=$T zsh -c "set -e; $run" >>"$log" 2>&1
  rc=$?
  case $rc in
    0) passed+=("$name"); print "ok    $name ($kind)" ;;
    77) skipped+=("$name"); print "skip  $name ($kind): unsupported here ($log)" ;;
    *) failed+=("$name (exit $rc)"); print "FAIL  $name: exit $rc ($log)"; tail -n 15 "$log" | sed 's/^/      /' ;;
  esac
  grep '^RESULT ' "$log" # a test's one-line results, if it prints them (present_cursor.zsh)
done

for dir in $MESON; do
  if (( BUILD_ONLY )); then
    print "none  meson test -C $dir: --build-only runs no test"
    continue
  fi
  case $KIND in
    pure) args=(--suite gw2 --no-suite gw2-metal) ;;
    metal) args=(--suite gw2-metal) ;;
    all) args=(--suite gw2) ;;
    display) print "none  meson test -C $dir: no display suite"; continue ;;
  esac
  log=$T/meson-${dir:t}.log
  if meson test -C "$dir" $args >"$log" 2>&1; then
    if grep -q '^No suitable tests defined' "$log"; then
      print "none  meson test -C $dir: no test in this selection"
    elif grep -q '^Ok: *[1-9]' "$log"; then
      passed+=("meson ${dir:t}")
      print "ok    meson test -C $dir ($(grep -E '^(Ok|Skipped):' "$log" | tr -s ' ' | tr '\n' ' '))"
    else
      skipped+=("meson ${dir:t}")
      print "skip  meson test -C $dir: nothing passed ($(grep -E '^(Ok|Skipped):' "$log" | tr -s ' ' | tr '\n' ' '))"
    fi
  else
    failed+=("meson ${dir:t}")
    print "FAIL  meson test -C $dir ($log)"
  fi
done

print
print "passed ${#passed}, failed ${#failed}, skipped ${#skipped} (not passed)${built:+, built and not run ${#built}}${unregistered:+, unregistered: ${(j:, :)unregistered}}"
print "logs: $T"
(( ${#failed} == 0 ))
