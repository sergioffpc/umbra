//! Generates the sRGB RGB-to-spectrum table (ADR 0002, arc42 §8.1).
//!
//! The Jakob–Hanika coefficient table is about 2.4 MB of `f32`, so it is
//! computed here rather than committed: [`rgb2spec`] ports pbrt-v4's
//! `rgb2spec_opt`, and the table is written to `OUT_DIR` as
//! `srgb_to_spectrum.bin`, little-endian `f32`: the 64 `z` nodes, then the
//! `3 × 64 × 64 × 64 × 3` coefficients. `src/rgb_to_spectrum.rs` includes
//! it. A test checks its hash on every CI target.

mod cie_5nm;
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

    let srgb = Gamut {
        illuminant: CIE_D65.map(|v| v / CIE_D65_NORMALIZATION),
        xyz_to_rgb: XYZ_TO_SRGB,
        rgb_to_xyz: SRGB_TO_XYZ,
    };
    let table = rgb2spec::optimise(&srgb);

    let out = PathBuf::from(env::var_os("OUT_DIR").expect("Cargo sets OUT_DIR"));
    fs::write(out.join("srgb_to_spectrum.bin"), to_le_bytes(&table)).expect("OUT_DIR is writable");
}
