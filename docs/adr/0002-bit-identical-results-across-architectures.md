# Bit-identical results across Worker architectures

The same Work Unit, rendered from the same source, must produce bit-identical Work Results on every Worker, whether arm64 (macOS) or x86-64 (Linux). The Coordinator can then reassign or re-render a Work Unit on any machine and get the same pixels. This makes per-pixel debugging reproducible anywhere and lets a mismatch be treated as a bug rather than as noise.

To get there, the core gives up some convenience and some speed:

- **No platform libm.** Transcendentals (`exp`, `pow`, `sin`, …) come from the pure-Rust `libm` crate, never from the system math library, because macOS libm and glibc differ in the last bit. The `no_std` core could not use `std`'s float methods anyway.
- **Explicit FMA, required in hardware.** pbrt-v4's accuracy helpers (`difference_of_products`, `sum_of_products`, `evaluate_polynomial`, compensated sums) use `mul_add` exactly where pbrt uses FMA, and nowhere else. Every target CPU has FMA (Apple M2, Zen, Haswell/Broadwell Xeon E5), and the `x86-64-v3` baseline includes it. Rust never contracts `a * b + c` implicitly.
- **No fast-math and no approximate instructions.** `rcp`/`rsqrt` estimates (`_mm_rcp_ps`, `vrecpeq_f32`, …) differ between vendors and are banned wherever they affect a result. `inv_dir` is an exact `1.0 / d`.
- **SIMD equals scalar.** Each SIMD kernel performs the same operations in the same order as its scalar reference, which serves as the test oracle. The `Vec4` facade gives `min`/`max` explicit NaN semantics (matching pbrt's ray-box behaviour), identical on NEON and AVX2.
- **IEEE defaults.** Umbra never sets flush-to-zero or denormals-are-zero.
- **Deterministic generated data.** The RGB-to-spectrum coefficient tables (Jakob & Hanika) are generated in `build.rs` with `libm`, and CI checks a hash of the generated table on every target.

NaN and infinity checks are `debug_assert!`s at the points where pbrt-v4 has `DCHECK`s: Pixel Sample contributions, weights and `SampledSpectrum`.

Seeding and the merge order of Work Results into the Film are not settled here. They belong to the determinism decision.

## Considered options

- **Faster custom polynomial approximations** for transcendentals: deferred. They are allowed only under the aggressive-optimisation rule (benchmark, isolated contract, tested against the `libm` reference).
- **Committing the generated tables as binaries**: rejected. That costs several MB of git history per regeneration, while `build.rs` keeps the algorithm readable in the repository.
- **Generating the tables at Worker start-up**: rejected. It slows every start-up and still needs deterministic math.
