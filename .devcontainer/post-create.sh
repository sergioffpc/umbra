#!/usr/bin/env bash
# Runs once after the dev container is created (postCreateCommand).
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

# Named volumes are created owned by root; hand them to the dev user.
sudo chown -R "$(id -u):$(id -g)" target /usr/local/cargo/registry /usr/local/cargo/git

# Same git setup as on the host: hooks, commit template, Git LFS, local
# settings. It writes to this clone's .git/config, which the host shares.
scripts/setup-git.sh

# Download dependencies now so the first build and rust-analyzer start fast.
cargo fetch --locked

rustc --version
cargo nextest --version
