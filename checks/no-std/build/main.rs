//! Rejects `alloc` in the core crates (ADR 0014) while `cargo check` builds
//! this crate. The library catches `std`, but linking `alloc` needs no
//! allocator until a final artifact is linked, so the sources are read
//! instead. Reruns whenever a core crate's `src/` changes.

mod scan;

use std::fs;
use std::path::{Path, PathBuf};

/// The core crates' sources that declare `extern crate alloc`.
fn files_linking_alloc(crates: &Path) -> Vec<PathBuf> {
    scan::CORE_CRATES
        .iter()
        .flat_map(|name| scan::rust_files(&crates.join(name).join("src")))
        .filter(|path| scan::links_alloc(&fs::read_to_string(path).expect("source file")))
        .collect()
}

#[expect(
    clippy::print_stdout,
    reason = "Cargo reads a build script's instructions from its stdout"
)]
fn main() {
    let crates = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../crates");
    for name in scan::CORE_CRATES {
        println!(
            "cargo::rerun-if-changed={}",
            crates.join(name).join("src").display()
        );
    }
    for path in files_linking_alloc(&crates) {
        println!(
            "cargo::error=a core crate links alloc (ADR 0014): {}",
            path.display()
        );
    }
}
