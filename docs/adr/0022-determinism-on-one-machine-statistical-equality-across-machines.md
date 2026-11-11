# Determinism on one machine, statistical equality across machines

> **Amended by [ADR 0026](0026-gpu-workers-from-the-first-milestone.md) and [ADR 0028](0028-one-core-for-cpu-and-gpu.md).** A GPU Worker promises statistical equality only, even on one machine. The ban on approximate instructions applies to the CPU; the GPU backend of `umbra-math` may use PTX's approximate instructions within documented bounds. `f64` is allowed only inside `umbra-math`'s backends. The rest of this ADR still applies.
>
> **Amended on 2026-11-11 by [#142](https://github.com/sergioffpc/umbra/issues/142).** Transcendentals come from Umbra's own kernels where they exist, and otherwise from glibc on x86-64 and from CUDA's `libdevice` on `nvptx64`, no longer from the `libm` crate. The former rejection of the platform libm is kept under Considered options, with why it no longer holds.
>
> **Amended on 2026-11-11.** CI's Rust jobs run on GitHub's `ubuntu-26.04` runner, no longer in the builder image ([#144](https://github.com/sergioffpc/umbra/issues/144)). The runner is on the release image's Ubuntu release, but its glibc is not the release's: it may be a different point release, and the runner has Ubuntu's `amd64v3` build of `libc6`, whose `cbrt` and other functions use FMA and return different last bits from the baseline `amd64` build the release image ships. No test pins bits that depend on glibc; the hash of the generated RGB-to-spectrum table is no longer checked.

Umbra promises bit-identical results only for one binary on one machine. Across machines, and across Render Jobs on a Cluster, it promises images that are statistically equal: the same to the eye, and indistinguishable by the test of [ADR 0013](0013-statistical-validation-against-a-published-pbrt-v4-reference.md). Decided with the owner on 2026-11-09. This supersedes [ADR 0002](0002-bit-identical-results-across-architectures.md). It amends [ADR 0004](0004-deterministic-film-for-a-fixed-render-job.md), [ADR 0009](0009-pull-based-pass-major-scheduling.md), [ADR 0011](0011-pixel-traces-by-deterministic-re-execution.md), [ADR 0012](0012-per-cpu-binaries-locked-to-one-commit.md), [ADR 0013](0013-statistical-validation-against-a-published-pbrt-v4-reference.md) and [ADR 0020](0020-bit-identical-exr-final-image.md), which assumed bit-identical results on every Worker.

The priority is to get the most out of the owner's machines. Bit-identity across machines served three purposes, and none of them is worth what it costs in speed and complexity:

- detecting a faulty Worker: the Cluster is assumed healthy, as it is already assumed not to be hostile;
- reproducing a Render Job bit for bit: an image that is equal to the eye is enough;
- tracing a pixel on another machine: a Pixel Trace is a debugging tool, run on the machine that rendered, usually on a local render with few Pixel Samples.

The decision:

- **What stays guaranteed.** For one binary on one machine, a Pixel Sample is a pure function of `(Scene, seed, pixel, sample index)`. `umbra trace` needs exactly this. The rules that provide it are kept from ADR 0004. All randomness comes from pbrt-v4's samplers, which rebuild their state in `StartPixelSample`. Thread-local RNGs, wall-clock time and shared counters stay banned on the render path. Threads split a Work Unit by pixel, and each pixel sums its Pixel Samples in ascending sample index. The BVH is a pure function of the Scene, independent of the thread count.
- **No verification of Workers.** Work Results carry no content hash. When a Work Unit has two Work Results, from the speculative tail or from a reassignment, the first one to arrive wins and the other is discarded unread. The `verify` Render Job option is dropped.
- **Merge on arrival.** The Film merges each Work Result as soon as it arrives. There is no early-arrival buffer. Pass-major scheduling no longer has to wait for Pass p − 2 of a Tile before it assigns Pass p. Splat accumulators are merged in arrival order too. Only the order of additions changes, so only the last bits change.
- **Fast numerics.** The rules of ADR 0002 that existed only for identity across machines go:
  - A transcendental comes from Umbra's own kernel where one exists (`sin_cos`, `atan2`, `erf`, `Vec4::exp`, `Vec4::pow`), scalar or SIMD. Each kernel is the fastest that meets an accuracy bound stated in its documentation. Its tests check that bound against a higher-precision reference, exhaustively over `f32` for unary functions.
  - Every other C math function, and the kernels' own fallbacks (`pow` with an infinite or NaN exponent, `sin_cos` beyond ±2²⁰), come from the platform's math library through `umbra-math`'s safe wrappers ([ADR 0007](0007-unsafe-code-as-an-aggressive-optimisation.md)): glibc's `libm.so` on x86-64, and CUDA's `libdevice` on `nvptx64` ([ADR 0028](0028-one-core-for-cpu-and-gpu.md)). Their accuracy bounds are the ones each source documents: the "Known Maximum Errors in Math Functions" section of glibc's manual for the scalar functions, and the CUDA Math API's ULP bounds for `libdevice`. `libmvec` is not used: Miri cannot call it, and it gains nothing in scalar calls. `std`'s float methods stay out of the `no_std` core.
  - glibc's last bits are not promised. They change with its point release and with Ubuntu's build of it (`amd64` or `amd64v3`). Every Worker and the Coordinator run the one release image ([ADR 0025](0025-one-x86-64-v3-release-image.md)), whose builder and runtime share one Ubuntu digest ([ADR 0005](0005-chiseled-release-container-image.md)), so a Cluster sees one glibc; CI's Rust jobs run on the `ubuntu-26.04` runner, whose glibc can differ. Results that depend on glibc are tested against error bounds, not against bits computed with another glibc.
  - A SIMD kernel may compute in a different order from its scalar reference. Its equivalence test then checks the documented error bound. A kernel that keeps the operations and their order keeps its bit-for-bit test.
  - `mul_add` may be used wherever it is faster or more accurate. The exception is code whose rounding-error bounds pbrt-v4 derives from its exact operations, such as `Interval` and the γ bounds of intersection tests: that code keeps pbrt-v4's operations.
- **Kept rules.** These are cheap, and they keep CI stable:
  - no flush-to-zero and no denormals-are-zero;
  - no approximate instructions whose bits differ between CPU vendors, such as the `rcp` and `rsqrt` estimates. Intel and AMD return different bits for them. The same binary would then render different Films on the R630 and the P620, and a golden Film hash would depend on the CPU that the CI runner happened to get;
  - explicit FMA on the `x86-64-v3` baseline ([ADR 0023](0023-x86-64-only.md)); Rust never contracts on its own.

  With these rules, one binary in practice gives the same bits on every `x86-64-v3` CPU. That is not promised, and only CI relies on it.

- **Golden Film hashes on one Pass.** The golden renders in CI use a single Pass: Pixel Samples per Pass equals Pixel Samples per pixel. Each Tile then has one Work Result, so arrival order cannot change the Film. A pull request that changes a hash still needs a passing `umbra validate` (ADR 0013).
- **GPU.** A GPU backend only has to be statistically equal to the CPU ([ADR 0024](0024-a-gpu-backend-on-the-roadmap.md)).

## Considered options

- **Transcendentals only from the `libm` crate, never from the platform libm**, as decided here until [#142](https://github.com/sergioffpc/umbra/issues/142): rejected on 2026-11-11. The platform libm was kept out for two reasons, and neither holds any more:
  - Its last bits differ between systems. But every Worker and the Coordinator run one image, whose builder and runtime share one Ubuntu digest, so glibc's bits are fixed for a release.
  - The `no_std` core cannot link it. But `umbra-math`'s backends can call it through FFI, glibc on x86-64 and `libdevice` on `nvptx64`, and the rest of the core calls their safe wrappers.

  The `libm` crate would also be one more implementation beside glibc and `libdevice`, whose bits match neither. It was faster than glibc for some functions on the P620 (`asinf`, `acosf`, `exp2f`, see the next options); those are cold, and one source of C math per platform outweighs the difference.

- **Keeping ADRs 0002 and 0004 as they were**: rejected. They cost speed and complexity for guarantees the owner does not need. The speed cost comes from fixed `libm` implementations: `libm::powf` measured about 8× slower than glibc's on the P620. The complexity is the early-arrival buffer, the Pass window in scheduling, and hash comparison of duplicates.
- **Bit-identity per binary and CPU, with the platform libm in release builds**: rejected. The core is `no_std` and cannot call the platform libm. The platform libm is also not faster overall. On the P620, glibc was faster for `sinf`, `expf` and `powf`, and `libm` was faster for `asinf`, `acosf`, `atan2f` and `exp2f`. The `no_std` argument no longer holds (see the first option).
- **Transcendentals from glibc's `libm.so` or `libmvec`, called through FFI from `umbra-math`'s x86-64 backend** ([#138](https://github.com/sergioffpc/umbra/issues/138)): rejected on the numbers, not on `no_std`, since a `no_std` backend can call glibc through FFI. Measured on 2026-11-11 with `umbra-math`'s `transcendentals` bench at `x86-64-v3`, on the P620 and the R630, in the release image's Ubuntu 26.04 base (glibc 2.43); the tables are in `crates/umbra-math/README.md`.
  - glibc's scalar functions do not beat Umbra's kernels. They tie on `erf` and are slower on `atan2`, `exp` and `pow`. `sincosf` is 9 % faster than Umbra's `sin_cos` on the R630 (11.9 against 13.1 ns) but 4.8 times slower on the P620 (27.3 against 5.72 ns).
  - `libmvec` at 8 lanes is 2.1 to 9.3 times faster than Umbra's kernels for `sin_cos`, `atan2`, `erf` and `exp`, and 1.2 to 1.4 times for `pow`. But its `atan2f` and `expf` reach 2 ULPs, over the 1-ULP bound. A `SampledSpectrum` fills only 4 lanes, and at 4 lanes `exp` ties with Umbra's kernel and `pow` is slower on the R630.
  - glibc's speed is not stable across versions. On the P620, scalar `sincosf` took 6.12 ns with glibc 2.41 and 27.3 ns with 2.43; on 2.41 `libmvec`'s `sin_cos` was slower at 8 lanes than at 4.
  - The costs stay: FFI `unsafe` under [ADR 0007](0007-unsafe-code-as-an-aggressive-optimisation.md), which needs an end-to-end gain that cannot be measured before there is a renderer; AddressSanitizer instead of Miri for those tests; and a backend tied to `linux-gnu`, with a `libm` fallback that gives other bits.

  Worth measuring again once `umbra bench` runs the Reference Scenes and a caller has 8 values per call, for `libmvec`'s `sin_cos` and `erf`, which stay within 1 ULP.

  [#142](https://github.com/sergioffpc/umbra/issues/142) adopted glibc's scalar functions for what the `libm` crate provided, not to replace Umbra's kernels. Two of the costs above shrank: Miri runs FFI calls to glibc's `libm.so` with `-Zmiri-native-lib`, `sincosf`'s pointers included, so AddressSanitizer is needed only for what Miri cannot see inside glibc; and the `libm` fallback goes, since every target has its own C math. `libmvec` stays rejected, and Miri cannot run it either, because it does not support `__m128` arguments.

- **No determinism at all**: rejected. A Pixel Trace re-executes Pixel Samples (ADR 0011), and it is useless if it follows a different path from the render. pbrt-v4's samplers are deterministic anyway.
- **Allowing `rcp`/`rsqrt` with a Newton step**: rejected. The refined result still depends on the vendor's estimate, so the golden hashes would depend on the runner's CPU. The gain over a division or square root is small.
- **Keeping the ordered merge for Films that reproduce on one machine**: rejected. The buffer and the scheduling window are complexity for a last-bit property. CI gets its determinism from single-Pass renders instead.

## Consequences

- Two renders of one Render Job on a Cluster can differ in the last bits of some pixels. Renders on different machines can differ by noise wherever a path branches differently. Between machines, the statistical test of ADR 0013 is the only comparison.
- A Pixel Trace reproduces a Pixel Sample exactly when it runs on the machine that rendered it, with the same binary. That is always the case for a local Render Job. For a Pixel Sample rendered by another machine of a Cluster, the trace very likely follows the same path, but this is not promised.
- Invariance tests compare Films rendered with one Pass, or compare Work Results rather than Films.
- ADR 0020's file-hash test hashes a single-Pass render.
- A bump of the release image's base digest, or a different glibc on CI's runner, can change the last bits of CPU Films. Golden Film Hashes are regenerated when CI's glibc changes them.
- `umbra-math`'s x86-64 backend is tied to `x86_64-unknown-linux-gnu`.
- Risk RK-2 (bit-identity broken across architectures) is retired. Its place is taken by error-bound tests for kernels that reorder operations.
