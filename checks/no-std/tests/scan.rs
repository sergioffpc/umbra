//! The source scan of the build script.

#[path = "../build/scan.rs"]
mod scan;

#[test]
fn links_alloc_matches_the_declaration() {
    assert!(scan::links_alloc("extern crate alloc;"));
    assert!(scan::links_alloc("extern  crate\nalloc ;"));
    assert!(!scan::links_alloc("extern crate core;"));
    assert!(!scan::links_alloc("use alloc_free::thing;"));
}

/// The scan reaches every core crate: a wrong path would make the build
/// script pass without reading anything.
#[test]
fn rust_files_finds_every_core_lib() {
    let crates = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../../crates");
    for name in scan::CORE_CRATES {
        let files = scan::rust_files(&crates.join(name).join("src"));
        assert!(
            files.iter().any(|path| path.ends_with("src/lib.rs")),
            "no src/lib.rs found for {name}"
        );
    }
}
