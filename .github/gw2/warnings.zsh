#!/bin/zsh
# gw2 (#162): fails a build whose log has a compiler warning that its baseline doesn't list.
#
#   warnings.zsh <build log> <baseline>
#
# A warning here is its file and message without line and column, so code that moves keeps its
# warnings, and a second warning of a kind a file already has gets through. The script prints
# every warning of the build, sorted: when a change removes a warning or has to keep a new one,
# that list is the new baseline.
emulate -L zsh
setopt err_exit pipe_fail no_unset

log=$1 base=$2
found=(${(f)"$(grep -E ': warning: ' "$log" |
  sed -E 's#^(\.\./)+(dxmt/)?##; s#[^/ ]+/\.\./##g; s#:[0-9]+:[0-9]+: warning: #: warning: #' |
  LC_ALL=C sort -u || true)"})
known=(${(f)"$(<"$base")"})
new=(${found:|known})
gone=(${known:|found})

print -r -- "${#found} distinct warnings in $log (baseline ${base:t}: ${#known}):"
print -rl -- $found
for w in $gone; do print -r -- "::notice::Gone, remove it from ${base:t}: $w"; done
for w in $new; do print -r -- "::error::New warning: $w"; done
(( ${#new} == 0 ))
