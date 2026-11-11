//! The hash of the sRGB RGB-to-spectrum table the build script generates
//! (ADR 0022): a table that changes with the generator, the toolchain or
//! glibc's `cbrt` (a bump of the image's Ubuntu digest) fails here.

use umbra_sampling::murmur_hash_64a;

/// The table as the build script writes it.
static SRGB_TABLE: &[u8] = include_bytes!(concat!(env!("OUT_DIR"), "/srgb_to_spectrum.bin"));

/// pbrt-v4 `MurmurHash64A` with seed 0 of the bytes of the table generated
/// with the cube root of glibc 2.43, the release image's (ADR 0022). Change
/// it only with the generator or the image's base, in the same commit, after
/// checking that every target computes the new value.
const SRGB_TABLE_HASH: u64 = 0xcf8f_bfb4_bd01_7562;

#[test]
fn srgb_table_hash() {
    let hash = murmur_hash_64a(SRGB_TABLE, 0);
    assert_eq!(
        hash, SRGB_TABLE_HASH,
        "the sRGB RGB-to-spectrum table hashes to {hash:#018x}"
    );
}
