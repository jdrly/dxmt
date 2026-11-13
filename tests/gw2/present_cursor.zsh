#!/bin/zsh
# gw2 (#230, for #223): one run of present_cursor_test with a WindowServer sample beside it, ending in
# one RESULT line to send back. On the Mac, from a terminal in the GUI session, game closed:
#
#   tests/gw2/present_cursor.zsh [--arch arm64|x86_64] [--bundle] [--no-sample] [--dir D] <test arguments>
#
# e.g. tests/gw2/present_cursor.zsh --present mtl4 --pointer wave
#      MTL_HUD_ENABLED=1 tests/gw2/present_cursor.zsh --present mtl4 --present-at done --pointer wave
#
# It builds the test into D (GW2_PRESENT_DIR, or $TMPDIR/gw2-present-cursor) with `run.zsh display
# --build-only` when the binary is missing or older than the source. --bundle starts the arm64 binary
# from its Game Mode bundle. Unless --no-sample, it asks for sudo first, runs the test for 1800 frames
# (unless --frames is given) so that `sudo sample WindowServer 10 10`, started at the test's second
# t=2 line, ends inside the run, and counts the sample with ws_sample_count.py. The test's output goes
# to the terminal and, with the sample, to D/runs/.
#
# The RESULT line: date, Mac model and macOS, the process (arch, bundle), MTL_HUD_ENABLED and
# MTL_DEBUG_LAYER, then passwait and early first (my Mac's state: Metal 3's passwait 13 to 17 ms with
# early 0, Metal 4's under 2 ms; on the rented M1 both were 0.5 ms), the window server's frame-info
# thread (quiet: 2 or 3 in 600; busy: 11 or more), the interval and missed refreshes, the layer's path,
# the display, the test's start line (every variant in effect), the rest of the summary, the exit code.
# Exit: the test's.

emulate -L zsh
setopt pipefail

ROOT=${0:A:h:h:h}
arch=arm64 bundle=0 sample=1
dir=${GW2_PRESENT_DIR:-${${TMPDIR:-/tmp}%/}/gw2-present-cursor}
args=()
while (( $# )); do
  case $1 in
    --arch) arch=$2; shift ;;
    --bundle) bundle=1 ;;
    --no-sample) sample=0 ;;
    --dir) dir=$2; shift ;;
    -h|--help) sed -n '2,22p' "${0:A}" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) args+=("$1") ;;
  esac
  shift
done
[[ $arch == (arm64|x86_64) ]] || { print -u2 "present_cursor.zsh: --arch arm64 or x86_64"; exit 2 }
(( bundle )) && [[ $arch == x86_64 ]] && { print -u2 "present_cursor.zsh: the bundle holds the arm64 binary"; exit 2 }

bin=$dir/present_cursor_test_$arch
(( bundle )) && bin=$dir/present_cursor_test.app/Contents/MacOS/present_cursor_test
if [[ ! -x $bin || $ROOT/tests/gw2/present_cursor_test.m -nt $bin ]]; then
  print "building present_cursor_test into $dir"
  GW2_TEST_DIR=$dir "$ROOT/tests/gw2/run.zsh" display --build-only >/dev/null ||
    { print -u2 "present_cursor.zsh: the build failed ($dir/present_cursor_test.log)"; exit 1 }
fi

mkdir -p "$dir/runs"
name=$(date +%Y%m%d-%H%M%S)-$arch${${bundle:#0}:+-bundle}-${(j:_:)${args//--/}}
name=${name//\//}
out=$dir/runs/$name.out ws=$dir/runs/$name.ws.txt
(( ${args[(I)--frames]} || ! sample )) || args+=(--frames 1800)
: >"$out"

sampler=0
if (( sample )); then
  sudo -v || { print -u2 "present_cursor.zsh: sample needs sudo (or --no-sample)"; exit 2 }
  {
    # past the calibration and the first second, unless the test ends first
    for i in {1..900}; do
      grep -q '^t=2 ' "$out" && break
      [[ -e $out.done ]] && exit 0
      sleep 0.2
    done
    grep -q '^t=2 ' "$out" || exit 0
    sudo -n sample WindowServer 10 10 -file "$ws" >"$ws.log" 2>&1
  } &
  sampler=$!
fi

"$bin" "${args[@]}" 2>&1 | tee "$out"
rc=${pipestatus[1]}
touch "$out.done"
overran=0
if (( sampler )); then
  kill -0 $sampler 2>/dev/null && overran=1
  wait $sampler
fi
rm -f "$out.done"

# the RESULT line, from the test's own lines
line() { grep -m1 "^$1" "$out" }
start=$(line 'present_cursor_test: ') summary=$(line 'summary [^:]') paths=$(line 'summary: ')
display=$(line 'display: ') proc=$(line 'process: ')
r=(RESULT "$(date '+%Y-%m-%d %H:%M')")
r+=("$(sysctl -n hw.model 2>/dev/null) macOS $(sw_vers -productVersion 2>/dev/null)")
r+=("${${proc#process: }:-$arch}")
(( bundle )) && r[-1]+=" from the bundle"
r[-1]+=" MTL_HUD_ENABLED=${MTL_HUD_ENABLED:-unset} MTL_DEBUG_LAYER=${MTL_DEBUG_LAYER:-unset}"
if [[ $summary =~ 'passwait p50=([^ ]+) max=([^ ]+) early=([^ ]+)' ]]; then
  r+=("passwait p50=$match[1] max=$match[2] early=$match[3]")
  rest=${summary/$MATCH/}
else
  r+=("no summary: $(grep -v '^ *$' "$out" | tail -n 1)")
  rest=
fi
if (( ! sample )); then
  r+=("ws=none")
elif [[ -s $ws ]]; then
  r+=("$(python3 -I "$ROOT/tests/gw2/ws_sample_count.py" "$ws" 2>&1)")
  (( overran )) && r[-1]+=" (the sample outlived the run)"
else
  r+=("ws=no sample ($(tail -n 1 "$ws.log" 2>/dev/null || print 'not started'))")
fi
[[ $rest =~ 'frames=[0-9]+ dropped=[0-9]+ interval p50=[^ ]+ p95=[^ ]+ missed=[0-9]+' ]] && {
  r+=("$MATCH")
  rest=${rest/$MATCH/}
}
[[ $paths =~ '([0-9]+) seconds direct, ([0-9]+) composited, ([0-9]+) unknown' ]] &&
  r+=("path direct=$match[1] composited=$match[2] unknown=$match[3]")
[[ $display =~ 'mode ([0-9]+x[0-9]+) pixels at ([0-9.]+) Hz' ]] && r+=("display $match[1]@$match[2]")
r+=("${start#present_cursor_test: }")
[[ -n $rest ]] && r+=("${(j: :)${=${rest#summary }}}")
r+=("exit $rc")
print -r -- "${(j: | :)r}"
(( sample )) && print "files: $out $ws"
exit $rc
