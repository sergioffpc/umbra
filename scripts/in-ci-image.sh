#!/usr/bin/env sh
# Runs a command in the CI image: the `ci` stage of the Dockerfile, on the
# Ubuntu digest and toolchain the release image is built with, so tests,
# oracles and hashes see the glibc the release ships (ADR 0005, ADR 0025).
# CI's Rust jobs (.github/workflows/rust.yml) run their scripts/check.sh group
# with it; run it locally the same way:
#
#   scripts/in-ci-image.sh scripts/check.sh rust
#   scripts/in-ci-image.sh scripts/check.sh miri asan
#
# Usage: scripts/in-ci-image.sh command [argument...]
#
# Builds the image with `docker build` (Docker or Podman) unless
# UMBRA_CI_IMAGE names one already built, as CI does. The checkout is mounted
# at /src; a git worktree cannot be, since its `.git` points outside it. The
# command runs as root, so its build output goes to UMBRA_CI_TARGET and the
# cargo registry to UMBRA_CI_REGISTRY: Docker volumes by default, which keep
# root-owned files out of the checkout, or absolute paths, which CI caches.
# RUSTFLAGS and CARGO_TERM_COLOR are passed through.
set -eu

cd "$(git rev-parse --show-toplevel)"

if [ "$#" -eq 0 ]; then
    echo "in-ci-image: usage: scripts/in-ci-image.sh command [argument...]" >&2
    exit 1
fi

docker=${DOCKER:-docker}
image=${UMBRA_CI_IMAGE:-}
if [ -z "$image" ]; then
    image=umbra-ci
    echo "in-ci-image: $docker build --target ci -t $image ."
    "$docker" build --target ci -t "$image" .
fi

echo "in-ci-image: $*"
exec "$docker" run --rm \
    -v "$PWD:/src" \
    -v "${UMBRA_CI_TARGET:-umbra-ci-target}:/src/target" \
    -v "${UMBRA_CI_REGISTRY:-umbra-ci-registry}:/usr/local/cargo/registry" \
    -e RUSTFLAGS \
    -e CARGO_TERM_COLOR \
    -w /src \
    "$image" "$@"
