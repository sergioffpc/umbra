//! The transcendentals Umbra uses, from every implementation there is,
//! side by side (<https://github.com/sergioffpc/umbra/issues/138>): Umbra's
//! own kernels (the crate's public interface), glibc's scalar `libm.so`
//! through `umbra_math::cmath`, and on `linux-gnu` glibc's vector `libmvec`
//! at 4 lanes (SSE) and 8 (AVX2). Elsewhere, and under Miri, the `libmvec`
//! lines are absent.
//!
//! The implementations live in `tests/umbra_math/transcendentals.rs`,
//! included here by `#[path]`; `accuracy/implementations.rs` there checks
//! them on the same inputs. Each iteration maps a batch of inputs of the
//! magnitudes a renderer meets, generated once with the PCG32 of the tests;
//! the `ItemsCount` counter gives the time per element, or per `Vec4` for
//! `exp4` and `pow4`, so an 8-lane call counts as two.
//!
//! Run with `cargo bench -p umbra-math --bench transcendentals`; the crate
//! README says how to compare machines at `target-cpu=x86-64-v3`.

#[path = "../tests/umbra_math/rng.rs"]
#[expect(dead_code, reason = "the inputs draw uniform values only")]
mod rng;
#[path = "../tests/umbra_math/transcendentals.rs"]
mod transcendentals;

use divan::Bencher;
use divan::counter::ItemsCount;
use umbra_math::Vec4;

use crate::transcendentals::{
    BATCH, GAMMA, GlibcScalar, Transcendentals, Umbra, angles, atan2_args, erf_args,
    optical_depths, reflectances,
};
#[cfg(all(target_os = "linux", target_env = "gnu", not(miri)))]
use crate::transcendentals::{Libmvec4, Libmvec8};

fn main() {
    divan::main();
}

/// A bench generic over [`Transcendentals`], run for every implementation
/// this target has.
macro_rules! bench {
    ($(#[$attr:meta])* fn $name:ident<K>($bencher:ident) $body:block) => {
        $(#[$attr])*
        #[cfg_attr(
            all(target_os = "linux", target_env = "gnu", not(miri)),
            divan::bench(types = [Umbra, GlibcScalar, Libmvec4, Libmvec8])
        )]
        #[cfg_attr(
            not(all(target_os = "linux", target_env = "gnu", not(miri))),
            divan::bench(types = [Umbra, GlibcScalar])
        )]
        fn $name<K: Transcendentals>($bencher: Bencher<'_, '_>) $body
    };
}

bench! {
    /// `sin x` and `cos x` of angles of a full turn and a bit, as the warps
    /// and `spherical_direction` take them.
    fn sin_cos<K>(bencher) {
        let x = angles();
        let (mut sin, mut cos) = (vec![0.0; BATCH], vec![0.0; BATCH]);
        bencher
            .counter(ItemsCount::new(BATCH))
            .bench_local(|| K::sin_cos(&x, &mut sin, &mut cos));
    }
}

bench! {
    /// `atan2(y, x)` of values of renderer magnitude.
    fn atan2<K>(bencher) {
        let (y, x) = atan2_args();
        let mut out = vec![0.0; BATCH];
        bencher
            .counter(ItemsCount::new(BATCH))
            .bench_local(|| K::atan2(&y, &x, &mut out));
    }
}

bench! {
    /// `erf x` over the range where it is not yet ±1.
    fn erf<K>(bencher) {
        let x = erf_args();
        let mut out = vec![0.0; BATCH];
        bencher
            .counter(ItemsCount::new(BATCH))
            .bench_local(|| K::erf(&x, &mut out));
    }
}

bench! {
    /// `SampledSpectrum::exp` of optical depths, as transmittance takes it:
    /// the time per `Vec4`.
    fn exp4<K>(bencher) {
        let v = optical_depths();
        let mut out = vec![Vec4::ZERO; BATCH];
        bencher
            .counter(ItemsCount::new(BATCH))
            .bench_local(|| K::exp4(&v, &mut out));
    }
}

bench! {
    /// `SampledSpectrum::pow` of reflectances with a gamma exponent: the
    /// time per `Vec4`.
    fn pow4<K>(bencher) {
        let v = reflectances();
        let mut out = vec![Vec4::ZERO; BATCH];
        bencher
            .counter(ItemsCount::new(BATCH))
            .bench_local(|| K::pow4(&v, GAMMA, &mut out));
    }
}
