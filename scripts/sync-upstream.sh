#!/bin/sh
# Update the 'upstream' branch with the pristine storescp.cc from a DCMTK
# source tree, then merge it into the current branch by hand.
#
# Usage: scripts/sync-upstream.sh /path/to/dcmtk-source
#
# The 'upstream' branch holds only src/storescp+.cc, byte-identical to
# DCMTK's dcmnet/apps/storescp.cc. Our local modifications live on 'main'
# as commits on top of it, so a DCMTK version bump is:
#
#   scripts/sync-upstream.sh ~/src/dcmtk
#   git merge upstream

set -eu

DCMTK_SRC=${1:?usage: $0 /path/to/dcmtk-source}
SRC_FILE=$DCMTK_SRC/dcmnet/apps/storescp.cc

if ! [ -f "$SRC_FILE" ]; then
    echo "error: $SRC_FILE not found (is $DCMTK_SRC a DCMTK source tree?)" >&2
    exit 1
fi

DCMTK_VERSION=$(cat "$DCMTK_SRC/VERSION" 2>/dev/null || echo unknown)
REPO_ROOT=$(git rev-parse --show-toplevel)
WORKTREE=$(mktemp -d)

cleanup() { git -C "$REPO_ROOT" worktree remove --force "$WORKTREE" 2>/dev/null || true; }
trap cleanup EXIT

git -C "$REPO_ROOT" worktree add "$WORKTREE" upstream
cp "$SRC_FILE" "$WORKTREE/src/storescp+.cc"
git -C "$WORKTREE" add src/storescp+.cc

if git -C "$WORKTREE" diff --cached --quiet; then
    echo "upstream branch already matches DCMTK $DCMTK_VERSION; nothing to do."
else
    git -C "$WORKTREE" commit -m "Update storescp.cc from DCMTK $DCMTK_VERSION"
    echo
    echo "upstream branch updated. Now merge it into your branch:"
    echo
    echo "  git merge upstream"
fi
