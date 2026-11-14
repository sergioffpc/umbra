//! Samplers (pbrt-v4 `src/pbrt/base/sampler.h`, `samplers.h`,
//! `samplers.cpp`): the sample values of each Pixel Sample, as a pure
//! function of the pixel, the sample index and the dimension (ADR 0022).
//!
//! The interface is the [`Sampler`] trait, dispatched through the closed
//! [`SamplerKind`] (ADR 0006).

use umbra_math::{Point2f, Point2i};

use crate::hash::mix_bits;
use crate::lowdiscrepancy::{
    DigitPermutations, PRIME_TABLE_SIZE, inverse_radical_inverse, owen_scrambled_radical_inverse,
    radical_inverse, scrambled_radical_inverse,
};

/// The sample values of the Pixel Samples of an image: each Pixel Sample is
/// started with [`start_pixel_sample`](Self::start_pixel_sample), then
/// draws its dimensions in order.
///
/// PBR 4e §8.3, pbrt-v4 `Sampler`.
pub trait Sampler {
    /// The number of Pixel Samples in each pixel.
    ///
    /// pbrt-v4 `Sampler::SamplesPerPixel`.
    fn samples_per_pixel(&self) -> u32;

    /// Starts the Pixel Sample `sample_index` of `pixel` at `dimension`: what
    /// follows depends on those alone, not on earlier calls.
    ///
    /// pbrt-v4 `Sampler::StartPixelSample`.
    fn start_pixel_sample(&mut self, pixel: Point2i, sample_index: u32, dimension: u32);

    /// The value of the next dimension, in `[0, 1)`.
    ///
    /// pbrt-v4 `Sampler::Get1D`.
    fn get_1d(&mut self) -> f32;

    /// The values of the next two dimensions, in `[0, 1)²`.
    ///
    /// pbrt-v4 `Sampler::Get2D`.
    fn get_2d(&mut self) -> Point2f;

    /// The values the Filter is sampled with, which some samplers draw from
    /// dimensions of their own.
    ///
    /// pbrt-v4 `Sampler::GetPixel2D`.
    fn get_pixel_2d(&mut self) -> Point2f;
}

/// How a [`HaltonSampler`] randomises its radical inverses, as a Scene names
/// it. pbrt-v4's `FastOwen` is for its Sobol samplers, which the Halton
/// sampler refuses; it arrives with them.
///
/// pbrt-v4 `RandomizeStrategy`.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize))]
pub enum RandomizeStrategy {
    /// No randomisation: the plain Halton sequence.
    None,
    /// A random permutation of each digit, chosen by the seed (pbrt-v4's
    /// default, `"permutedigits"`).
    #[default]
    PermuteDigits,
    /// Owen scrambling, chosen by the dimension alone: pbrt-v4's Halton
    /// sampler does not seed it.
    Owen,
}

/// A [`RandomizeStrategy`] with what it needs to draw: the digit
/// permutations for [`RandomizeStrategy::PermuteDigits`].
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum HaltonRandomizer<'a> {
    /// [`RandomizeStrategy::None`].
    None,
    /// [`RandomizeStrategy::PermuteDigits`], with the permutations of the
    /// Render Job's seed.
    PermuteDigits(DigitPermutations<&'a [u16]>),
    /// [`RandomizeStrategy::Owen`].
    Owen,
}

/// The randomised Halton sequence, with its first two dimensions spread
/// over the pixels of a tile of up to 128 × 128 pixels that repeats over
/// the image, so each pixel gets its own stratum of them.
///
/// PBR 4e §8.6, pbrt-v4 `HaltonSampler`.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct HaltonSampler<'a> {
    samples_per_pixel: u32,
    randomizer: HaltonRandomizer<'a>,
    base_scales: [u64; 2],
    base_exponents: [u32; 2],
    mult_inverse: [u64; 2],
    halton_index: u64,
    dimension: usize,
}

/// The side of the tile of pixels the first two dimensions are spread
/// over.
///
/// pbrt-v4 `HaltonSampler::MaxHaltonResolution`.
const MAX_HALTON_RESOLUTION: u32 = 128;

impl<'a> HaltonSampler<'a> {
    /// The sampler of `samples_per_pixel` Pixel Samples per pixel for an
    /// image of `full_resolution` pixels.
    ///
    /// # Panics
    ///
    /// If either side of `full_resolution` is not positive.
    ///
    /// pbrt-v4 `HaltonSampler(samplesPerPixel, fullResolution, randomize,
    /// seed)`, with the permutations of `seed` in `randomizer`.
    #[must_use]
    pub fn new(
        samples_per_pixel: u32,
        full_resolution: Point2i,
        randomizer: HaltonRandomizer<'a>,
    ) -> Self {
        let resolution = [full_resolution.x, full_resolution.y]
            .map(|r| u32::try_from(r).expect("a resolution is positive"));
        let (scale_x, exponent_x) = base_scale(2, resolution[0]);
        let (scale_y, exponent_y) = base_scale(3, resolution[1]);
        Self {
            samples_per_pixel,
            randomizer,
            base_scales: [scale_x, scale_y],
            base_exponents: [exponent_x, exponent_y],
            mult_inverse: [
                multiplicative_inverse(scale_y, scale_x),
                multiplicative_inverse(scale_x, scale_y),
            ],
            halton_index: 0,
            dimension: 0,
        }
    }

