#!/usr/bin/env bash
# Runs once after the dev container is created (postCreateCommand).
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

# Named volumes are created owned by root; hand them to the dev user.
sudo chown -R "$(id -u):$(id -g)" target /usr/local/cargo/registry /usr/local/cargo/git

# Same git setup as on the host: hooks, commit template, local
# settings. It writes to this clone's .git/config, which the host shares.
scripts/setup-git.sh

# Matt Pocock's engineering skills (the ones CLAUDE.md and docs/agents refer
# to). Not fatal: it needs network access and must not block the container.
# A fresh install starts from a stale snapshot of the official marketplace,
# so refresh it first or the plugin is "not found".
{
    claude plugin marketplace add anthropics/claude-plugins-official 2>/dev/null || true
    claude plugin marketplace update claude-plugins-official \
        && claude plugin install mattpocock-skills@claude-plugins-official
} || echo "warning: could not install mattpocock-skills; run it manually" >&2

# Prettier and markdownlint-cli2, run by the pre-commit hook. npm comes from
# the Node.js feature, which is installed after the Dockerfile, so they cannot
# go there.
scripts/install-tools.sh prettier markdownlint-cli2

# Download dependencies now so the first build and rust-analyzer start fast.
cargo fetch --locked

rustc --version
cargo nextest --version
