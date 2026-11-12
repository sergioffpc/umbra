//! The final image of a Render Job, and how it is written, in the format
//! its path's extension names (arc42 §8.15, ADR 0020).

use std::io;
use std::path::Path;

use image::{ImageFormat, Rgb32FImage};
use umbra_math::Point2i;

use crate::atomic_file::write_atomically;

/// A finished Render Job's image: one output RGB per pixel.
#[derive(Debug, Clone, PartialEq)]
pub struct Image {
    resolution: Point2i,
    pixels: Vec<[f32; 3]>,
}

impl Image {
    /// The image of `resolution` with `pixels`, row by row.
    ///
    /// # Panics
    ///
    /// If there is not one pixel per pixel of `resolution`.
    #[must_use]
    pub fn new(resolution: Point2i, pixels: Vec<[f32; 3]>) -> Self {
        let [width, height] = [resolution.x, resolution.y].map(|side| usize::try_from(side).ok());
        assert_eq!(
            width.zip(height).map(|(w, h)| w * h),
            Some(pixels.len()),
            "one RGB per pixel of {resolution:?}"
        );
        Self { resolution, pixels }
    }

    /// The resolution, in pixels.
    #[must_use]
    pub fn resolution(&self) -> Point2i {
        self.resolution
    }

    /// The output RGB of every pixel, row by row.
    #[must_use]
    pub fn pixels(&self) -> &[[f32; 3]] {
        &self.pixels
    }

    /// Writes the image to `path`, replacing any file there, in the format
    /// its extension names. The `image` crate encodes it, writing EXR in one
    /// thread, so the file's bytes are a pure function of the image (ADR
    /// 0020).
    ///
    /// # Errors
    ///
    /// `InvalidInput` if the extension names no format `image` writes, and
    /// any I/O or encoding error, such as RGB floats in a format that
    /// cannot hold them; `path` is then left as it was.
    ///
    /// # Panics
    ///
    /// Never: [`Image::new`] checks that there is one RGB per pixel of an
    /// `i32` resolution.
    pub fn write(&self, path: &Path) -> io::Result<()> {
        let format = ImageFormat::from_path(path)
            .map_err(|error| io::Error::new(io::ErrorKind::InvalidInput, error))?;
        let (width, height) = self.size();
        let [width, height] = [width, height].map(|side| {
            u32::try_from(side).expect("an image's sides come from an `i32` resolution")
        });
        let buffer = Rgb32FImage::from_raw(width, height, self.pixels.as_flattened().to_vec())
            .expect("one RGB per pixel");
        write_atomically(path, |file| {
            buffer.write_to(file, format).map_err(io::Error::other)
        })
    }

    /// The width and height, as `usize`.
    fn size(&self) -> (usize, usize) {
        let [w, h] = [self.resolution.x, self.resolution.y]
            .map(|side| usize::try_from(side).expect("an image's sides are positive"));
        (w, h)
    }
}
