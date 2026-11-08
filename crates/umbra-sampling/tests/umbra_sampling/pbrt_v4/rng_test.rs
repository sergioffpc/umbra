//! pbrt-v4 `src/pbrt/util/rng_test.cpp`, and `rng.h` against pbrt-v4's
//! values. `TEST(RNG, Double)` is not ported: Umbra has no `f64` samples
//! (ADR 0001).

use umbra_sampling::{Rng, Uniform};

use super::known_values::{
    RNG_ADVANCE, RNG_BELOW, RNG_DEFAULT_F32, RNG_DEFAULT_I32, RNG_DEFAULT_I64, RNG_DEFAULT_U32,
    RNG_DEFAULT_U64, RNG_OFFSET_U32, RNG_SEQUENCE_U32,
};

/// `TEST(RNG, Reseed)`.
#[test]
fn rng_reseed() {
    let mut rng = Rng::new(1234);
    let values: Vec<u32> = (0..100).map(|_| rng.uniform()).collect();

    rng.set_sequence(1234);
    for &v in &values {
        assert_eq!(v, rng.uniform::<u32>());
    }
}

/// `TEST(RNG, Advance)`.
#[test]
fn rng_advance() {
    let mut rng = Rng::default();
    rng.set_sequence_with_offset(1234, 6502);
    let v: Vec<f32> = (0..1000).map(|_| rng.uniform()).collect();

    rng.set_sequence_with_offset(1234, 6502);
    rng.advance(16);
    assert_eq!(rng.uniform::<f32>(), v[16]);

    for (i, &vi) in v.iter().enumerate().rev() {
        rng.set_sequence_with_offset(1234, 6502);
        rng.advance(i64::try_from(i).unwrap());
        assert_eq!(rng.uniform::<f32>(), vi);
    }

    // Switch to another sequence, then go back and check one last time.
    rng.set_sequence(32);
    let _: f32 = rng.uniform();
    for i in [5, 998, 552, 37, 16] {
        rng.set_sequence_with_offset(1234, 6502);
        rng.advance(i);
        assert_eq!(rng.uniform::<f32>(), v[usize::try_from(i).unwrap()]);
    }
}

/// `TEST(RNG, OperatorMinus)`.
#[test]
fn rng_operator_minus() {
    let mut ra = Rng::new(1337);
    let mut rb = Rng::new(1337);
    let mut rng = Rng::default();
    for _ in 0..10 {
        let step = 1 + rng.uniform_below(1000_u32);
        for _ in 0..step {
            let _: u32 = ra.uniform();
        }
        let step = i64::from(step);
        assert_eq!(step, &ra - &rb);
        assert_eq!(-step, &rb - &ra);

        // Resynchronise them.
        if rng.uniform::<u32>() & 1 != 0 {
            rb.advance(step);
        } else {
            ra.advance(-step);
        }
        assert_eq!(0, &ra - &rb);
        assert_eq!(0, &rb - &ra);
    }
}

/// `TEST(RNG, Int)`.
#[test]
fn rng_int() {
    let mut rng = Rng::default();
    let count = 10_000;
    let values: Vec<i32> = (0..count).map(|_| rng.uniform()).collect();
    let positive = values.iter().filter(|&&v| v > 0).count();
    let negative = values.iter().filter(|&&v| v < 0).count();
    let zero = values.iter().filter(|&&v| v == 0).count();

    assert!((4_800..5_200).contains(&positive), "{positive} positive");
    assert!((4_800..5_200).contains(&negative), "{negative} negative");
    assert!(zero < 10, "{zero} zero");
}

/// `TEST(RNG, Uint64)`.
#[test]
fn rng_uint64() {
    let mut rng = Rng::default();
    let count = 10_000;
    let mut bit_counts = [0_u32; 64];
    for _ in 0..count {
        let v: u64 = rng.uniform();
        for (b, n) in bit_counts.iter_mut().enumerate() {
            *n += u32::from(v & (1 << b) != 0);
        }
    }
    for (b, &n) in bit_counts.iter().enumerate() {
        assert!((4_800..5_200).contains(&n), "bit {b} set {n} times");
    }
}

/// `RNG`'s constructors give pbrt-v4's streams.
#[test]
fn known_values_streams() {
    let mut rng = Rng::default();
    for &expected in RNG_DEFAULT_U32 {
        assert_eq!(rng.uniform::<u32>(), expected);
    }

    for &(sequence_index, expected) in RNG_SEQUENCE_U32 {
        let mut rng = Rng::new(sequence_index);
        assert_eq!(
            [(); 4].map(|()| rng.uniform::<u32>()),
            expected,
            "RNG({sequence_index:#x})"
        );
    }

    let mut rng = Rng::with_offset(1234, 6502);
    for &expected in RNG_OFFSET_U32 {
        assert_eq!(rng.uniform::<u32>(), expected);
    }

    let mut rng = Rng::new(77);
    for &(delta, expected) in RNG_ADVANCE {
        rng.advance(delta);
        assert_eq!(rng.uniform::<u32>(), expected, "after Advance({delta})");
    }
}

/// Asserts that `RNG()` draws `expected`, as values of type `T`.
#[track_caller]
fn assert_default_stream<T: Uniform + PartialEq + core::fmt::Debug>(expected: &[T]) {
    let mut rng = Rng::default();
    for e in expected {
        assert_eq!(&rng.uniform::<T>(), e, "RNG() draws pbrt-v4's values");
    }
}

/// `Uniform<T>()` for each type and `Uniform<uint32_t>(b)` give pbrt-v4's
/// values.
#[test]
fn known_values_types() {
    let mut rng = Rng::default();
    for &expected in RNG_DEFAULT_F32 {
        assert_eq!(rng.uniform::<f32>().to_bits(), expected);
    }
    assert_default_stream(RNG_DEFAULT_U64);
    assert_default_stream(RNG_DEFAULT_I32);
    assert_default_stream(RNG_DEFAULT_I64);

    let mut rng = Rng::new(7);
    for &(b, expected) in RNG_BELOW {
        assert_eq!(
            [(); 4].map(|()| rng.uniform_below(b)),
            expected,
            "Uniform({b})"
        );
    }
}
