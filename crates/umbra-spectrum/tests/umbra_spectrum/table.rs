//! The hash of the sRGB RGB-to-spectrum table the build script generates
//! (ADR 0022): a table that changes with the generator, the toolchain or
//! `libm` fails here.

use umbra_sampling::murmur_hash_64a;

/// The table as the build script writes it.
static SRGB_TABLE: &[u8] = include_bytes!(concat!(env!("OUT_DIR"), "/srgb_to_spectrum.bin"));

/// pbrt-v4 `MurmurHash64A` with seed 0 of the bytes of the table generated
/// at the commit that introduced it. Change it only with the generator, in
/// the same commit, after checking that every target computes the new
/// value.
const SRGB_TABLE_HASH: u64 = 0xee0e_883e_1ac1_d488;

#[test]
fn srgb_table_hash() {
    let hash = murmur_hash_64a(SRGB_TABLE, 0);
    assert_eq!(
        hash, SRGB_TABLE_HASH,
        "the sRGB RGB-to-spectrum table hashes to {hash:#018x}"
    );
}
