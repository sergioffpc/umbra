//! Generates the sRGB RGB-to-spectrum table (ADR 0022, arc42 §8.1).
//!
//! The Jakob–Hanika coefficient table is about 2.4 MB of `f32`, so it is
//! computed here rather than committed: [`rgb2spec`] ports pbrt-v4's
//! `rgb2spec_opt`, and the table is written to `OUT_DIR` as
//! `srgb_to_spectrum.bin`, little-endian `f32`: the 64 `z` nodes, then the
//! `3 × 64 × 64 × 64 × 3` coefficients. `src/rgb_to_spectrum.rs` includes
//! it. A test checks its hash on every CI target.

mod cie_5nm;
// glibc's C math through `umbra-math`'s own wrappers (ADR 0007),
// compiled here from their source rather than taken from the crate: a
// build-dependency on `umbra-math` would need its x86-64-v3 baseline on the
// host, and builds with `--target` (the `asan` group of `scripts/check.sh`)
// compile build scripts without RUSTFLAGS. The file depends on `core` only.
// The script calls `cbrt` alone, and the wrappers are `pub` for the crate.
#[path = "../../umbra-math/src/platform/x86_64.rs"]
#[expect(
    dead_code,
    unreachable_pub,
    reason = "the wrappers of umbra-math's public interface, of which the script calls one"
)]
mod glibc;
mod rgb2spec;

use std::env;
use std::fs;
use std::path::PathBuf;

use cie_5nm::{CIE_D65, CIE_D65_NORMALIZATION, SRGB_TO_XYZ, XYZ_TO_SRGB};
use rgb2spec::{Gamut, Table};

/// The table's bytes: the `z` nodes, then the coefficients, little-endian.
fn to_le_bytes(table: &Table) -> Vec<u8> {
    table
        .scale
        .iter()
        .chain(&table.data)
        .flat_map(|v| v.to_le_bytes())
        .collect()
}

#[expect(
    clippy::print_stdout,
    reason = "Cargo reads a build script's instructions from its stdout"
)]
fn main() {
    println!("cargo::rerun-if-changed=build");
    println!("cargo::rerun-if-changed=../umbra-math/src/platform/x86_64.rs");

    let srgb = Gamut {
        illuminant: CIE_D65.map(|v| v / CIE_D65_NORMALIZATION),
        xyz_to_rgb: XYZ_TO_SRGB,
        rgb_to_xyz: SRGB_TO_XYZ,
    };
    let table = rgb2spec::optimise(&srgb);

    let out = PathBuf::from(env::var_os("OUT_DIR").expect("Cargo sets OUT_DIR"));
    fs::write(out.join("srgb_to_spectrum.bin"), to_le_bytes(&table)).expect("OUT_DIR is writable");
}
