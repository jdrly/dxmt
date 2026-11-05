#!/bin/zsh
# gw2 (#111): the native PSO archive tool (tools/gw2/pso_archive_tool.cpp) end to end, across
# processes, on real AIR. Run by tests/gw2/pso_archive_tool_test.mm:
#
#   pso_archive_tool.zsh <recipe generator> <tool x86_64> <tool arm64> <tool built "testing"> <work dir>
#
# The AIR is a copy, made here, of an isolated copy of DXMT's AIR cache: $GW2_AIR_DB, else lane C's
# work/dxmt-squeeze/native/w0/sample.db in the GW2 on Mac workspace. Without it the test is skipped
# (77). The real caches under $(getconf DARWIN_USER_CACHE_DIR)dxmt/ are never opened: every path
# below is in the work dir.
#
# Checks: the tool publishes a generation with every synthetic recipe (full VS/PS, blending, sparse
# slots, MSAA, fragment-less) and a fresh process hits each of them, on x86_64 (as the game) and
# arm64; `status` reads the store; a held writer lock (a running game) makes `run` refuse and leaves
# the store as it was; a second run publishes a newer generation that still hits everything; an
# injected crash at one recipe is bisected in probe processes, that recipe left out and the rest
# published and hit; six named packer failures (faked, more than one flush's three attempts) exclude
# six shapes over several flushes and publish the rest (#112 review). Before all that, and without
# AIR or Metal (#136): an empty or relative DXMT_CACHE_DIR is warned about and the defaults stay under
# the user cache dir, an absolute one is the base, and an overlong one (no base) exits 1 when a default
# needs it instead of using paths relative to the working directory. Those runs of `status` only stat
# a recipes file of an exe name no game has.

emulate -L zsh
setopt pipefail
G=$1 TX=$2 TA=$3 TT=$4 W=$5
[[ -x $G && -x $TX && -x $TA && -x $TT && -n $W ]] ||
  { print -u2 "usage: $0 <generator> <tool x86_64> <tool arm64> <tool testing> <dir>"; exit 2 }
fails=0
fail() { print "FAIL $*"; fails=$((fails + 1)) }
# expect <file> <extended regex>...: each must match a line of the file
expect() {
  local f=$1 e
  shift
  for e in "$@"; do
    grep -Eq -- "$e" $f || fail "${f:t}: no $e"
  done
}

