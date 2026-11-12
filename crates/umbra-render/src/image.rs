//! The final image of a Render Job, and how it is written (arc42 §8.15,
//! ADR 0020).

use std::fmt;
use std::fs::{self, File};
use std::io::{self, BufWriter, Write};
use std::path::{Path, PathBuf};

use image::{ImageFormat, Rgb32FImage};
use umbra_math::Point2i;

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

/// The OpenEXR writer, through the `image` crate: RGB as 32-bit floats,
/// written in one thread, so the file's bytes are a pure function of the
/// image (ADR 0020).
#[derive(Debug, Clone, Copy, Default)]
pub struct ExrWriter;

impl ImageWriter for ExrWriter {
    fn write(&self, image: &Image, path: &Path) -> io::Result<()> {
        let (width, height) = image.size();
        let [width, height] = [width, height].map(|side| {
            u32::try_from(side).expect("an image's sides come from an `i32` resolution")
        });
        let buffer = Rgb32FImage::from_raw(width, height, image.pixels.as_flattened().to_vec())
            .expect("one RGB per pixel");
        write_atomically(path, |file| {
            buffer
                .write_to(file, ImageFormat::OpenExr)
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
