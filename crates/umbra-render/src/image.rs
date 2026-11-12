//! The final image of a Render Job, and how it is written (arc42 §8.15,
//! ADR 0020).

use std::fmt;
use std::fs::{self, File};
use std::io::{self, BufWriter, Write};
use std::path::{Path, PathBuf};

use exr::prelude::{
    AttributeValue, Blocks, Compression, Encoding, Layer, LayerAttributes, LineOrder,
    SpecificChannels, Text, Vec2, WritableImage, f16,
};
use umbra_math::Point2i;

use crate::reproduction::ReproductionRecord;

/// The name of the EXR attribute that holds the Reproduction Record.
pub const REPRODUCTION_RECORD_ATTRIBUTE: &str = "umbra.reproductionRecord";

/// A finished Render Job's image: one output RGB per pixel, and the
/// Reproduction Record that made it.
#[derive(Debug, Clone, PartialEq)]
pub struct Image {
    resolution: Point2i,
    pixels: Vec<[f32; 3]>,
    record: ReproductionRecord,
}

impl Image {
    /// The image of `resolution` with `pixels`, row by row, made by the
    /// Render Job of `record`.
    ///
    /// # Panics
    ///
    /// If there is not one pixel per pixel of `resolution`.
    #[must_use]
    pub fn new(resolution: Point2i, pixels: Vec<[f32; 3]>, record: ReproductionRecord) -> Self {
        let [width, height] = [resolution.x, resolution.y].map(|side| usize::try_from(side).ok());
        assert_eq!(
            width.zip(height).map(|(w, h)| w * h),
            Some(pixels.len()),
            "one RGB per pixel of {resolution:?}"
        );
        Self {
            resolution,
            pixels,
            record,
        }
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

    /// The Reproduction Record.
    #[must_use]
    pub fn record(&self) -> &ReproductionRecord {
        &self.record
    }

    /// The width and height, as `usize`.
    fn size(&self) -> (usize, usize) {
        let [w, h] = [self.resolution.x, self.resolution.y]
            .map(|side| usize::try_from(side).expect("an image's sides are positive"));
        (w, h)
    }
}

/// The seam between a Render Job and an image format, called once per
/// image, so it is `dyn` (ADR 0006).
pub trait ImageWriter: fmt::Debug {
    /// Writes `image` to `path`, replacing any file there.
    ///
    /// # Errors
    ///
    /// Any I/O or encoding error; `path` is then left as it was.
    fn write(&self, image: &Image, path: &Path) -> io::Result<()>;
}

/// The OpenEXR writer: RGB as half floats, ZIP compression, increasing line
/// order and a fixed attribute order, so the file's bytes are a pure
/// function of the image (ADR 0020).
#[derive(Debug, Clone, Copy, Default)]
pub struct ExrWriter;

impl ImageWriter for ExrWriter {
    fn write(&self, image: &Image, path: &Path) -> io::Result<()> {
        let (width, _) = image.size();
        let channels = SpecificChannels::rgb(|Vec2(x, y): Vec2<usize>| {
            let [r, g, b] = image.pixels[y * width + x].map(f16::from_f32);
            (r, g, b)
        });
        let mut attributes = LayerAttributes::default();
        attributes.other.insert(
            Text::from_slice_unchecked(REPRODUCTION_RECORD_ATTRIBUTE.as_bytes()),
            AttributeValue::Text(Text::from_slice_unchecked(
                image.record.to_json().as_bytes(),
            )),
        );
        let encoding = Encoding {
            compression: Compression::ZIP16,
            blocks: Blocks::ScanLines,
            line_order: LineOrder::Increasing,
        };
        let layer = Layer::new(image.size(), attributes, encoding, channels);
        write_atomically(path, |file| {
            exr::image::Image::from_layer(layer)
                .write()
                .non_parallel()
                .to_buffered(file)
                .map_err(io::Error::other)
        })
    }
}

/// Writes a file at `path` with `write`, atomically: into a temporary file
/// in the same directory, synced to disk, then renamed over `path`. A
/// failure leaves `path` as it was.
///
/// # Errors
///
/// Any I/O error, or the one `write` returns.
pub fn write_atomically(
    path: &Path,
    write: impl FnOnce(&mut BufWriter<File>) -> io::Result<()>,
) -> io::Result<()> {
    let temporary = temporary_path(path)?;
    let written = write_and_sync(&temporary, write).and_then(|()| fs::rename(&temporary, path));
    if written.is_err() {
        // The error that matters is the write's; the temporary file may
        // not even exist.
        let _ = fs::remove_file(&temporary);
    }
    written?;
    File::open(directory(path))?.sync_all()
}

/// Creates `path` with `write`, and syncs it to disk.
fn write_and_sync(
    path: &Path,
    write: impl FnOnce(&mut BufWriter<File>) -> io::Result<()>,
) -> io::Result<()> {
    let mut writer = BufWriter::new(File::create(path)?);
    write(&mut writer)?;
    writer.flush()?;
    writer
        .into_inner()
        .map_err(io::IntoInnerError::into_error)?
        .sync_all()
}

/// The directory `path` is in.
fn directory(path: &Path) -> &Path {
    path.parent()
        .filter(|dir| !dir.as_os_str().is_empty())
        .unwrap_or_else(|| Path::new("."))
}

/// A hidden file next to `path`, named after it and this process.
fn temporary_path(path: &Path) -> io::Result<PathBuf> {
    let name = path
        .file_name()
        .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidInput, "the path names no file"))?;
    let mut temporary = std::ffi::OsString::from(".");
    temporary.push(name);
    temporary.push(format!(".{}.tmp", std::process::id()));
    Ok(directory(path).join(temporary))
}
