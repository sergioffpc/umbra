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

# Hooks: fmt/clippy, Conventional Commits, git flow branch names.
set_local core.hooksPath .githooks
set_local commit.template .gitmessage

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
