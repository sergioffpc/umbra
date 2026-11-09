//! No core crate links `alloc` (ADR 0014). The library of this crate catches
//! `std`, but linking `alloc` needs no allocator until a final artifact is
//! built, so this test reads the sources of the seven core crates instead.

use std::fs;
use std::path::{Path, PathBuf};

const CORE_CRATES: [&str; 7] = [
    "umbra-math",
    "umbra-sampling",
    "umbra-spectrum",
    "umbra-geometry",
    "umbra-shading",
    "umbra-camera",
    "umbra-integrators",
];

/// Every `.rs` file under `dir`, recursively.
fn rust_files(dir: &Path) -> Vec<PathBuf> {
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
fn links_alloc(source: &str) -> bool {
    let words: Vec<&str> = source.split_whitespace().collect();
    words
        .windows(3)
        .any(|w| matches!(w, ["extern", "crate", name] if name.trim_end_matches(';') == "alloc"))
}

#[test]
fn no_core_crate_links_alloc() {
    let crates = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../crates");
    let offenders: Vec<PathBuf> = CORE_CRATES
        .iter()
        .flat_map(|name| rust_files(&crates.join(name).join("src")))
        .filter(|path| links_alloc(&fs::read_to_string(path).expect("source file")))
        .collect();
    assert!(
        offenders.is_empty(),
        "core crates link alloc (ADR 0014): {offenders:?}"
    );
}

#[test]
fn links_alloc_matches_the_declaration() {
    assert!(links_alloc("extern crate alloc;"));
    assert!(links_alloc("extern  crate\nalloc ;"));
    assert!(!links_alloc("extern crate core;"));
    assert!(!links_alloc("use alloc_free::thing;"));
}