    /// The Halton index of the first Pixel Sample of `pixel`: the one whose
    /// first two dimensions, scaled by the base scales, fall in the pixel's
    /// position within the repeating tile.
    ///
    /// pbrt-v4 `HaltonSampler::StartPixelSample`.
    fn first_index(&self, pixel: Point2i) -> u64 {
        let sample_stride = self.base_scales[0] * self.base_scales[1];
        if sample_stride == 1 {
            return 0;
        }
        let tile = i32::try_from(MAX_HALTON_RESOLUTION).expect("128 fits i32");
        let pm = [pixel.x, pixel.y]
            .map(|c| u64::try_from(c.rem_euclid(tile)).expect("a residue is non-negative"));
        let offsets = [2, 3].into_iter().enumerate().map(|(i, base)| {
            let dim_offset = inverse_radical_inverse(pm[i], base, self.base_exponents[i]);
            dim_offset * (sample_stride / self.base_scales[i]) * self.mult_inverse[i]
        });
        offsets.sum::<u64>() % sample_stride
    }

    /// The value of `dimension` for the current Halton index.
    ///
    /// pbrt-v4 `HaltonSampler::SampleDimension`.
    fn sample_dimension(&self, dimension: usize) -> f32 {
        match self.randomizer {
            HaltonRandomizer::None => radical_inverse(dimension, self.halton_index),
            HaltonRandomizer::PermuteDigits(permutations) => {
                scrambled_radical_inverse(dimension, self.halton_index, permutations.get(dimension))
            }
            HaltonRandomizer::Owen => {
                let d = u64::try_from(dimension).expect("a dimension fits u64");
                #[expect(
                    clippy::cast_possible_truncation,
                    reason = "as pbrt-v4: the uint64_t MixBits passed as a uint32_t hash"
                )]
                let hash = mix_bits(1 + (d << 4)) as u32;
                owen_scrambled_radical_inverse(dimension, self.halton_index, hash)
            }
        }
    }
}

/// The smallest power of `base` at least `min(resolution, 128)`, and its
/// exponent.
fn base_scale(base: u64, resolution: u32) -> (u64, u32) {
    let target = u64::from(resolution.min(MAX_HALTON_RESOLUTION));
    let (mut scale, mut exponent) = (1, 0);
    while scale < target {
        scale *= base;
        exponent += 1;
    }
    (scale, exponent)
}

/// The inverse of `a` modulo `n`, for coprime `a` and `n`; 0 modulo 1.
///
/// pbrt-v4 `HaltonSampler::multiplicativeInverse`, by the iterative form of
/// its extended Euclidean algorithm: the inverse is unique modulo `n`.
fn multiplicative_inverse(a: u64, n: u64) -> u64 {
    let signed = |v: u64| i64::try_from(v).expect("a base scale fits i64");
    let n_signed = signed(n);
    let (mut old_r, mut r) = (signed(a), n_signed);
    let (mut old_x, mut x) = (1_i64, 0_i64);
    while r != 0 {
        let q = old_r / r;
        (old_r, r) = (r, old_r - q * r);
        (old_x, x) = (x, old_x - q * x);
    }
    debug_assert_eq!(old_r, 1, "{a} and {n} are coprime");
    u64::try_from(old_x.rem_euclid(n_signed)).expect("a residue is non-negative")
}

impl Sampler for HaltonSampler<'_> {
    fn samples_per_pixel(&self) -> u32 {
        self.samples_per_pixel
    }

    fn start_pixel_sample(&mut self, pixel: Point2i, sample_index: u32, dimension: u32) {
        let sample_stride = self.base_scales[0] * self.base_scales[1];
        self.halton_index = self.first_index(pixel) + u64::from(sample_index) * sample_stride;
        self.dimension = usize::try_from(dimension.max(2)).expect("a dimension fits usize");
    }

    fn get_1d(&mut self) -> f32 {
        if self.dimension >= PRIME_TABLE_SIZE {
            self.dimension = 2;
        }
        let dimension = self.dimension;
        self.dimension += 1;
        self.sample_dimension(dimension)
    }

    fn get_2d(&mut self) -> Point2f {
        if self.dimension + 1 >= PRIME_TABLE_SIZE {
            self.dimension = 2;
        }
        let dimension = self.dimension;
        self.dimension += 2;
        Point2f::new(
            self.sample_dimension(dimension),
            self.sample_dimension(dimension + 1),
        )
    }

    fn get_pixel_2d(&mut self) -> Point2f {
        Point2f::new(
            radical_inverse(0, self.halton_index >> self.base_exponents[0]),
            radical_inverse(1, self.halton_index / self.base_scales[1]),
        )
    }
}

/// The samplers, one per Render Job, held by value and matched on every
/// call (ADR 0006). Only pbrt-v4's `halton` so far; the others arrive with
/// the milestones whose Reference Scenes use them.
///
/// pbrt-v4 `Sampler`'s tagged pointer.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum SamplerKind<'a> {
    /// pbrt-v4 `"halton"`.
    Halton(HaltonSampler<'a>),
}

impl<'a> From<HaltonSampler<'a>> for SamplerKind<'a> {
    fn from(sampler: HaltonSampler<'a>) -> Self {
        Self::Halton(sampler)
    }
}

impl Sampler for SamplerKind<'_> {
    #[inline]
    fn samples_per_pixel(&self) -> u32 {
        match self {
            Self::Halton(s) => s.samples_per_pixel(),
        }
    }

    #[inline]
    fn start_pixel_sample(&mut self, pixel: Point2i, sample_index: u32, dimension: u32) {
        match self {
            Self::Halton(s) => s.start_pixel_sample(pixel, sample_index, dimension),
        }
    }

    #[inline]
    fn get_1d(&mut self) -> f32 {
        match self {
            Self::Halton(s) => s.get_1d(),
        }
    }

    #[inline]
    fn get_2d(&mut self) -> Point2f {
        match self {
            Self::Halton(s) => s.get_2d(),
        }
    }

    #[inline]
    fn get_pixel_2d(&mut self) -> Point2f {
        match self {
            Self::Halton(s) => s.get_pixel_2d(),
        }
    }
}
