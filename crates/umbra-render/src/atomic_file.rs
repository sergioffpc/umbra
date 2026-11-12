//! Writing a file atomically: the final image and the Render Job Report
//! are either written whole or not at all (arc42 §8.15).

use std::fs::{self, File};
use std::io::{self, BufWriter, Write};
use std::path::{Path, PathBuf};

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
