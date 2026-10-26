#!/usr/bin/env bash
#
# ci/clean.sh - drop the build trees, keep the checkout.
#
#     ./ci/clean.sh          # remove build/
#     ./ci/clean.sh --all    # ...and the gate's build-ci/ too
#     ./ci/clean.sh -n       # say what would go, remove nothing
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DRY=""
ALL=""

while [ $# -gt 0 ]; do
    case "$1" in
        -n|--dry-run) DRY="yes"; shift ;;
        --all)        ALL="yes"; shift ;;
        -h|--help)    sed -n '2,7p' "$0"; exit 0 ;;
        *) echo "usage: ci/clean.sh [--all] [-n]" >&2; exit 2 ;;
    esac
done

TARGETS=()
add() { if [ -e "$1" ]; then TARGETS+=("$1"); fi; }
add "$REPO_DIR/build"
if [ -n "$ALL" ]; then add "$REPO_DIR/build-ci"; fi

if [ "${#TARGETS[@]}" -eq 0 ]; then
    echo "Nothing to remove."
    exit 0
fi

for t in "${TARGETS[@]}"; do
    if [ -n "$DRY" ]; then echo "would remove ${t#"$REPO_DIR"/}"; else echo "removing ${t#"$REPO_DIR"/}"; rm -rf -- "$t"; fi
done
