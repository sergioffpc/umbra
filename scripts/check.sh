#!/usr/bin/env sh
# Runs the checks CI runs, on every tracked file: the one list of them. CI
# (.github/workflows/ci.yml) runs one group per job; locally, run them all
# before opening a pull request. The pre-commit hook runs the fast ones on the
# staged files only. CI runs the Rust groups in the `ci` stage of the
# Dockerfile, on the glibc the release ships; scripts/in-ci-image.sh runs them
# there locally too.
#
# Usage: scripts/check.sh [group...]   (no group: rust, lint and deny)
#   rust  fmt, clippy, doc, nextest, doctests
#   miri  umbra-math's tests under Miri, calling glibc's libm natively (slow;
#         not in the default run)
#   asan  umbra-math's tests under AddressSanitizer (not in the default run)
#   lint  typos, unused dependencies, crate READMEs, shell, workflows,
#         EditorConfig, JSON, YAML, Markdown and TOML
#   deny  cargo deny: advisories, bans, licenses and sources
#   readmes  crate READMEs only (part of lint; the pre-commit hook runs it)
#   unstable  unstable features outside umbra-kernels (part of lint)
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

# The `unsafe` code of `umbra-math` under Miri (ADR 0007), with the
# `x86-64-v3` baseline the crate requires unless RUSTFLAGS is set. Miri runs
# the FFI calls of `umbra_math::cmath` in the native libm.so.6. The path is
# the one the dynamic loader resolves (`ldconfig -p`), not the `/lib` symlink:
# Miri takes a symbol only from the file it names, and finds none through
# `/lib/x86_64-linux-gnu`, so every call there is unsupported. The pbrt-v4
# test ports are left out: their long loops take hours under Miri, and the
# equivalence tests cover every `unsafe` item. Needs the `miri` and
# `rust-src` components.
miri() {
    step env RUSTFLAGS="${RUSTFLAGS:--C target-cpu=x86-64-v3}" \
        MIRIFLAGS="-Zmiri-native-lib=/usr/lib/x86_64-linux-gnu/libm.so.6" \
        cargo miri nextest run -p umbra-math --all-features --locked \
        -E 'not test(/^pbrt_v4::/)'
}

# The tests of `umbra-math`, which call glibc, under AddressSanitizer, which
# sees inside glibc where Miri cannot (ADR 0007). The explicit `--target`
# keeps build scripts and proc macros uninstrumented.
asan() {
    step env RUSTFLAGS="${RUSTFLAGS:--C target-cpu=x86-64-v3} -Zsanitizer=address" \
        cargo nextest run -p umbra-math --all-features --locked \
        --target x86_64-unknown-linux-gnu --profile ci
}

lint() {
    step typos
    step cargo shear
    readmes
    unstable
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
unstable() {
    echo "check: unstable features outside umbra-kernels"
    if git grep -nE '^[[:space:]]*#!\[(cfg_attr\(.*[(, ])?feature\(' \
        -- 'crates/*.rs' ':!crates/umbra-kernels/' >&2; then
        echo "check: only umbra-kernels may enable #![feature(...)] (ADR 0027)" >&2
        return 1
    fi
}

if [ "$#" -eq 0 ]; then
    set -- rust lint deny
fi
for group in "$@"; do
    case "$group" in
        rust | miri | asan | lint | deny | readmes | unstable) "$group" ;;
        *)
            echo "check: unknown group '$group'; known: rust miri asan lint deny readmes unstable" >&2
            exit 1
            ;;
    esac
done
