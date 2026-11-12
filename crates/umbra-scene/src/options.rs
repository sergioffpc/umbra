//! The Render Job Options: what a Render Job is given at submission rather
//! than in its Scene File (arc42 §8.2).

use std::num::NonZeroU32;

/// The seed of every sampler of a Render Job: with the Scene, the pixel and
/// the sample index, it fixes every Pixel Sample (ADR 0022). pbrt-v4's
/// `--seed`, an `int`; 0 by default.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Default)]
pub struct Seed(pub i32);

impl From<Seed> for u32 {
    /// The seed's bits, as pbrt-v4 passes its `int` seed where a `uint32_t`
    /// is taken.
    fn from(seed: Seed) -> Self {
        seed.0.cast_unsigned()
    }
}

/// The settings of a Render Job given at submission: the seed, how the
/// image is cut into Tiles, and how the Pixel Samples of every pixel are
/// cut into Passes (arc42 §8.8).
///
/// A Pass is `pixel_samples_per_pass` consecutive sample indices; the last
/// one is shorter if `pixel_samples_per_pixel` is not a multiple of it.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct RenderJobOptions {
    /// The seed of the samplers.
    pub seed: Seed,
    /// The side of a square Tile, in pixels; the Tiles at the right and
    /// bottom edges are cut short by the image.
    pub tile_size: NonZeroU32,
    /// The number of Pixel Samples of a pixel in one Pass.
    pub pixel_samples_per_pass: NonZeroU32,
    /// The number of Pixel Samples of every pixel.
    pub pixel_samples_per_pixel: NonZeroU32,
}

impl Default for RenderJobOptions {
    /// Seed 0, 32 × 32 Tiles and Passes of 64 Pixel Samples (arc42 §8.8),
    /// and pbrt-v4's default of 16 Pixel Samples per pixel.
    fn default() -> Self {
        Self {
            seed: Seed(0),
            tile_size: NonZeroU32::new(32).expect("32 is not zero"),
            pixel_samples_per_pass: NonZeroU32::new(64).expect("64 is not zero"),
            pixel_samples_per_pixel: NonZeroU32::new(16).expect("16 is not zero"),
        }
    }
}
