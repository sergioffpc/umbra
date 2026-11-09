#!/usr/bin/env sh
# Runs the checks CI runs, on every tracked file: the one list of them. CI
# (.github/workflows/ci.yml) runs one group per job; locally, run them all
# before opening a pull request. The pre-commit hook runs the fast ones on the
# staged files only. Miri runs in CI only (it needs a nightly toolchain).
#
# Usage: scripts/check.sh [group...]   (no group: rust, lint and deny)
#   rust  fmt, clippy, doc, nextest, doctests
#   lint  typos, unused dependencies, shell, workflows, EditorConfig, JSON,
#         YAML, Markdown and TOML
#   deny  cargo deny: advisories, bans, licenses and sources
#
# The tools are installed by scripts/install-tools.sh. Stops at the first
# failure.
set -eu

cd "$(git rev-parse --show-toplevel)"

# Prints the command, then runs it.
step() {
    echo "check: $*"
    "$@"
}

# Tracked files matching the given pathspecs, as arguments to a command.
tracked() {
    pathspecs=""
    while [ "$1" != -- ]; do
        pathspecs="$pathspecs $1"
        shift
    done
    shift
    echo "check: $* on tracked$pathspecs"
    # Word splitting of the pathspecs is intended; `set -f` keeps the shell
    # from expanding their globs, which are git's.
    set -f
    # shellcheck disable=SC2086
    git ls-files -z -- $pathspecs | xargs -0 "$@"
    set +f
}

rust() {
    step cargo fmt --all -- --check
    step cargo clippy --all-targets --all-features --locked -- -D warnings
    step env RUSTDOCFLAGS="-D warnings" cargo doc --no-deps --document-private-items --all-features --locked
    step cargo nextest run --all-features --locked --profile ci
    # nextest does not run doctests.
    step cargo test --doc --all-features --locked
}

lint() {
    step typos
    step cargo shear
    tracked '.githooks/*' '*.sh' -- shellcheck
    step actionlint
    tracked . -- editorconfig-checker
    tracked '*.json' '*.yml' '*.yaml' '*.md' -- prettier --check
    tracked '*.toml' -- taplo fmt --check
    tracked '*.toml' -- taplo lint
    tracked '*.md' -- markdownlint-cli2
}

deny() {
    step cargo deny check
}

if [ "$#" -eq 0 ]; then
    set -- rust lint deny
fi
for group in "$@"; do
    case "$group" in
        rust | lint | deny) "$group" ;;
        *)
            echo "check: unknown group '$group'; known: rust lint deny" >&2
            exit 1
            ;;
    esac
done
