//! What a Work Unit produces (ADR 0001, ADR 0022).

use std::ops::AddAssign;

use umbra_integrators::FilmSample;

use crate::decomposition::WorkUnit;

/// The sums of one pixel's Pixel Samples, in `f64` (ADR 0001): the
/// filter-weighted sensor RGB and the filter weights.
///
/// pbrt-v4 `RGBFilm::RGBPixel`.
#[derive(Debug, Clone, Copy, PartialEq, Default)]
pub struct PixelSum {
    /// The sum of `weight · rgb` over the Pixel Samples, red, green, blue.
    pub rgb: [f64; 3],
    /// The sum of the filter weights.
    pub weight: f64,
}

impl PixelSum {
    /// The pixel's sensor RGB: the weighted sum over the sum of weights,
    /// both narrowed to `f32` first, or the sum itself when no weight was
    /// added.
    ///
    /// pbrt-v4 `RGBFilm::GetPixelRGB`.
    #[must_use]
    #[expect(
        clippy::cast_possible_truncation,
        reason = "pbrt-v4 narrows its double sums to Float"
    )]
    pub fn sensor_rgb(&self) -> [f32; 3] {
        let rgb = self.rgb.map(|c| c as f32);
        let weight = self.weight as f32;
        if weight == 0.0 {
            rgb
        } else {
            rgb.map(|c| c / weight)
        }
    }
}

impl AddAssign<FilmSample> for PixelSum {
    /// Adds one Pixel Sample, as pbrt-v4 does: `weight · rgb` in `f32`,
    /// then summed in `f64`.
    ///
    /// pbrt-v4 `RGBFilm::AddSample`.
    fn add_assign(&mut self, sample: FilmSample) {
        let FilmSample { rgb, weight } = sample;
        for (sum, c) in self.rgb.iter_mut().zip([rgb.r, rgb.g, rgb.b]) {
            *sum += f64::from(weight * c);
        }
        self.weight += f64::from(weight);
    }
}

impl AddAssign for PixelSum {
    /// Merges another Work Result's sums into these.
    fn add_assign(&mut self, other: Self) {
        for (sum, c) in self.rgb.iter_mut().zip(other.rgb) {
            *sum += c;
        }
        self.weight += other.weight;
    }
}

/// The unnormalised contributions of exactly one Work Unit: the
/// [`PixelSum`] of each pixel of its Tile, over the sample indices of its
/// Pass, row by row.
#[derive(Debug, Clone, PartialEq)]
pub struct WorkResult {
    unit: WorkUnit,
    pixels: Vec<PixelSum>,
}

impl WorkResult {
    /// The Work Result of `unit` with these pixel sums, row by row across
    /// its Tile.
    #[must_use]
    pub fn new(unit: WorkUnit, pixels: Vec<PixelSum>) -> Self {
        Self { unit, pixels }
    }

    /// The Work Unit it answers.
    #[must_use]
    pub fn unit(&self) -> WorkUnit {
        self.unit
    }

    /// The pixel sums, row by row across the Tile.
    #[must_use]
    pub fn pixels(&self) -> &[PixelSum] {
        &self.pixels
    }
}
