# umbra-spectrum

How light is represented in Umbra's rendering core: spectral distributions
and the `Spectrum` interface over them, the four wavelengths a Pixel Sample
carries and the values a spectrum takes at them, tristimulus colours, RGB
colour spaces, and the RGB-to-spectrum table that turns the RGB of a Scene
File into a spectrum.

It is a core crate: `#![no_std]` without `alloc` (ADR 0014, ADR 0015). It
depends on `umbra-math` (the `Vec4` lanes of a `SampledSpectrum`, matrices,
`f32` helpers) and `umbra-sampling` (the visible-wavelength distribution),
and `umbra-shading` and `umbra-camera` depend on it (arc42
§5.1). It covers PBR 4e §4.5–4.6.

```rust
use umbra_spectrum::{
    RGB, RGBAlbedoSpectrum, RGBColorSpace, SampledWavelengths, Spectrum, SpectrumKind,
};

// The colour space is built on each call to `srgb`: build it once and keep it.
let srgb = RGBColorSpace::srgb();

// A reflectance given as RGB, as a Scene File gives it.
let red: SpectrumKind<'_> = RGBAlbedoSpectrum::new(&srgb, RGB::new(0.8, 0.1, 0.1)).into();

// The four wavelengths of one Pixel Sample, importance-sampled over the
// visible range, and the reflectance at each.
let lambda = SampledWavelengths::sample_visible(0.5);
let reflectance = red.sample(&lambda);
assert!(reflectance.is_nonzero());
assert!(reflectance.max_component_value() <= red.max_value());
```

## What is in it

| Module (pbrt-v4 source)                               | Contents                                                                                                                                                                                                                                                                                                                   |
| ----------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `spectrum` (`util/spectrum.h`)                        | The `Spectrum` trait (`eval`, `max_value`, `sample`); `ConstantSpectrum`, `DenselySampledSpectrum`, `RGBAlbedoSpectrum`, `RGBUnboundedSpectrum`, `RGBIlluminantSpectrum`; `SpectrumKind`, the closed enum over them (ADR 0006); `inner_product`, `spectrum_to_xyz`, `spectrum_to_photometric`                              |
| `sampled` (`util/spectrum.h`)                         | `SampledSpectrum`, a spectrum at `N_SPECTRUM_SAMPLES` = 4 wavelengths, with pbrt-v4's arithmetic, reductions, `safe_div`, `exp`, `pow` and conversion to XYZ and RGB; `SampledWavelengths`, uniform or visible-importance-sampled, with their PDFs and `terminate_secondary`; `LAMBDA_MIN`, `LAMBDA_MAX`, `CIE_Y_INTEGRAL` |
| `spectra` (`util/spectrum.cpp`)                       | pbrt-v4's named spectra as `static`s computed at compile time: the CIE 1931 matching functions `X`, `Y`, `Z` and the illuminant `STD_ILLUM_D65`; `StdIlluminant`, a `Copy` name for an illuminant                                                                                                                          |
| `color` (`util/color.h`)                              | `RGB`, `XYZ`, and `RGBSigmoidPolynomial`, the spectrum an RGB becomes                                                                                                                                                                                                                                                      |
| `color_space` (`util/colorspace.h`)                   | `RGBColorSpace`: primaries, white, the RGB↔XYZ matrices and the RGB-to-spectrum lookup. sRGB is the only one so far                                                                                                                                                                                                        |
| `rgb_to_spectrum` (`cmd/rgb2spec_opt.cpp`, generated) | The sRGB RGB-to-spectrum table, which the build script generates                                                                                                                                                                                                                                                           |

`spectra` is public as a module (`umbra_spectrum::spectra::X`), as pbrt-v4's
`Spectra::X()` is a namespace; everything else is re-exported at the crate
root.

Not here yet, on purpose: `PiecewiseLinearSpectrum` and `BlackbodySpectrum`
join with the `.spd` files and blackbody emitters of later milestones, and
the colour spaces other than sRGB (DCI-P3, Rec. 2020, ACES2065-1) when a
Reference Scene needs them (arc42 §4.1). The colour encodings of image
textures join with image textures.

## Design decisions

### pbrt-v4, ported faithfully

The routines are pbrt-v4's (`src/pbrt/util/spectrum.h`, `color.h`,
`colorspace.h`), in `f32` (ADR 0001), with pbrt-v4's names in snake case, so
the book and the C++ source remain the documentation. "Faithfully" includes
the order of operations and the rounding of constants: pbrt-v4's spectral data
is written as `f64` literals, because C++ reads them as `double`, and rounded
to `f32` the way C++ rounds them. pbrt-v4's spectral data and table generator
are Apache-2.0, attributed in `NOTICE`.

### Spectral only, four wavelengths in SIMD lanes

