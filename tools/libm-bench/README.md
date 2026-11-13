# libm-bench

Times umbra-math's 19 `f32` libm functions on the CPU and the GPU, three ways each, and writes the results into the [Umbra Math Guide](../../crates/umbra-math/docs/math-guide.html#libm) ("What libm costs"). Run it through `scripts/bench-libm.sh`, on each benchmark machine, in the dev container:

```sh
scripts/bench-libm.sh p620   # Lenovo P620: Threadripper PRO 3975WX, and its NVIDIA GPU
scripts/bench-libm.sh r630   # Dell R630: 2 × Xeon E5-2690 v4
scripts/bench-libm.sh m2     # Apple M2, in the linux/arm64 dev container
```

Then review the guide with `git diff` and commit it. Each machine's results are one line per device in the guide's results block, which the script replaces and which no other machine touches, so runs on different machines merge without conflict.

## The three columns

| Column     | CPU                                                                                                                                           | GPU                                                                             |
| ---------- | --------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------- |
| glibc      | glibc's scalar function, declared here, one call per element                                                                                  | CUDA's `libdevice` (`__nv_sinf`, …)                                             |
| umbra-math | what umbra-math offers: its own kernel, the instruction, or its glibc wrapper                                                                 | the same functions compiled by umbra-math's `nvptx64` backend                   |
| best SIMD  | glibc's `libmvec` (`_ZGVdN8…`, AVX2, on x86-64; `_ZGVnN4…`, AdvSIMD, on arm64), found by `dlsym`; the vector instruction for exact operations | the approximate intrinsics (`__nv_fast_*`, `ex2.approx.ftz`, `sqrt.approx.ftz`) |

A function the platform does not have in a column, such as a `libmvec` function an older glibc lacks, or `cosf` in umbra-math's `nvptx64` backend, has no cell there.

## How it measures

- **Inputs.** 1024 per function, from the range a renderer gives it, from a fixed seed: the same on every machine.
- **CPU time.** The batch is mapped one call at a time (one element, one `Vec4`, or one vector), each result through `black_box`, so LLVM cannot vectorise a scalar implementation across calls. The time is the median of 201 runs of at least 200 µs, per element, on CPU 2 (`taskset`).
- **GPU time.** Throughput: 2²⁰ threads evaluate 2048 inputs each from the 1024-entry inputs, which stay in L1, and XOR the results' bits so none is dead code. The time is the median of 21 launches over the 2³¹ evaluations, in picoseconds. The cheapest functions sit on the floor of that loop.
- **Error.** The largest over the 1024 inputs, in ULPs from the correctly rounded result, which is glibc's `f64` routine (or the exact `f32` operation) rounded to `f32`.

## Layout

- `src/`: the host, `libm-bench cpu`, `gpu` (with the `gpu` feature) and `guide`, which splices records into the guide.
- `kernels/`: the GPU kernels, built for `nvptx64-nvidia-cuda` with CUDA's `libdevice` linked in, as `umbra-gpu` builds its programs.

Both are Cargo workspaces of their own, outside the umbra workspace: the kernels use the nightly's `ptx-kernel` ABI, which ADR 0027 keeps out of `crates/`. They take the repository's toolchain and `.cargo/config.toml`, so the host builds at `target-cpu=native`, which on x86-64 gives `libmvec` its AVX2 ABI.
