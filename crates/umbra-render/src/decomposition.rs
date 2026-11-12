//! How a Render Job is cut into Work Units: Tiles of the image times Passes
//! of the sample indices (arc42 §8.8, ADR 0009).

use std::error::Error;
use std::fmt;
use std::ops::Range;

use umbra_math::Point2i;
use umbra_scene::RenderJobOptions;

/// The index of a Tile, in raster order: left to right, then top to
/// bottom.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct TileIndex(pub u32);

/// The index of a Pass: Pass `p` is the same range of sample indices in
/// every pixel.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct PassIndex(pub u32);

/// One Tile rendered for one Pass: what a Worker is assigned, and what one
/// Work Result answers.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct WorkUnit {
    /// The Tile.
    pub tile: TileIndex,
    /// The Pass.
    pub pass: PassIndex,
}

/// The pixels of a Tile: `min` included, `max` excluded.
///
/// pbrt-v4 `Bounds2i`, as its tiles use it.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct TileBounds {
    /// The top-left pixel.
    pub min: Point2i,
    /// One past the bottom-right pixel.
    pub max: Point2i,
}

impl TileBounds {
    /// The number of pixels across.
    ///
    /// # Panics
    ///
    /// If `max` is left of `min`.
    #[must_use]
    pub fn width(&self) -> usize {
        usize::try_from(self.max.x - self.min.x).expect("a Tile is not inverted")
    }

    /// The number of pixels down.
    ///
    /// # Panics
    ///
    /// If `max` is above `min`.
    #[must_use]
    pub fn height(&self) -> usize {
        usize::try_from(self.max.y - self.min.y).expect("a Tile is not inverted")
    }

    /// The number of pixels.
    ///
    /// # Panics
    ///
    /// If the bounds are inverted.
    #[must_use]
    pub fn area(&self) -> usize {
        self.width() * self.height()
    }
}

/// Why a Render Job cannot be cut into Work Units.
#[derive(Debug, Clone, PartialEq, Eq)]
#[non_exhaustive]
pub enum RenderError {
    /// More Tiles than a `u32` counts.
    TooManyTiles,
}

impl fmt::Display for RenderError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::TooManyTiles => write!(f, "the image has more than 2^32 - 1 Tiles"),
        }
    }
}

impl Error for RenderError {}

/// The Tiles and Passes of a Render Job: its image and Render Job Options
/// fix them, never the Workers (arc42 §8.2).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Decomposition {
    resolution: Point2i,
    tile_size: i32,
    tiles_x: u32,
    tile_count: u32,
    samples_per_pass: u32,
    samples_per_pixel: u32,
}

impl Decomposition {
    /// The decomposition of an image of `resolution` pixels, both sides
    /// positive, by `options`.
    ///
    /// # Errors
    ///
    /// [`RenderError::TooManyTiles`] if the Tiles cannot be counted in a
    /// `u32`.
    ///
    /// # Panics
    ///
    /// If a side of `resolution` is not positive.
    pub fn new(resolution: Point2i, options: &RenderJobOptions) -> Result<Self, RenderError> {
        let tile_size =
            i32::try_from(options.tile_size.get()).map_err(|_| RenderError::TooManyTiles)?;
        let tiles = |side: i32| {
            u32::try_from(side)
                .expect("a resolution is positive")
                .div_ceil(options.tile_size.get())
        };
        let tiles_x = tiles(resolution.x);
        let tile_count = tiles_x
            .checked_mul(tiles(resolution.y))
            .ok_or(RenderError::TooManyTiles)?;
        Ok(Self {
            resolution,
            tile_size,
            tiles_x,
            tile_count,
            samples_per_pass: options.pixel_samples_per_pass.get(),
            samples_per_pixel: options.pixel_samples_per_pixel.get(),
        })
    }

    /// The image's resolution.
    #[must_use]
    pub fn resolution(&self) -> Point2i {
        self.resolution
    }

    /// The number of Tiles.
    #[must_use]
    pub fn tile_count(&self) -> u32 {
        self.tile_count
    }

    /// The number of Passes: the last one is short if the Pixel Samples
    /// per pixel are not a whole number of Passes.
    #[must_use]
    pub fn pass_count(&self) -> u32 {
        self.samples_per_pixel.div_ceil(self.samples_per_pass)
    }

    /// Whether `unit` is one of this Render Job's.
    #[must_use]
    pub fn contains(&self, unit: WorkUnit) -> bool {
        unit.tile.0 < self.tile_count && unit.pass.0 < self.pass_count()
    }

    /// Every Work Unit, Pass by Pass, each Pass's Tiles in raster order
    /// (ADR 0009).
    pub fn work_units(&self) -> impl Iterator<Item = WorkUnit> + use<> {
        let tile_count = self.tile_count;
        (0..self.pass_count()).flat_map(move |pass| {
            (0..tile_count).map(move |tile| WorkUnit {
                tile: TileIndex(tile),
                pass: PassIndex(pass),
            })
        })
    }

    /// The pixels of `tile`, cut short by the image at its right and bottom
    /// edges.
    ///
    /// # Panics
    ///
    /// If `tile` is not one of this Render Job's.
    #[must_use]
    pub fn tile_bounds(&self, tile: TileIndex) -> TileBounds {
        assert!(tile.0 < self.tile_count, "{tile:?} is not in the image");
        let column = i32::try_from(tile.0 % self.tiles_x).expect("a Tile column fits i32");
        let row = i32::try_from(tile.0 / self.tiles_x).expect("a Tile row fits i32");
        let min = Point2i::new(column * self.tile_size, row * self.tile_size);
        let max = Point2i::new(
            (min.x + self.tile_size).min(self.resolution.x),
            (min.y + self.tile_size).min(self.resolution.y),
        );
        TileBounds { min, max }
    }

    /// The sample indices of `pass`.
    #[must_use]
    pub fn sample_indices(&self, pass: PassIndex) -> Range<u32> {
        let start = pass.0 * self.samples_per_pass;
        start..(start + self.samples_per_pass).min(self.samples_per_pixel)
    }
}
