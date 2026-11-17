#!/bin/zsh
# gw2 (#318): my fork's footprint in upstream's files, as a Markdown table for the step summary.
#
#   .github/gw2/footprint.zsh [UPSTREAM [HEAD [BASE]]]
#
#   UPSTREAM  upstream's tree (default origin/main, which mirrors 3Shain/dxmt)
#   HEAD      the commit to measure (default HEAD)
#   BASE      the branch a pull request goes into (default: none). With it, the table also has
#             the footprint at the merge base of HEAD and BASE, and the change between the two.
#
# A file counts when upstream has it and my fork changed or deleted it. "Added" lines are mine
# inside upstream's files. "Removed" lines are upstream lines my fork rewrote or deleted: each one
# is a place where an upstream change to that line conflicts at the next sync. CONTRIBUTING's
# code rules are about keeping that number from growing.
#
# It reads git objects only and always exits 0: a number for the reviewer, not a gate.

emulate -L zsh
setopt pipefail

UPSTREAM=${1:-origin/main} HEAD_REV=${2:-HEAD} BASE=${3:-}

if ! git rev-parse --verify --quiet "$UPSTREAM^{commit}" >/dev/null ||
   ! git rev-parse --verify --quiet "$HEAD_REV^{commit}" >/dev/null; then
  print -r -- "Fork footprint: no \`$UPSTREAM\` or no \`$HEAD_REV\` in this checkout (it needs the full history), so no numbers."
  exit 0
fi

# footprint REV: "files added removed" for REV against UPSTREAM, upstream's files only
footprint() {
  git diff --numstat --no-renames --diff-filter=MD $UPSTREAM $1 |
    awk '{ files++; if ($1 != "-") { added += $1; removed += $2 } } END { printf "%d %d %d\n", files, added, removed }'
}

head=(${=$(footprint $HEAD_REV)})
print -r -- "### Fork footprint in upstream's files"
print -r -- ''
print -r -- "Against \`$UPSTREAM\` at \`$(git rev-parse --short $UPSTREAM)\`. Removed lines are upstream lines my fork rewrote or deleted."
print -r -- ''
print -r -- "| | Files | Added lines | Removed lines |"
print -r -- "|---|---|---|---|"
print -r -- "| \`$(git rev-parse --short $HEAD_REV)\` | $head[1] | $head[2] | $head[3] |"
if [[ -n $BASE ]] && mb=$(git merge-base $HEAD_REV $BASE 2>/dev/null); then
  base=(${=$(footprint $mb)})
  print -r -- "| \`$(git rev-parse --short $mb)\` (where it left \`$BASE\`) | $base[1] | $base[2] | $base[3] |"
  printf '| Change | %+d | %+d | %+d |\n' $(( head[1] - base[1] )) $(( head[2] - base[2] )) $(( head[3] - base[3] ))
fi
print -r -- ''
print -r -- "The ten files with the most removed lines:"
print -r -- ''
print -r -- "| File | Added | Removed |"
print -r -- "|---|---|---|"
git diff --numstat --no-renames --diff-filter=MD $UPSTREAM $HEAD_REV |
  awk '$1 != "-" { print $2 "\t" $1 "\t" $3 }' | sort -rn | head -10 |
  awk -F'\t' '{ printf "| `%s` | %d | %d |\n", $3, $2, $1 }'
exit 0
