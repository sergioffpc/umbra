//! The citation check over a repository tree with a seeded uncited item.

use std::fs;
use std::path::{Path, PathBuf};

use xtask::{CORE_CRATES, Missing};

/// A fresh repository tree under the system temporary directory.
fn tree(name: &str) -> PathBuf {
    let root = std::env::temp_dir().join(format!("xtask-{name}-{}", std::process::id()));
    let _ = fs::remove_dir_all(&root);
    for krate in CORE_CRATES {
        write(
            &root,
            &format!("crates/{krate}/src/lib.rs"),
            "//! A core crate.\n",
        );
    }
    root
}

fn write(root: &Path, path: &str, text: &str) {
    let path = root.join(path);
    fs::create_dir_all(path.parent().expect("a file path has a parent")).expect("create dirs");
    fs::write(path, text).expect("write the fixture");
}

/// `umbra-math` with a cited `vector::Vector3` and an uncited
/// `vector::simd::seeded_uncited`, plus an uncited item in its `tests/`.
fn seeded_tree() -> PathBuf {
    let root = tree("seeded");
    write(
        &root,
        "crates/umbra-math/src/lib.rs",
        "//! Math.\npub mod vector;\n",
    );
    write(
        &root,
        "crates/umbra-math/src/vector.rs",
        "//! Vectors.\npub mod simd;\n/// PBR 4e §3.3, pbrt-v4 `Vector3`.\npub struct Vector3;\n",
    );
    write(
        &root,
        "crates/umbra-math/src/vector/simd.rs",
        "//! SIMD.\n\n/// Seeded.\npub fn seeded_uncited() {}\n",
    );
    write(
        &root,
        "crates/umbra-math/tests/vector.rs",
        "pub fn not_core() {}\n",
    );

    root
}

#[test]
fn a_seeded_uncited_item_fails_the_check() {
    let root = seeded_tree();

    let audit = xtask::audit_workspace(&root).expect("the tree is readable");

    let found: Vec<_> = audit
        .violations()
        .iter()
        .map(|v| (v.path().to_owned(), v.line(), v.item(), v.missing()))
        .collect();
    assert_eq!(
        found,
        [(
            PathBuf::from("crates/umbra-math/src/vector/simd.rs"),
            4,
            "umbra_math::vector::simd::seeded_uncited",
            Missing::SectionAndSymbol
        )]
    );
    assert!(
        audit
            .book_map()
            .contains("- `umbra_math::vector::Vector3`: pbrt-v4 `Vector3` in")
    );
    fs::remove_dir_all(root).expect("clean up");
}

#[test]
fn a_missing_core_crate_is_an_error() {
    let root = tree("missing");
    fs::remove_dir_all(root.join("crates/umbra-camera")).expect("remove a crate");

    assert!(
        xtask::audit_workspace(&root).is_err(),
        "umbra-camera is missing"
    );
    fs::remove_dir_all(root).expect("clean up");
}
