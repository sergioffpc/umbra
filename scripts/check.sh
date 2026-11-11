#!/usr/bin/env sh
# Runs the checks CI runs, on every tracked file: the one list of them. CI
# (.github/workflows/ci.yml) runs one group per job; locally, run them all
# before opening a pull request. The pre-commit hook runs the fast ones on the
# staged files only. Miri runs in CI only (its own job in ci.yml).
#
# Usage: scripts/check.sh [group...]   (no group: rust, lint and deny)
#   rust  fmt, clippy, doc, nextest, doctests
#   lint  typos, unused dependencies, crate READMEs, shell, workflows,
#         EditorConfig, JSON, YAML, Markdown and TOML
#   deny  cargo deny: advisories, bans, licenses and sources
#   readmes  crate READMEs only (part of lint; the pre-commit hook runs it)
#   features  unstable features outside umbra-kernels (part of lint)
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
    readmes
    features
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

# Every crate has a README.md, declares it in its manifest and makes it its
# rustdoc documentation (CONTRIBUTING.md, Crate READMEs). Whether the README
# is up to date is for review to judge.
readmes() {
    echo "check: crate READMEs"
    missing=0
    for dir in crates/*/; do
        crate=${dir%/}
        root=$crate/src/lib.rs
        [ -f "$root" ] || root=$crate/src/main.rs
        if [ ! -f "$crate/README.md" ]; then
            echo "check: $crate has no README.md" >&2
            missing=1
        fi
        if ! grep -qxF 'readme = "README.md"' "$crate/Cargo.toml"; then
            echo "check: $crate/Cargo.toml does not set readme = \"README.md\"" >&2
            missing=1
        fi
        if ! grep -qxF '#![doc = include_str!("../README.md")]' "$root"; then
            echo "check: $root does not include README.md as its documentation" >&2
            missing=1
        fi
    done
    [ "$missing" -eq 0 ]
}

# The toolchain is a nightly, but only `umbra-kernels` enables unstable
# features (ADR 0027): every other crate builds on stable features alone.
features() {
    echo "check: unstable features outside umbra-kernels"
    if git grep -nE '^[[:space:]]*#!\[feature\(' -- 'crates/*.rs' \
        ':!crates/umbra-kernels/' >&2; then
        echo "check: only umbra-kernels may enable #![feature(...)] (ADR 0027)" >&2
        return 1
    fi
}

if [ "$#" -eq 0 ]; then
    set -- rust lint deny
fi
for group in "$@"; do
    case "$group" in
        rust | lint | deny | readmes | features) "$group" ;;
        *)
            echo "check: unknown group '$group'; known: rust lint deny readmes features" >&2
            exit 1
            ;;
    esac
done
