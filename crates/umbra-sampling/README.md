# umbra-sampling

How Umbra's rendering core chooses numbers: pbrt-v4's hashing and random
number generator, the sampling functions ("warps") that map uniform samples
to the distributions Shapes, Lights, BxDFs, Cameras and Films sample, their
PDFs and inverses, and piecewise-constant distributions for tabulated
functions.

It is a core crate: `#![no_std]` without `alloc` (ADR 0014, ADR 0015). It
depends on `umbra-math` only, whose `cmath` wrappers give it glibc's C math,
and `umbra-spectrum` and
`umbra-geometry` depend on it (arc42 §5.1). It covers PBR 4e chapter 2 and
appendix A, and the RNG of §B.2.8; chapter 8's samplers are still to come.

```rust
use umbra_math::Point2f;
use umbra_sampling::{PiecewiseConstant1D, Rng, cosine_hemisphere_pdf, sample_cosine_hemisphere};

// pbrt-v4's PCG32, on stream 7: the same seed draws pbrt-v4's numbers.
let mut rng = Rng::new(7);
let u = Point2f::new(rng.uniform(), rng.uniform());

// A cosine-weighted direction around +z, and its PDF with respect to solid angle.
let w = sample_cosine_hemisphere(u);
assert!(w.z >= 0.0);
assert!(cosine_hemisphere_pdf(w.z) >= 0.0);

// A tabulated function sampled by inverting its CDF, in storage the caller
// provides: a `Vec` here, a borrowed slice on the per-Pixel-Sample path.
let d = PiecewiseConstant1D::new_in(&[1.0, 3.0], 0.0..1.0, |n| vec![0.0; n]);
let s = d.sample(0.5);
assert_eq!(s.offset, 1);
```

## What is in it

| Module (pbrt-v4 source)                    | Contents                                                                                                                                      |
| ------------------------------------------ | --------------------------------------------------------------------------------------------------------------------------------------------- |
| `hash` (`util/hash.h`)                     | `mix_bits`, `murmur_hash_64a`, and the variadic `hash`/`hash_float` over `Hashable` values and tuples, which seed every sampler and RNG       |
| `rng` (`util/rng.h`)                       | `Rng`, O'Neill's PCG32, with streams, offsets, `advance` and the distance between two states; `Uniform`/`UniformBelow` for the types it draws |
| `warp` (`util/sampling.h`, `sampling.cpp`) | The warps, their PDFs and, where pbrt-v4 has them, their inverses, one submodule per family (below)                                           |
| `piecewise` (`util/sampling.h`)            | `PiecewiseConstant1D` and `PiecewiseConstant2D`, sampled by inversion, over storage the caller provides                                       |

The warp families:

| Submodule           | Contents                                                                                               |
| ------------------- | ------------------------------------------------------------------------------------------------------ |
| `disk`              | Uniform points of the unit disk, polar and concentric                                                  |
| `sphere`            | Uniform directions over the sphere, the hemisphere and a cone, and cosine-weighted over the hemisphere |
| `triangle`          | Triangles uniformly by area, and uniformly by the solid angle they subtend from a point                |
| `bilinear`          | The bilinear distribution on `[0, 1]²`                                                                 |
| `univariate`        | Linear, exponential and visible-wavelength distributions                                               |
| `discrete`          | `sample_discrete` among weighted outcomes, and the balance and power heuristics of MIS                 |
| `henyey_greenstein` | The Henyey–Greenstein phase function and its sampling                                                  |

Everything is re-exported at the crate root.

Not here yet: the low-discrepancy sequences and the `Sampler` interface with
its closed `SamplerKind` (ADR 0006) arrive with the `HaltonSampler` of
[#77 Black frame through Renderer and Film](https://github.com/sergioffpc/umbra/issues/77),
and the other samplers with the milestones whose Reference Scenes use them.
The `serde` feature is wired but derives nothing until then.

## Design decisions

### pbrt-v4, ported faithfully, so a seed draws pbrt-v4's numbers

The routines are pbrt-v4's, in `f32` (ADR 0001), with pbrt-v4's names in snake
case. A Pixel Sample is a pure function of the Scene, the seed, the pixel and
the sample index (ADR 0022), and its numbers come from these routines, so they
follow pbrt-v4's operations in pbrt-v4's order with no implicit FMA. The hash
and the RNG then give pbrt-v4's bits exactly; a warp gives pbrt-v4's bits up
to the last-bit differences between the glibc of Umbra's image and the libm
pbrt-v4 was built with.

`hash` hashes the bytes pbrt-v4's variadic `Hash` copies: the arguments one
after the other, with no padding, each in its C++ layout. The width of each
argument matters, as it does in pbrt-v4.

### No allocator: distributions live in storage the caller provides

The core never allocates (ADR 0008, ADR 0014). pbrt-v4's
`PiecewiseConstant1D` and `PiecewiseConstant2D` take an `Allocator`; here
`new_in` takes an `alloc` closure that returns a buffer of the length asked
for, and the distribution is generic over that storage. A `std` crate building
a Scene passes `|n| vec![0.0; n]`; per-Pixel-Sample code samples a
distribution over a borrowed slice.

## Things to know when using it

- **`Rng` is `Clone` but not `Copy`**, so a stream is never duplicated by
  accident. `Rng::new(i)` starts sequence `i` at an offset of `mix_bits(i)`, as
  pbrt-v4's `RNG(seqIndex)` does.
- **`rng.uniform::<f32>()` is in `[0, 1)`**: a `u32` draw times 2⁻³², clamped
  to `ONE_MINUS_EPSILON`.
- **Hash with the width pbrt-v4 uses**: an `i32` where pbrt-v4 hashes an
  `int`, a `u64` where it hashes a `uint64_t`. Another width gives another
  hash, and another sequence.
- **Hemispheres and cones are around `+z`**, and PDFs of directions are with
  respect to solid angle.
- **Negative values of a piecewise-constant function count as their absolute
  value**, as in pbrt-v4, and a function that is zero everywhere is sampled
  uniformly with a PDF of 0.
- **`new_in` panics** if the function is empty, the domain is empty, or
  `alloc` returns storage of another length.

## Tests

`tests/umbra_sampling/` is one integration-test crate, through the public
interface only:

- `pbrt_v4/*_test.rs` port pbrt-v4's unit tests, one module per C++ test file
  (`hash_test.cpp`, `rng_test.cpp`, `sampling_test.cpp`), each test naming the
  `TEST(Suite, Name)` it ports. `TEST(RNG, Double)` is not ported: Umbra has
  no `f64` samples (ADR 0001).
- `pbrt_v4/known_values.rs` holds the values pbrt-v4 itself computes on fixed
  inputs, generated by `known_values.cpp` from pbrt-v4 at the pinned commit
  (the file says how to rebuild it), and the `known_values` tests compare
  Umbra's results with them.
- `sampling.rs` runs the statistical tests of arc42 §8.11 on every warp and
  distribution, with fixed seeds: a χ² goodness-of-fit of the samples against
  the PDF (`chi2.rs`, corrected for the number of χ² tests in the suite), the
  PDF agreeing with the sampling routine, and the PDF integrating to 1.
- `generators.rs` ports the sample generators pbrt-v4's tests draw from.

Run them with `cargo nextest run -p umbra-sampling`.