# 0. DXMT_CACHE_DIR (#136), in a scratch dir of its own (the work dir is recreated below)
B=$W.base
rm -rf $B && mkdir -p $B || exit 2
X=pso-archive-tool-test.exe
U=$(getconf DARWIN_USER_CACHE_DIR)
[[ $U == /* ]] || { print "FAIL no user cache dir"; exit 1 }
warned='DXMT_CACHE_DIR="(rel/cache)?" isn.t an absolute unix path; ignored'
for v in rel/cache ''; do
  DXMT_CACHE_DIR=$v $TX status --archive-dir $B/store --exe $X >$B/rel.out 2>$B/rel.err ||
    fail "status with DXMT_CACHE_DIR=\"$v\" exited $?"
  expect $B/rel.err "^pso_archive_tool: Caches: $warned"
  [[ $(grep -c 'DXMT_CACHE_DIR' $B/rel.err) == 1 ]] || fail "DXMT_CACHE_DIR=\"$v\": not one warning"
  grep -Fxq "recipes: ${U}dxmt/$X/recipes_1.db (missing)" $B/rel.out ||
    fail "DXMT_CACHE_DIR=\"$v\": the recipes aren't under the user cache dir: $(grep '^recipes' $B/rel.out)"
done
DXMT_CACHE_DIR=$B/abs $TX status --exe $X >$B/abs.out 2>$B/abs.err || fail "status with an absolute DXMT_CACHE_DIR exited $?"
grep -q 'DXMT_CACHE_DIR' $B/abs.err && fail "an absolute DXMT_CACHE_DIR was warned about"
grep -Fxq "store base: $B/abs/dxmt/pso" $B/abs.out || fail "the store base isn't under DXMT_CACHE_DIR"
grep -Fxq "recipes: $B/abs/dxmt/$X/recipes_1.db (missing)" $B/abs.out || fail "the recipes aren't under DXMT_CACHE_DIR"
long=/${(l:1100::x:)}
for cmd in status run; do
  DXMT_CACHE_DIR=$long $TX $cmd --exe $X >$B/long.out 2>$B/long.err
  rc=$?
  (( rc == 1 )) || fail "$cmd with an overlong DXMT_CACHE_DIR exited $rc, not 1"
  expect $B/long.err "DXMT_CACHE_DIR is longer than PATH_MAX"
  grep -q '^store base: dxmt/' $B/long.out && fail "$cmd with an overlong DXMT_CACHE_DIR used a relative store base"
done
DXMT_CACHE_DIR=$long $TX status --archive-dir $B/store --recipes $B/r.db >$B/long_ok.out 2>&1 ||
  fail "status with an overlong DXMT_CACHE_DIR and every path given exited $?"
rm -rf $B

AIR=${GW2_AIR_DB:-$HOME/Development/personal/gw2-on-mac/work/dxmt-squeeze/native/w0/sample.db}
if [[ ! -f $AIR ]]; then
  print "SKIP: no isolated AIR copy at $AIR (set GW2_AIR_DB)"
  (( fails )) && { print "pso_archive_tool: $fails failures before the skip"; exit 1 }
  exit 77
fi
rm -rf $W && mkdir -p $W || exit 2
cp $AIR $W/air.db || exit 2

$G $W/air.db $W/recipes_1.db 8 >$W/gen.out 2>&1
rc=$?
(( rc == 77 )) && { cat $W/gen.out; exit 77 }
(( rc == 0 )) || { cat $W/gen.out; print "FAIL the generator exited $rc"; exit 1 }
N=$(sed -n 's/^RECIPES \([0-9]*\).*/\1/p' $W/gen.out)
print -- "$(grep '^RECIPES' $W/gen.out)"
(( N >= 10 )) || { print "FAIL only ${N:-0} synthetic recipes"; exit 1 }
for shape in full blend sparse msaa novs; do
  grep -q "^RECIPES.* $shape=[1-9]" $W/gen.out || fail "no $shape recipe built on this device"
done

S=$W/store
A=(--recipes $W/recipes_1.db --air $W/air.db --archive-dir $S --exe Gw2-64.exe --metal-version 320)

# 1. publish every recipe; the run's own fresh-process verify hits each one
$TX run $A >$W/run1.out 2>$W/run1.err
rc=$?
(( rc == 0 )) || fail "run 1 exited $rc"
expect $W/run1.out "^RESULT recipes=$N selected=$N built=$N failed=0 nofunction=0 airmiss=0 " \
  " published=$N flush=1 mode=writer" "^VERIFY .* built=$N .* hits=$N misses=0 "
expect $W/run1.err "published generation [0-9]+: $N pipelines"
gen1=$(sed -n 's/^RESULT.* generation=\([0-9]*\).*/\1/p' $W/run1.out)

# 2. fresh processes, both architectures: every pipeline is a lookup hit, nothing published
for arch in x86_64 arm64; do
  T=$TX; [[ $arch == arm64 ]] && T=$TA
  $T verify $A --quiet >$W/verify_$arch.out 2>$W/verify_$arch.err || fail "verify $arch exited $?"
  expect $W/verify_$arch.out "^RESULT .* built=$N .* hits=$N misses=0 .* generation=$gen1 published=0 flush=0"
done

# 3. status reads the store without writing it
$TX status $A >$W/status.out 2>&1 || fail "status exited $?"
expect $W/status.out "writer lock: free" "generation $gen1: 1 shards, $N pipeline keys"

# 4. a running game holds the writer lock: run refuses, the store stays as it was
lock=($S/Gw2-64.exe/os-*/gen1/LOCK(N))
if (( ${#lock} != 1 )); then
  fail "no LOCK in the store"
else
  cur_before=$(cat ${lock[1]:h}/CURRENT)
  perl -MFcntl=:flock -e 'open(my $f, "<", $ARGV[0]) or die; flock($f, LOCK_EX) or die; print "held\n"; $| = 1; sleep 60' \
    ${lock[1]} >$W/holder.out &
  holder=$!
  for i in {1..50}; do grep -q held $W/holder.out 2>/dev/null && break; sleep 0.1; done
  $TX run $A >$W/busy.out 2>$W/busy.err
  rc=$?
  (( rc == 3 )) || fail "run under a held lock exited $rc, not 3"
  expect $W/busy.err "store is busy"
  $TX status $A >$W/status_busy.out 2>&1
  expect $W/status_busy.out "writer lock: held"
  kill $holder 2>/dev/null
  wait $holder 2>/dev/null
  [[ $(cat ${lock[1]:h}/CURRENT) == "$cur_before" ]] || fail "the busy run changed CURRENT"
fi

# 5. a second run (another session) publishes a newer generation that still hits everything
$TX run $A >$W/run2.out 2>$W/run2.err || fail "run 2 exited $?"
gen2=$(sed -n 's/^RESULT.* generation=\([0-9]*\).*/\1/p' $W/run2.out)
(( gen2 > gen1 )) || fail "run 2 published generation ${gen2:-none}, not newer than $gen1"
expect $W/run2.out "^RESULT .* hits=$N misses=0 " "^VERIFY .* hits=$N misses=0 "

# 6. a crash at recipe 3: bisected in probes on scratch stores, left out, the rest published and hit
S2=$W/store2
A2=(--recipes $W/recipes_1.db --air $W/air.db --archive-dir $S2 --exe Gw2-64.exe --metal-version 320)
PSO_ARCHIVE_TOOL_TEST_CRASH=3 $TX run $A2 >$W/crash.out 2>$W/crash.err
rc=$?
(( rc == 0 )) || fail "the crash run exited $rc"
M=$((N - 1))
expect $W/crash.out "^ISOLATED 3 exit=134" "^RESULT recipes=$N selected=$M built=$M failed=0 .* published=$M flush=1" \
  "^VERIFY .* built=$M .* hits=$M misses=0 " "^LEFTOUT 1"
expect $W/crash.err "bisecting $N recipes" "recipe 3 fails alone"
[[ -z $(ls $W/store2/Gw2-64.exe/os-*/gen1 2>/dev/null | grep -v -E '^(CURRENT|LOCK|g[0-9a-f]{16}\.manifest|[0-9a-f]{64}\.shard)$') ]] ||
  fail "leftover files in the store after the crash run"

# 7. six named packer failures (faked: each names the archive's pipeline no. 2): one flush tries three
#    writes, so the tool must flush again while failures name new shapes; six shapes are excluded and
#    the rest published; a fresh process hits the rest and classifies the six as excluded misses
S3=$W/store3
A3=(--recipes $W/recipes_1.db --air $W/air.db --archive-dir $S3 --exe Gw2-64.exe --metal-version 320)
K=$((N - 6))
PSO_ARCHIVE_TEST_FAIL=6 PSO_ARCHIVE_TEST_INDEX=2 $TT run $A3 >$W/named.out 2>$W/named.err
rc=$?
(( rc == 0 )) || fail "the named-failure run exited $rc"
expect $W/named.out "^RESULT recipes=$N selected=$N built=$N .* skipped=6 generation=1 published=$K flush=1" \
  "^VERIFY .* built=$N .* hits=$K misses=6 .* missexcl=6 "
expect $W/named.err "published generation 1: $K pipelines .* 6 excluded shapes"
grep -q '^ISOLATED' $W/named.out && fail "named failures were bisected instead of excluded"
$TX status $A3 >$W/status_named.out 2>&1
expect $W/status_named.out "generation 1: 1 shards, $K pipeline keys, .* 6 excluded shapes"

if (( fails )); then
  print "pso_archive_tool: $fails failures (logs in $W)"
  exit 1
fi
print "pso_archive_tool: ok ($N recipes, generations $gen1 -> $gen2, crash isolated)"
