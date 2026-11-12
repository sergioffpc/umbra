//! The Film of a Render Job, which merges Work Results as they arrive
//! (ADR 0022).

use std::collections::HashSet;
use std::fmt;

use umbra_math::SquareMatrix;
use umbra_scene::{RenderJobOptions, Scene};

use crate::decomposition::{Decomposition, WorkUnit};
use crate::error::Error;
use crate::image::Image;
use crate::work_result::{PixelSum, WorkResult};

/// What [`Film::accept`] did with a Work Result.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Acceptance {
    /// It was the first for its Work Unit, and is merged.
    Merged,
    /// Its Work Unit was already merged: it was discarded unread.
    Duplicate,
}

/// A hash of every pixel sum of a Film, to tell two Films apart.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct FilmHash(pub u64);

impl fmt::Display for FilmHash {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{:016x}", self.0)
    }
}

/// The single pixel accumulator of a Render Job: the `f64` sums of every
/// pixel (ADR 0001), into which each Work Result is merged as it arrives,
/// in any order (ADR 0022). At most one Work Result is merged per Work
/// Unit, so every Pixel Sample counts exactly once.
#[derive(Debug, Clone, PartialEq)]
pub struct Film {
    decomposition: Decomposition,
    pixels: Vec<PixelSum>,
    merged: HashSet<WorkUnit>,
    output_rgb_from_sensor_rgb: SquareMatrix<3>,
}

impl Film {
    /// The empty Film of a Render Job of `scene` with `options`.
    ///
    /// # Errors
    ///
    /// [`Error::TooManyTiles`] if the Tiles cannot be counted in a
    /// `u32`.
    ///
    /// # Panics
    ///
    /// Never for a built Scene, whose resolution is positive.
    pub fn new(scene: &Scene, options: &RenderJobOptions) -> Result<Self, Error> {
        let decomposition = Decomposition::new(scene.full_resolution(), options)?;
        let resolution = decomposition.resolution();
        let len = [resolution.x, resolution.y]
            .map(|side| usize::try_from(side).expect("a resolution is positive"))
            .iter()
            .product();
        Ok(Self {
            decomposition,
            pixels: vec![PixelSum::default(); len],
            merged: HashSet::new(),
            output_rgb_from_sensor_rgb: *scene.output_rgb_from_sensor_rgb(),
        })
    }

    /// Merges `result` if it is the first for its Work Unit; otherwise
    /// discards it unread.
    ///
    /// # Panics
    ///
    /// If `result` answers no Work Unit of this Render Job, or does not
    /// hold one sum per pixel of its Tile.
    pub fn accept(&mut self, result: &WorkResult) -> Acceptance {
        let unit = result.unit();
        assert!(
            self.decomposition.contains(unit),
            "{unit:?} is not in this Render Job"
        );
        if self.merged.contains(&unit) {
            return Acceptance::Duplicate;
        }
        let bounds = self.decomposition.tile_bounds(unit.tile);
        assert_eq!(
            result.pixels().len(),
            bounds.area(),
            "one sum per pixel of {unit:?}"
        );
        self.merged.insert(unit);
        let width = self.width();
        let rows = result.pixels().chunks_exact(bounds.width());
        for (y, row) in (bounds.min.y..).zip(rows) {
            let start = Self::offset(width, bounds.min.x, y);
            merge_row(&mut self.pixels[start..start + row.len()], row);
        }
        Acceptance::Merged
    }

    /// Whether every Work Unit of the Render Job has been merged.
    #[must_use]
    pub fn is_complete(&self) -> bool {
        self.merged.len() == self.decomposition.work_units().count()
    }

    /// The sums of every pixel, row by row across the image.
    #[must_use]
    pub fn pixels(&self) -> &[PixelSum] {
        &self.pixels
    }

    /// A hash of every pixel sum, FNV-1a over the bits of their `f64`s in
    /// raster order: equal Films have equal hashes, and Films that differ in
    /// any bit very likely do not.
    #[must_use]
    pub fn hash(&self) -> FilmHash {
        const OFFSET_BASIS: u64 = 0xcbf2_9ce4_8422_2325;
        const PRIME: u64 = 0x0100_0000_01b3;
        let bytes = self.pixels.iter().flat_map(|p| {
            [p.rgb[0], p.rgb[1], p.rgb[2], p.weight]
                .into_iter()
                .flat_map(|v| v.to_bits().to_le_bytes())
        });
        FilmHash(bytes.fold(OFFSET_BASIS, |h, b| (h ^ u64::from(b)).wrapping_mul(PRIME)))
    }

    /// The final image: each pixel's weighted sensor RGB over its weight, in
    /// the output colour space.
    ///
    /// pbrt-v4 `RGBFilm::GetPixelRGB`.
    ///
    /// # Panics
    ///
    /// If a Work Unit is not merged yet: an image exists only once every
    /// Pass is rendered (ADR 0004).
    #[must_use]
    pub fn finish(self) -> Image {
        assert!(self.is_complete(), "every Work Unit is merged");
        let pixels = self
            .pixels
            .iter()
            .map(|sum| self.output_rgb_from_sensor_rgb * sum.sensor_rgb())
            .collect();
        Image::new(self.decomposition.resolution(), pixels)
    }

    /// The image's width, in pixels.
    fn width(&self) -> usize {
        usize::try_from(self.decomposition.resolution().x).expect("a resolution is positive")
    }

    /// The index in `pixels` of pixel `(x, y)`.
    fn offset(width: usize, x: i32, y: i32) -> usize {
        let [x, y] = [x, y].map(|c| usize::try_from(c).expect("a pixel is in the image"));
        y * width + x
    }
}

/// Adds each sum of `row` to the matching one of `film_row`.
fn merge_row(film_row: &mut [PixelSum], row: &[PixelSum]) {
    for (pixel, &sum) in film_row.iter_mut().zip(row) {
        *pixel += sum;
    }
}
