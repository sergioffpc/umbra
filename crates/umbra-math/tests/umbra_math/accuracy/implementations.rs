//! Every implementation that `benches/transcendentals.rs` times, on the
//! bench's own inputs, against glibc's `f64` routines: each result within
//! [`BOUND`] ULPs, the bound Umbra's functions document, except `libmvec`'s,
//! which glibc documents to 4 ULPs and which measure 2 for `atan2f` and
//! `expf`. For the glibc implementations, this catches a wrong FFI
//! signature, a mixed-up lane order or a wrong chunking of the batch into
//! calls, all of which are off by far more.

use crate::transcendentals::{
    BATCH, GAMMA, GlibcScalar, Transcendentals, Umbra, angles, atan2_args, erf_args,
    optical_depths, reflectances,
};
use umbra_math::Vec4;
use umbra_math::double;

use super::{BOUND, narrow, ulps};

/// The largest error over `(argument, ours, reference)` triples, with an
/// argument where it occurs.
fn worst(errors: impl Iterator<Item = (f32, f32, f32)>) -> (u32, f32) {
    errors
        .map(|(x, ours, reference)| (ulps(ours, reference), x))
        .max_by_key(|&(e, _)| e)
        .unwrap_or((0, 0.0))
}

/// The lanes of a batch of `Vec4`s, in order.
fn lanes(v: &[Vec4]) -> impl Iterator<Item = f32> + '_ {
    v.iter().flat_map(|v| v.to_array())
}

/// Asserts that `K`'s results on the bench's inputs are within `bound` ULPs.
#[track_caller]
fn assert_within_bound<K: Transcendentals>(what: &str, bound: u32) {
    for (name, (error, x)) in [
        ("sin_cos", sin_cos_error::<K>()),
        ("atan2", atan2_error::<K>()),
        ("erf", erf_error::<K>()),
        ("exp4", exp4_error::<K>()),
        ("pow4", pow4_error::<K>()),
    ] {
        assert!(
            error <= bound,
            "{what} {name}: {error} ULPs at {x:e} ({:#010x})",
            x.to_bits()
        );
    }
}

fn sin_cos_error<K: Transcendentals>() -> (u32, f32) {
    let x = angles();
    let (mut sin, mut cos) = (vec![0.0; BATCH], vec![0.0; BATCH]);
    K::sin_cos(&x, &mut sin, &mut cos);
    let sines = x
        .iter()
        .zip(&sin)
        .map(|(&x, &s)| (x, s, narrow(double::sin(f64::from(x)))));
    let cosines = x
        .iter()
        .zip(&cos)
        .map(|(&x, &c)| (x, c, narrow(double::cos(f64::from(x)))));
    worst(sines.chain(cosines))
}

/// Reports the `y` argument where the error occurs.
fn atan2_error<K: Transcendentals>() -> (u32, f32) {
    let (y, x) = atan2_args();
    let mut out = vec![0.0; BATCH];
    K::atan2(&y, &x, &mut out);
    worst(
        y.iter()
            .zip(&x)
            .zip(&out)
            .map(|((&y, &x), &r)| (y, r, narrow(double::atan2(f64::from(y), f64::from(x))))),
    )
}

fn erf_error<K: Transcendentals>() -> (u32, f32) {
    let x = erf_args();
    let mut out = vec![0.0; BATCH];
    K::erf(&x, &mut out);
    worst(
        x.iter()
            .zip(&out)
            .map(|(&x, &r)| (x, r, narrow(double::erf(f64::from(x))))),
    )
}

fn exp4_error<K: Transcendentals>() -> (u32, f32) {
    let v = optical_depths();
    let mut out = vec![Vec4::ZERO; BATCH];
    K::exp4(&v, &mut out);
    worst(
        lanes(&v)
            .zip(lanes(&out))
            .map(|(x, r)| (x, r, narrow(double::exp(f64::from(x))))),
    )
}

fn pow4_error<K: Transcendentals>() -> (u32, f32) {
    let v = reflectances();
    let mut out = vec![Vec4::ZERO; BATCH];
    K::pow4(&v, GAMMA, &mut out);
    worst(
        lanes(&v)
            .zip(lanes(&out))
            .map(|(x, r)| (x, r, narrow(double::pow(f64::from(x), f64::from(GAMMA))))),
    )
}

#[test]
fn glibc_scalar_within_bound() {
    assert_within_bound::<GlibcScalar>("glibc scalar", BOUND);
}

#[test]
fn umbra_within_bound() {
    assert_within_bound::<Umbra>("Umbra", BOUND);
}

/// The `libmvec` implementations, where the bench has them.
#[cfg(all(target_arch = "x86_64", not(miri)))]
mod libmvec {
    use super::assert_within_bound;
    use crate::transcendentals::{Libmvec4, Libmvec8};

    /// The bound of glibc's vector functions, 4 ULPs, as the glibc manual
    /// states it next to its "Errors in Math Functions" tables.
    const LIBMVEC_BOUND: u32 = 4;

    #[test]
    fn libmvec_4_lanes_within_bound() {
        assert_within_bound::<Libmvec4>("libmvec, 4 lanes", LIBMVEC_BOUND);
    }

    #[test]
    fn libmvec_8_lanes_within_bound() {
        assert_within_bound::<Libmvec8>("libmvec, 8 lanes", LIBMVEC_BOUND);
    }
}
