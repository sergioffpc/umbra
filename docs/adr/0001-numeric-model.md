# Numeric model: f32, spectral-only, pbrt-v4 error bounds

Umbra follows pbrt-v4's numeric model with no configurability. The rendering core computes in `f32` only, with no `f64` build option and no `Float` alias. Radiance is always spectral: a `SampledSpectrum` of a constant `N_SPECTRUM_SAMPLES = 4` wavelengths, sampled as pbrt-v4 does. There is no RGB rendering mode. Self-intersection is avoided with pbrt-v4's conservative floating-point error bounds (`Interval`/`Point3fi`, a `pError` for every shape, `offset_ray_origin`/`spawn_ray_to`, `1 + 2γ(3)` in ray-box tests, scalar and SIMD alike). `f64` appears only in the per-pixel sums of Work Results and the Film.

We chose this because:

- `f32` and N = 4 fit the `Vec4` SIMD facade exactly: one 128-bit register on NEON and SSE.
- It respects the GPU-readiness constraints, since consumer GPUs have slow `f64`.
- The proven error bounds work for every pbrt-v4 shape, and they keep the code explained as in the book.

## Considered options

- **Opt-in `f64`** (pbrt's `PBRT_FLOAT_AS_DOUBLE`): rejected. It doubles the test matrix, halves the SIMD width and spreads casts through the code. Robustness comes from the error bounds, not from more bits.
- **Configurable N, or an RGB mode**: rejected. An RGB mode would duplicate the material and texture paths. Changing N, if ever needed, means editing one constant.
- **Wächter & Binder ULP-space offsetting** (*Ray Tracing Gems*, ch. 6): rejected as the default. It is cheaper but empirical: its offsets do not depend on the shape's actual error, it over-offsets contact geometry, and it does not handle shadow-ray endpoints. It may return only for triangles, under the aggressive-optimisation rule (benchmark, isolated contract, tested against the reference).
- **Epsilon offsets**: rejected because they are fragile under changes of scene scale.

## Consequences

- Every Pixel Sample is converted to sensor RGB on the Worker. A Work Result carries, per pixel, the `f64` sums of weighted RGB and of filter weights, sent as raw `f64`. The Film accumulates in `f64`.
- The Film uses filter importance sampling only: each Pixel Sample contributes to its own pixel, so Tiles never splat into each other.