Light is spectral everywhere in the core; there is no RGB rendering mode
(ADR 0001). A Pixel Sample carries `N_SPECTRUM_SAMPLES` = 4 wavelengths, and
both `SampledSpectrum` and `SampledWavelengths` keep them in the four lanes of
a `umbra-math` `Vec4`, one wavelength per lane. Each lane-wise operation
computes the bits of pbrt-v4's loop over the samples, and the reductions
(`average`, `min_component_value`, `max_component_value`) keep its order.
`exp` and `pow` are the exception: they use `umbra-math`'s four-lane kernels,
within 1 ULP of the correctly rounded result rather than the bits of
glibc's `expf` and `powf` (ADR 0022).

### Named spectra computed at compile time

pbrt-v4 builds its named spectra in `Spectra::Init` at start-up. Here each is
a `static` `DenselySampledSpectrum` at 1 nm over `[LAMBDA_MIN, LAMBDA_MAX]`,
evaluated by `const fn` with pbrt-v4's `f32` operations, so the values are
pbrt-v4's bits and nothing is initialised at run time (arc42 §8.1). A colour
space names its illuminant by `StdIlluminant`, a `Copy` enum, rather than by
reference, so a Scene arena that holds a colour space holds no pointer
(ADR 0014).

### The RGB-to-spectrum table, generated by the build script

An RGB becomes a spectrum through the Jakob–Hanika table: for each RGB on a
64 × 64 × 64 grid (per largest component), the coefficients of a sigmoid of a
quadratic in λ (PBR 4e §4.6.6). The sRGB table is about 2.4 MB of `f32`, so it
is generated rather than committed: `build/rgb2spec.rs` ports pbrt-v4's
`rgb2spec_opt` operation for operation in `f64`, and `build/main.rs` writes
the table to `OUT_DIR` as little-endian `f32`, which `src/rgb_to_spectrum.rs`
includes as bytes.

The table is a pure function of its sources: IEEE `f64` arithmetic with no
implicit FMA, the hardware square root, and glibc's cube root, with each
cell's value independent of the thread that computes it (ADR 0022). The
build script calls `cbrt` through `umbra_math::double`'s wrapper, which it
compiles from `umbra-math`'s source by `#[path]` rather than depending on
the crate, so the script needs no `x86-64-v3` host. It can differ from
pbrt-v4's own table in the last bits of some coefficients, because pbrt-v4's
build lets the C++ compiler contract FMAs and its glibc may not be the
image's. A test pins the table's hash, which a bump of the image's Ubuntu
digest can change (ADR 0022).

## Things to know when using it

- **`RGBColorSpace::srgb()` builds the colour space on every call**: it
  computes the white point and inverts two matrices. pbrt-v4 builds it once at
  start-up; build it once and keep it.
- **Pick the RGB spectrum by what the value means**, as pbrt-v4 does:
  `RGBAlbedoSpectrum` for a reflectance in `[0, 1]` (it asserts the bound in
  debug builds), `RGBUnboundedSpectrum` for a value that may exceed 1, and
  `RGBIlluminantSpectrum` for an emitter, which multiplies by the colour
  space's illuminant.
- **`SpectrumKind<'a>` borrows**: a `DenselySampledSpectrum` refers to its
  values, so the enum carries their lifetime. The named spectra are
  `'static`.
- **`is_nonzero` is pbrt-v4's `operator bool`**: true when any wavelength is
  non-zero. `safe_div` gives 0 where the divisor is 0, as pbrt-v4's
  `SafeDiv` does.
- **`terminate_secondary`** keeps only the first wavelength, for dispersion;
  the others get PDF 0 and the first's PDF is divided by 4.
- **Wavelengths are in nm**, as in pbrt-v4.
- **The `serde` feature** (off by default; `umbra-scene` turns it on) derives
  `Serialize`/`Deserialize` for the types stored in Scene arenas.

## Tests

`tests/umbra_spectrum/` is one integration-test crate, through the public
interface only:

- `pbrt_v4/*_test.rs` port pbrt-v4's unit tests, one module per C++ test file
  (`color_test.cpp`, `spectrum_test.cpp`), each test naming the
  `TEST(Suite, Name)` it ports. Only the sRGB cases are ported, for the
  reasons above.
- `pbrt_v4/known_values.rs` holds the values pbrt-v4 itself computes on fixed
  inputs, generated by `known_values.cpp` from pbrt-v4 at the pinned commit
  (the file says how to rebuild it); the `known_values` tests compare Umbra's
  results with them, bit for bit where the computation is the same and within
  a stated number of ULPs where pbrt-v4 goes through the platform libm.
- `sampled.rs` checks `SampledSpectrum` and `SampledWavelengths` on worked
  examples, including the edge cases of pbrt-v4's definitions on zeros and
  NaNs and the order of its reductions.
- `table.rs` checks the hash of the generated sRGB table, so a table that
  changes with the generator, the toolchain or glibc's `cbrt` fails.
- `generators.rs` ports the sample generators pbrt-v4's tests draw from.

Run them with `cargo nextest run -p umbra-spectrum`.
