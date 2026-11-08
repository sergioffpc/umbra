//! pbrt-v4 `src/pbrt/util/hash_test.cpp`, and `hash.h` against pbrt-v4's
//! values.

use umbra_math::{Normal3f, Point2f, Point2i, Point3f, Vector3f};
use umbra_sampling::{hash, hash_float, mix_bits, murmur_hash_64a};

use super::known_values::{HASH, HASH_FLOAT, MIX_BITS, MURMUR_HASH_64A};

/// `TEST(Hash, VarArgs)`: `Hash` of one value is `HashBuffer` of its bytes.
#[test]
fn hash_var_args() {
    for v in [1_i64, -12511, 31_415_821, 37] {
        assert_eq!(murmur_hash_64a(&v.to_le_bytes(), 0), hash(v));
    }
}

/// `TEST(Hash, Collisions)`. pbrt-v4 never inserts into its sets, so only
/// its `same` count checks anything; this checks that, and that the full
/// 64-bit hashes of the first ten million `int`s are distinct, which is what
/// the test meant.
#[test]
fn hash_collisions() {
    let mut hashes: Vec<u64> = (0..10_000_000_i32).map(hash).collect();
    assert!(
        hashes.iter().zip(0_u64..).all(|(&h, i)| h != i),
        "a hash equals its input"
    );
    hashes.sort_unstable();
    let count = hashes.len();
    hashes.dedup();
    assert_eq!(count, hashes.len(), "two inputs hash to the same 64 bits");
}

/// `TEST(Hash, Unaligned)`: the hash depends on the bytes, not on where they
/// sit in memory.
#[test]
fn hash_unaligned() {
    let bytes: Vec<u8> = [0xface_beef_u64, 0x6502_8088, 0x1337_2048]
        .iter()
        .flat_map(|v| v.to_le_bytes())
        .collect();
    let expected = murmur_hash_64a(&bytes, 0);
    for delta in 0..8 {
        let mut shifted = vec![0_u8; bytes.len() + 8];
        shifted[delta..delta + bytes.len()].copy_from_slice(&bytes);
        assert_eq!(
            expected,
            murmur_hash_64a(&shifted[delta..delta + bytes.len()], 0)
        );
    }
}

/// `MixBits`, `MurmurHash64A`, `Hash` and `HashFloat` give pbrt-v4's values.
#[test]
fn known_values() {
    for &(v, expected) in MIX_BITS {
        assert_eq!(mix_bits(v), expected, "MixBits({v:#x})");
    }

    let key: Vec<u8> = (0..32_u8)
        .map(|i| i.wrapping_mul(37).wrapping_add(11))
        .collect();
    for &(len, seed, expected) in MURMUR_HASH_64A {
        assert_eq!(
            murmur_hash_64a(&key[..len], seed),
            expected,
            "MurmurHash64A of {len} bytes, seed {seed:#x}"
        );
    }

    let hashes = [
        hash(0_i32),
        hash(37_i32),
        hash(-12511_i64),
        hash(0xface_beef_u64),
        hash((Point2i::new(3, 7), 42_i32)),
        hash((0.25_f32, Point2f::new(0.5, 0.75))),
        hash((0_i32, Vector3f::new(0.1, -0.2, 0.3))),
        hash((Point2i::new(-1, 5), 9_i64, 3_i32)),
        hash((
            Point3f::new(1.0, 2.0, 3.0),
            Normal3f::new(0.0, 0.0, 1.0),
            7_u32,
        )),
    ];
    assert_eq!(hashes, HASH);

    let floats = [
        hash_float(0_i32),
        hash_float((Point3f::new(1.0, 2.0, 3.0), Vector3f::new(0.1, -0.2, 0.3))),
    ];
    assert_eq!(floats.map(f32::to_bits), HASH_FLOAT);
}
