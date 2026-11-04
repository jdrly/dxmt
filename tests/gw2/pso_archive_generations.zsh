#!/bin/zsh
# gw2 (#75): the PSO archive's generation publisher (DXMT_PSO_GENERATIONS=1) through the real Metal
# adapter, across processes. Run by tests/gw2/pso_archive_test.m:
#
#   pso_archive_generations.zsh <new driver> <work dir>
#
# Checks each session's report and counters for what the publisher must do: publish a delta shard per
# flush without Metal's re-serialization failure, serve the next session's lookups, exclude a failing
# shape by its key (now and in later sessions), fall back to the last good generation, keep one
# writer, survive a kill, and merge a session's shards.

emulate -L zsh
setopt pipefail
N=$1 W=$2
[[ -x $N && -n $W ]] || { print -u2 "usage: $0 <driver> <dir>"; exit 2 }
rm -rf $W && mkdir -p $W || exit 2
export PSO_SESSION_GENERATIONS=1
fails=0
step=0

# session <expect...> -- [VAR=value...] <script words>: each expect is an extended regex the output
# must match; a leading ! means it must not
session() {
  local expects=() envs=()
  while [[ $1 != -- ]]; do expects+=($1); shift; done
  shift
  while [[ $1 == *=* ]]; do envs+=($1); shift; done
  step=$((step + 1))
  local out=$W/session$step.out
  env $envs $N $W "$@" >$out 2>&1
  local e bad=0
  for e in $expects; do
    if [[ $e == '!'* ]]; then
      grep -Eq -- "${e#!}" $out && { print "FAIL session $step ($*): matched ${e#!}"; bad=1 }
    else
      grep -Eq -- "$e" $out || { print "FAIL session $step ($*): no ${e}"; bad=1 }
    fi
  done
  if (( bad )); then
    sed 's/^/      /' $out | head -30
    fails=$((fails + 1))
  else
    print "ok: session $step ($*)"
  fi
}

# 1. two flushes: two delta shards, no packer failure (the legacy mode fails here and drops a pipeline)
session 'published generation 1: 12 pipelines' 'published generation 2: 4 pipelines' '!publish of generation|Metal names' \
  'skipped=0' 'generation=2 published=4' -- b0-11 f b12-15 f q
G=($W/session.exe/*/gen1(N))
G=$G[1]
# 2. the next session hits every pipeline the first published
session 'loaded shard' 'hits=16 misses=4' 'miss_new=4' 'published generation 3' -- b0-19 f q
# 3. a failing shape (fake packer error naming no. 1 = pipeline 31): its key, both pipelines of it,
#    excluded; the retry publishes the rest; a later add of it is refused
session 'Metal names pipeline no. 1 of an archive never serialized before: render vs=vs_31' \
  'published generation 4: 3 pipelines \(\+5\)' 'skipped=2' 'excluded=1' '1 excluded shapes' \
  -- PSO_ARCHIVE_TEST_FAIL=1 PSO_ARCHIVE_TEST_INDEX=1 b30-33 d31 f d31 f q
# 4. in a later session the shape is still excluded: its miss is classified, its add refused
session '1 failing shapes are excluded' 'miss_excluded=1' 'excluded=1' 'hits=1' -- b31 b32 f q
# 5. a corrupt current manifest: the last good generation
cur=$(print $G/g*.manifest(On[1]))
printf '\x00\x00\x00\x00' | dd of=$cur bs=1 seek=24 conv=notrunc 2>/dev/null
session 'generation 5 failed verification' 'using the last good generation' 'on \(generations\): generation 4, ' \
  'published generation 6' -- b0 f q
# 6. one writer: a second process while the first holds the store gets lookup only
env PSO_SESSION_GENERATIONS=1 $N $W b50 s3 f q >$W/holder.out 2>&1 &
holder=$!
sleep 1
session 'lookup only' 'mode=2' 'flush ret=0' 'added=0' -- b51 f q
wait $holder
grep -q 'published generation 7' $W/holder.out || { print "FAIL holder: no generation 7"; cat $W/holder.out; fails=$((fails + 1)) }
# 7. killed after publishing generation 8: the next session loads it
env PSO_SESSION_GENERATIONS=1 $N $W b60-63 f b64 s30 >$W/killed.out 2>&1 &
victim=$!
for i in {1..50}; do grep -q 'published generation 8' $W/killed.out && break; sleep 0.2; done
kill -9 $victim 2>/dev/null
wait $victim 2>/dev/null
session 'generation 8, ' '!failed verification' 'hits=4' -- b60-63 f q
# 8. a session's ninth publish merges its eight shards into one
session "merged this session's 8 shards into one" 'published generation 18' \
  -- b70 f b71 f b72 f b73 f b74 f b75 f b76 f b77 f b78 f q
# 9. only the current and the previous generation's files stay
n=$(print -l $G/g*.manifest | wc -l)
(( n == 2 )) || { print "FAIL: $n manifests on disk, want 2"; fails=$((fails + 1)) }
ls $G | grep -q '\.tmp$' && { print "FAIL: leftover .tmp"; fails=$((fails + 1)) }

(( fails == 0 )) || { print "$fails failures (logs in $W)"; exit 1 }
print "pso_archive_generations: ok"
