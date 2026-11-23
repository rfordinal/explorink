#!/bin/sh
# Lists the upstream CrossPoint commits that landed since our last review pass.
# The pointer and the ritual are documented in docs/upstream-crosspoint.md.
set -eu

ROOT=$(git rev-parse --show-toplevel)
PTR=$(grep -v '^#' "$ROOT/docs/upstream-reviewed.sha" | awk 'NF {print $1; exit}')
[ -n "$PTR" ] || { echo "no pointer in docs/upstream-reviewed.sha" >&2; exit 1; }

git remote get-url upstream >/dev/null 2>&1 || {
  echo "no 'upstream' remote; add https://github.com/crosspoint-reader/crosspoint-reader.git" >&2
  exit 1
}
git fetch upstream --quiet || echo "warning: fetch failed, reading the last fetch" >&2

echo "reviewed up to: $(git log -1 --format='%h %ad %s' --date=short "$PTR")"
echo "new since then: $(git rev-list --count "$PTR..upstream/develop") commits"
echo
git log --format='%h %ad %s' --date=short "$PTR..upstream/develop"
