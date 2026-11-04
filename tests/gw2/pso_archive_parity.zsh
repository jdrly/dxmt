#!/bin/zsh
# gw2 (#75): parity of the PSO archive instance (src/winemetal/unix/pso_archive.c, generation
# publisher off) with the archive as #55 left it (tests/gw2/fixtures/legacy_pso_archive.c). Run by
# tests/gw2/pso_archive_test.m:
#
#   pso_archive_parity.zsh <legacy driver> <new driver> <work dir>
#
# Each scenario runs the same session script through both drivers, each in its own archive
# directory, and compares the reports and counters (timings, sizes, temp paths, session file names
# normalized), the file listings (session names by rank) and every .keys file byte for byte. Sessions
# are a second apart, so the session files' names (unix time first) sort the same in both.

emulate -L zsh
setopt pipefail
L=$1 N=$2 W=$3
[[ -x $L && -x $N && -n $W ]] || { print -u2 "usage: $0 <legacy> <new> <dir>"; exit 2 }
# repeated slashes collapsed: the drivers print their directory normalized, and norm() matches it
W=$(print -r -- $W | tr -s /)
rm -rf $W && mkdir -p $W/legacy $W/new || exit 2
fails=0

norm() {
  sed -E -e "s|$1|DIR|g" -e 's/[0-9]{10}-[0-9]+\.mtlarchive/SESSION.mtlarchive/g' \
    -e 's/(in|after) [0-9]+ ms/\1 N ms/g' -e 's/[0-9]+\.[0-9] MB/X MB/g' \
    -e 's/gpuarchiver-[0-9a-f]+/gpuarchiver-H/g' -e 's/default-[0-9a-f]+\.metallib/default-H.metallib/g'
}

# the files under $1, sorted, each session's "<time>-<pid>" replaced by its rank
listing() {
  (cd $1 && find . -type f | sort | awk '{
    if (match($0, /[0-9]+-[0-9]+\.mtlarchive/)) {
      stem = substr($0, RSTART, RLENGTH)
      if (!(stem in rank)) rank[stem] = ++n
      sub(/[0-9]+-[0-9]+\.mtlarchive/, "S" rank[stem] ".mtlarchive")
    }
    print
  }')
}

keys() {
  local f
  for f in $(cd $1 && find . -name '*.keys' | sort); do cat $1/$f; done
}

# run <name> [VAR=value...] -- <script words>
run() {
  local name=$1 envs=()
  shift
  while [[ $1 != -- ]]; do envs+=($1); shift; done
  shift
  env $envs $L $W/legacy "$@" 2>&1 | norm $W/legacy >$W/$name.legacy.out
  env $envs $N $W/new "$@" 2>&1 | norm $W/new >$W/$name.new.out
  local ok=1
  diff -u $W/$name.legacy.out $W/$name.new.out >$W/$name.diff || ok=0
  diff -u <(listing $W/legacy) <(listing $W/new) >>$W/$name.diff || ok=0
  cmp -s <(keys $W/legacy) <(keys $W/new) || { print ".keys files differ" >>$W/$name.diff; ok=0 }
  if (( ok )); then
    print "parity ok: $name ($(grep -c . $W/$name.new.out) lines)"
  else
    print "PARITY FAIL: $name"
    head -40 $W/$name.diff
    fails=$((fails + 1))
  fi
  sleep 1.1
}

# the newest session file of $1, truncated (an interrupted write the .sum catches)
truncate_newest() {
  local f=$(find $1 -name '*.mtlarchive' | sort | tail -1)
  head -c 1000 $f >$f.cut && mv $f.cut $f
}

run first -- b0-11 f b12-15 f q                                 # Metal's own packer failure, rebuilt
run hits -- b0-19 f q                                           # hits, known/new misses
run fake PSO_ARCHIVE_TEST_FAIL=1 PSO_ARCHIVE_TEST_INDEX=2 -- b20-27 d22 f q
run twice PSO_ARCHIVE_TEST_FAIL=2 PSO_ARCHIVE_TEST_INDEX=1 -- b28-31 f b32 f q   # retry fails too, dirty
run nothing -- f q
truncate_newest $W/legacy
truncate_newest $W/new
run truncated -- b0-3 f q                                       # deleted: .sum does not match
run capped PSO_SESSION_MAX=3 -- b40-45 f q
run keep PSO_SESSION_KEEP=2 -- b0 f q                           # older sessions deleted
run unflushed -- b46-47                                         # the exit report

(( fails == 0 )) || { print "$fails parity failures (logs in $W)"; exit 1 }
print "pso_archive_parity: ok"
