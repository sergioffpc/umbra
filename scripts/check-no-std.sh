#!/usr/bin/env sh
# Checks that the seven core crates build without `std` and without `alloc`
# (ADR 0014, ADR 0023), with the `serde` feature off and on. It links them into
# the `no_std` `staticlib` of `ci/no-std-check/`, whose `Cargo.toml` explains
# what makes the build fail. The copied `Cargo.lock` keeps the versions the
# root workspace locks.
set -eu

cd "$(git rev-parse --show-toplevel)"

cp Cargo.lock ci/no-std-check/Cargo.lock
cargo build --manifest-path ci/no-std-check/Cargo.toml --target-dir target/no-std-check
cargo build --manifest-path ci/no-std-check/Cargo.toml --target-dir target/no-std-check --features serde
