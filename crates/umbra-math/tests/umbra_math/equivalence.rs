//! The optimised kernels against `crate::reference` (ADR 0007): for
//! every operation the crate also provides as a safe, book-faithful scalar
//! reference, the two must give the same bits for every input.
//!
//! Inputs mix the special values (signed zeros, infinities, NaNs of both
//! signs, the subnormal and normal extremes), random bit patterns, and
//! values of the magnitudes a renderer meets. Results are compared bit for
//! bit; a NaN only has to be a NaN, since the payload of a NaN computed from
//! non-NaN inputs is the CPU's default and outside the contract of the crate.
//! Operations that select rather than compute (`next_float_up`,
//! `round_up_pow2`) are compared bit for bit, NaN included.
//!
//! The `Interval` operations and everything built on them get finite inputs
//! of bounded magnitude only ([`finite_samples`]): their outward rounding
//! steps the bits of its operand as pbrt-v4's `NextFloatUp`/`NextFloatDown`
//! do, which for a NaN depends on its payload (a NaN whose payload is all
//! ones steps to a zero), and LLVM may give a NaN it computes any payload,
//! differently in each place the code is inlined. A NaN bound is garbage in,
//! outside the contract, in the reference as much as in the kernels.

mod float;
mod interval;
mod square_matrix;
mod transform;

/// The random samples per test: enough to cover the exponent range many
/// times over natively, a sample of that under Miri, which interprets the
/// code a few hundred times slower.
pub(crate) const COUNT: usize = if cfg!(miri) { 1_000 } else { 200_000 };

/// The stride of the sweeps over every `u32`.
pub(crate) const STRIDE: u32 = if cfg!(miri) { 1 << 19 } else { 251 };

use umbra_math::Interval;

use crate::rng::Rng;

/// The values every kernel is tried on, besides random ones.
pub(crate) const SPECIAL: [f32; 18] = [
    0.0,
    -0.0,
    1.0,
    -1.0,
    0.5,
    -2.5,
    f32::from_bits(0x0000_0001),
    f32::from_bits(0x8000_0001),
    f32::MIN_POSITIVE,
    1e-40,
    f32::MAX,
    f32::MIN,
    f32::INFINITY,
    f32::NEG_INFINITY,
    f32::NAN,
    f32::from_bits(0x7fc0_1234),
    f32::from_bits(0xffc0_0001),
    f32::from_bits(0x7f80_0001),
];

/// The finite special values: those of [`SPECIAL`] whose products and
/// quotients with values of bounded magnitude never overflow to a NaN.
pub(crate) const SPECIAL_FINITE: [f32; 10] = [
    0.0,
    -0.0,
    1.0,
    -1.0,
    0.5,
    -2.5,
    f32::from_bits(0x0000_0001),
    f32::from_bits(0x8000_0001),
    f32::MIN_POSITIVE,
    1e-40,
];

/// A value of renderer magnitude: uniform in `[−1, 1)` scaled by a power of
/// two in `[2⁻²⁴, 2²⁴]`.
fn moderate(rng: &mut Rng) -> f32 {
    let magnitude = f32::from_bits((rng.uniform_u32() % 48 + 103) << 23);
    (rng.uniform_f32() * 2.0 - 1.0) * magnitude
}

/// A uniformly chosen entry of `table`.
fn pick<const N: usize>(rng: &mut Rng, table: &[f32; N]) -> f32 {
    table[rng.uniform_u32() as usize % N]
}

/// One sample: a special value, random bits, or a value of renderer
/// magnitude, by turns.
pub(crate) fn sample(rng: &mut Rng, k: usize) -> f32 {
    match k % 4 {
        0 => pick(rng, &SPECIAL),
        1 => f32::from_bits(rng.uniform_u32()),
        _ => moderate(rng),
    }
}

/// One finite sample: a finite special value or a value of renderer
/// magnitude, by turns.
pub(crate) fn finite_sample(rng: &mut Rng, k: usize) -> f32 {
    if k.is_multiple_of(4) {
        pick(rng, &SPECIAL_FINITE)
    } else {
        moderate(rng)
    }
}

/// `n` samples.
pub(crate) fn samples(seed: u64, n: usize) -> Vec<f32> {
    let mut rng = Rng::new(seed);
    (0..n).map(|k| sample(&mut rng, k)).collect()
}

/// `n` finite samples.
pub(crate) fn finite_samples(seed: u64, n: usize) -> Vec<f32> {
    let mut rng = Rng::new(seed);
    (0..n).map(|k| finite_sample(&mut rng, k)).collect()
}

/// `n` intervals with finite bounds: exact ones, ones with a small error,
/// and ones between two arbitrary samples, by turns.
pub(crate) fn intervals(seed: u64, n: usize) -> Vec<Interval> {
    let mut rng = Rng::new(seed);
    (0..n)
        .map(|k| {
            let v = finite_sample(&mut rng, k);
            match k % 3 {
                0 => Interval::from(v),
                1 => {
                    let err = v.abs() * rng.uniform_f32() * 1e-5;
                    Interval::from_value_and_error(v, err)
                }
                _ => Interval::new(v, finite_sample(&mut rng, k + 1)),
            }
        })
        .collect()
}

/// Bit-identical, or both NaN.
pub(crate) fn same(a: f32, b: f32) -> bool {
    a.to_bits() == b.to_bits() || (a.is_nan() && b.is_nan())
}

/// Both bounds [`same`].
pub(crate) fn same_interval(a: Interval, b: Interval) -> bool {
    same(a.lower_bound(), b.lower_bound()) && same(a.upper_bound(), b.upper_bound())
}
