//! Finds `extern crate alloc` in the sources of the core crates (ADR 0014).
//! Shared by the build script and by `tests/scan.rs`.

use std::fs;
use std::path::{Path, PathBuf};

/// The seven core crates (ADR 0015), as directories under `crates/`.
pub(crate) const CORE_CRATES: [&str; 7] = [
    "umbra-math",
    "umbra-sampling",
    "umbra-spectrum",
    "umbra-geometry",
    "umbra-shading",
    "umbra-camera",
    "umbra-integrators",
];

/// Every `.rs` file under `dir`, recursively.
pub(crate) fn rust_files(dir: &Path) -> Vec<PathBuf> {
    let entries = fs::read_dir(dir).unwrap_or_else(|e| panic!("read {}: {e}", dir.display()));
    entries
        .map(|entry| entry.expect("directory entry").path())
        .flat_map(|path| {
            if path.is_dir() {
                rust_files(&path)
            } else if path.extension().is_some_and(|ext| ext == "rs") {
                vec![path]
            } else {
                Vec::new()
            }
        })
        .collect()
}

/// Whether `source` declares `extern crate alloc`, however it is spaced.
pub(crate) fn links_alloc(source: &str) -> bool {
    let words: Vec<&str> = source.split_whitespace().collect();
    words
        .windows(3)
        .any(|w| matches!(w, ["extern", "crate", name] if name.trim_end_matches(';') == "alloc"))
}
