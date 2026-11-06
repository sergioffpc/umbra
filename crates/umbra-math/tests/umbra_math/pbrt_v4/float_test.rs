//! pbrt-v4 `src/pbrt/util/float_test.cpp`.

use crate::rng::Rng;
use umbra_math::{exponent, next_float_down, next_float_up, significand};

/// `TEST(FloatingPoint, Pieces)`.
#[test]
fn floating_point_pieces() {
    assert_eq!(1, exponent(2.0));
    assert_eq!(-1, exponent(0.5));
    assert_eq!(0b100_0000_0000_0000_0000_0000, significand(3.0));
    assert_eq!(0b110_0000_0000_0000_0000_0000, significand(7.0));
}

fn get_float(rng: &mut Rng) -> f32 {
    loop {
        let f = f32::from_bits(rng.uniform_u32());
        if !f.is_nan() {
            return f;
        }
    }
}

/// `TEST(FloatingPoint, NextUpDownFloat)`. `f32::next_up`/`next_down` stand in
/// for `std::nextafter`.
#[test]
fn floating_point_next_up_down_float() {
    next_up_down_special_values();
    let mut rng = Rng::default();
    for _ in 0..100_000 {
        let f = get_float(&mut rng);
        if f.is_finite() {
            assert_eq!(f.next_up().to_bits(), next_float_up(f).to_bits(), "{f}");
            assert_eq!(f.next_down().to_bits(), next_float_down(f).to_bits(), "{f}");
        }
    }
}

fn next_up_down_special_values() {
    assert!(next_float_up(-0.0) > 0.0, "up from -0");
    assert!(next_float_down(0.0) < 0.0, "down from +0");
    assert_eq!(next_float_up(f32::INFINITY), f32::INFINITY, "up from +inf");
    assert!(
        next_float_down(f32::INFINITY) < f32::INFINITY,
        "down from +inf"
    );
    assert_eq!(
        next_float_down(f32::NEG_INFINITY),
        f32::NEG_INFINITY,
        "down from -inf"
    );
    assert!(
        next_float_up(f32::NEG_INFINITY) > f32::NEG_INFINITY,
        "up from -inf"
    );
}

/// Not in pbrt-v4's tests: NaN bit patterns step as pbrt-v4's unsigned
/// arithmetic does, wrapping instead of overflowing.
#[test]
fn next_up_down_nan_bits_wrap() {
    assert_eq!(0, next_float_down(f32::from_bits(0xffff_ffff)).to_bits());
    assert_eq!(
        0x7fff_fffe,
        next_float_up(f32::from_bits(0x7fff_ffff)).to_bits()
    );
}
