#!/usr/bin/env sh
# Benchmarks umbra-math's libm functions on one of the three benchmark
# machines and writes the results into the Umbra Math Guide
# (crates/umbra-math/docs/math-guide.html, "What libm costs"): the CPU
# always, and the GPU where there is an NVIDIA GPU and CUDA's libdevice.
# Run it in the dev container, whose glibc is the release image's, then
# review and commit the guide. tools/libm-bench/README.md has the details.
#
# Usage: scripts/bench-libm.sh p620|r630|m2
#   p620  Lenovo P620: AMD Ryzen Threadripper PRO 3975WX, and its NVIDIA GPU
#   r630  Dell R630: 2 × Intel Xeon E5-2690 v4
#   m2    Apple M2, in the linux/arm64 dev container (ADR 0034)
#
# The CPU bench runs on one core, CPU 2 (on the R630, a core of NUMA node
# 0), so the scheduler does not move it. Each machine's results replace its
# own lines in the guide and leave the others' alone.
set -eu

cd "$(git rev-parse --show-toplevel)"

case "${1:-}" in
    p620) label=P620 ;;
    r630) label=R630 ;;
    m2) label=M2 ;;
    *)
        echo "usage: scripts/bench-libm.sh p620|r630|m2" >&2
        exit 1
        ;;
esac
machine=$1

tool=tools/libm-bench
guide=crates/umbra-math/docs/math-guide.html
target=target/libm-bench
bin=$target/release/libm-bench
commit=$(git rev-parse --short HEAD)
date=$(date -u +%Y-%m-%d)

# CUDA's libdevice, looked for as umbra-gpu's build script does.
libdevice=${UMBRA_LIBDEVICE:-}
for root in "${CUDA_PATH:-}" "${CUDA_HOME:-}" /usr/local/cuda; do
    if [ -z "$libdevice" ] && [ -n "$root" ] && [ -f "$root/nvvm/libdevice/libdevice.10.bc" ]; then
        libdevice=$root/nvvm/libdevice/libdevice.10.bc
    fi
done
gpu=no
if [ -n "$libdevice" ] && command -v nvidia-smi >/dev/null && nvidia-smi -L >/dev/null 2>&1; then
    gpu=yes
fi

features=
[ "$gpu" = yes ] && features="--features gpu"
# shellcheck disable=SC2086 # $features is empty or one flag
cargo build --release --locked --manifest-path "$tool/Cargo.toml" --target-dir "$target" $features

pin=
if command -v taskset >/dev/null; then
    pin="taskset -c 2"
else
    echo "bench-libm: no taskset, so the CPU bench is not pinned to a core" >&2
fi

records=$(mktemp -d)
trap 'rm -rf "$records"' EXIT

echo "bench-libm: CPU of $label"
# shellcheck disable=SC2086 # $pin is empty or a command and its arguments
$pin "$bin" cpu "$machine-cpu" "$label" "$commit" "$date" >"$records/cpu.js"

if [ "$gpu" = yes ]; then
    echo "bench-libm: GPU of $label"
    # The kernels are built as umbra-gpu builds its programs: for nvptx64,
    # without the host's RUSTFLAGS, with libdevice linked in.
    (cd "$tool/kernels" && env -u RUSTFLAGS cargo rustc --release --locked \
        --target nvptx64-nvidia-cuda --target-dir "../../../$target-kernels" \
        --lib --crate-type cdylib -- "-Clink-arg=$libdevice")
    ptx=$target-kernels/nvptx64-nvidia-cuda/release/libm_bench_kernels.ptx
    "$bin" gpu "$machine-gpu" "$label" "$commit" "$date" "$ptx" >"$records/gpu.js"
else
    echo "bench-libm: no NVIDIA GPU with CUDA's libdevice here, so no GPU results"
fi

"$bin" guide "$guide" "$records"/*.js
echo "bench-libm: updated $guide; review it with git diff, then commit it"
