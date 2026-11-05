#!/usr/bin/env sh
# Applies the repository's recommended git settings to this clone only
# (`git config --local`). Safe to re-run.
set -eu

cd "$(git rev-parse --show-toplevel)"

set_local() {
    git config --local "$1" "$2"
    echo "  $1 = $2"
}

echo "Configuring $(pwd)"

# Hooks: fmt/clippy, Conventional Commits, git flow branch names, Git LFS.
set_local core.hooksPath .githooks
set_local commit.template .gitmessage

# Git LFS: install only the smudge/clean filters. --skip-repo stops git lfs
# from writing its own hooks; .githooks already calls git lfs where needed.
if ! command -v git-lfs >/dev/null 2>&1; then
    echo "error: git-lfs is not installed (macOS: brew install git-lfs)" >&2
    exit 1
fi
git lfs install --local --skip-repo >/dev/null
echo "  git lfs filters installed"
git lfs pull
# Fetch LFS objects for the checked-out commit only, not all history.
set_local lfs.fetchrecentalways false

# History: rebase local work on pull instead of creating merge commits.
set_local pull.rebase true
set_local rebase.autoStash true
set_local rebase.autoSquash true
set_local rebase.updateRefs true

# Remotes: drop deleted remote branches and tags; push sets upstream itself.
set_local fetch.prune true
set_local fetch.pruneTags true
set_local push.autoSetupRemote true
set_local push.followTags true

# Merges and diffs: remember conflict resolutions, show base in conflicts,
# better diff heuristics, highlight moved lines.
set_local rerere.enabled true
set_local rerere.autoUpdate true
set_local merge.conflictStyle zdiff3
set_local diff.algorithm histogram
set_local diff.colorMoved default

# Safety: reject corrupt objects on transfer.
set_local transfer.fsckObjects true

echo "Done."
