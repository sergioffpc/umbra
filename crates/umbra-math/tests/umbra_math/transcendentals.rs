//! The transcendentals Umbra uses, from every implementation there is to
//! compare (<https://github.com/sergioffpc/umbra/issues/138>): the
//! `libm` crate, Umbra's own kernels, and, on `linux-gnu`, glibc's scalar
//! `libm.so` and its vector `libmvec` at four and eight lanes.
//!
//! `benches/transcendentals.rs` includes this module by `#[path]` and times
//! the implementations side by side; `accuracy/implementations.rs` checks
//! each of them on the bench's inputs. glibc is linked here and nowhere in
//! the library.
//!
//! Every implementation maps a whole batch, one call at a time: one element
//! for the scalar ones, four or eight for `libmvec`. Each call's result
//! passes through [`black_box`], as in `benches/kernels.rs`, so LLVM cannot
//! vectorise a scalar implementation across calls.

use std::hint::black_box;

use umbra_math::Vec4;

use crate::rng::Rng;

// Explicit path, so the benchmark can include this file by `#[path]` too.
#[cfg(all(target_os = "linux", target_env = "gnu", not(miri)))]
#[path = "transcendentals/glibc.rs"]
mod glibc;

#[cfg(all(target_os = "linux", target_env = "gnu", not(miri)))]
pub(crate) use glibc::{GlibcScalar, Libmvec4, Libmvec8};

/// The inputs per batch: a multiple of eight, the widest call.
pub(crate) const BATCH: usize = 1024;

/// The exponent of the `pow` batch: a gamma curve.
pub(crate) const GAMMA: f32 = 2.2;

/// One implementation of the transcendentals, mapped over a batch. Every
/// output slice has the length of the inputs.
pub(crate) trait Transcendentals {
    /// `sin x` and `cos x`.
    fn sin_cos(x: &[f32], sin: &mut [f32], cos: &mut [f32]);
    /// `atan2(y, x)`.
    fn atan2(y: &[f32], x: &[f32], out: &mut [f32]);
    /// `erf x`.
    fn erf(x: &[f32], out: &mut [f32]);
    /// `eˣ` per lane.
    fn exp4(v: &[Vec4], out: &mut [Vec4]);
    /// `xᵉ` per lane.
    fn pow4(v: &[Vec4], e: f32, out: &mut [Vec4]);
}

/// The `libm` crate, one scalar call per element (per lane for `Vec4`).
#[derive(Debug)]
pub(crate) struct LibmCrate;

/// Umbra's public functions: `sin_cos`, `atan2`, `erf`, `Vec4::exp` and
/// `Vec4::pow`.
#[derive(Debug)]
pub(crate) struct Umbra;

impl Transcendentals for LibmCrate {
    fn sin_cos(x: &[f32], sin: &mut [f32], cos: &mut [f32]) {
        map_pair(x, sin, cos, |[x]| ([libm::sinf(x)], [libm::cosf(x)]));
    }
    fn atan2(y: &[f32], x: &[f32], out: &mut [f32]) {
        map2(y, x, out, |[y], [x]| [libm::atan2f(y, x)]);
    }
    fn erf(x: &[f32], out: &mut [f32]) {
        map(x, out, |[x]| [libm::erff(x)]);
    }
    fn exp4(v: &[Vec4], out: &mut [Vec4]) {
        map_vec4(v, out, |[v]| [Vec4::from(v.to_array().map(libm::expf))]);
    }
    fn pow4(v: &[Vec4], e: f32, out: &mut [Vec4]) {
        map_vec4(v, out, |[v]| {
            [Vec4::from(v.to_array().map(|x| libm::powf(x, e)))]
        });
    }
}

impl Transcendentals for Umbra {
    fn sin_cos(x: &[f32], sin: &mut [f32], cos: &mut [f32]) {
        map_pair(x, sin, cos, |[x]| {
            let (s, c) = umbra_math::sin_cos(x);
            ([s], [c])
        });
    }
    fn atan2(y: &[f32], x: &[f32], out: &mut [f32]) {
        map2(y, x, out, |[y], [x]| [umbra_math::atan2(y, x)]);
    }
    fn erf(x: &[f32], out: &mut [f32]) {
        map(x, out, |[x]| [umbra_math::erf(x)]);
    }
    fn exp4(v: &[Vec4], out: &mut [Vec4]) {
        map_vec4(v, out, |[v]| [v.exp()]);
    }
    fn pow4(v: &[Vec4], e: f32, out: &mut [Vec4]) {
        map_vec4(v, out, |[v]| [v.pow(e)]);
    }
}

/// `x` in calls of `N` elements, with no remainder.
fn calls<const N: usize, T>(x: &[T]) -> &[[T; N]] {
    let (calls, rest) = x.as_chunks();
    assert!(rest.is_empty(), "a batch is a whole number of calls");
    calls
}

/// `x` in calls of `N` elements, with no remainder, for writing.
fn calls_mut<const N: usize, T>(x: &mut [T]) -> &mut [[T; N]] {
    let (calls, rest) = x.as_chunks_mut();
    assert!(rest.is_empty(), "a batch is a whole number of calls");
    calls
}

/// `out = f(x)`, `N` elements per call.
fn map<const N: usize>(x: &[f32], out: &mut [f32], f: impl Fn([f32; N]) -> [f32; N]) {
    assert_eq!(x.len(), out.len(), "one output per input");
    for (out, &x) in calls_mut(out).iter_mut().zip(calls(x)) {
        *out = black_box(f(x));
    }
}

/// `out = f(a, b)`, `N` elements per call.
fn map2<const N: usize>(
    a: &[f32],
    b: &[f32],
    out: &mut [f32],
    f: impl Fn([f32; N], [f32; N]) -> [f32; N],
) {
    assert_eq!(a.len(), out.len(), "one output per input");
    assert_eq!(b.len(), out.len(), "one output per input");
    for ((out, &a), &b) in calls_mut(out).iter_mut().zip(calls(a)).zip(calls(b)) {
        *out = black_box(f(a, b));
    }
}

/// `(p, q) = f(x)`, `N` elements per call.
fn map_pair<const N: usize>(
    x: &[f32],
    p: &mut [f32],
    q: &mut [f32],
    f: impl Fn([f32; N]) -> ([f32; N], [f32; N]),
) {
    assert_eq!(x.len(), p.len(), "one output per input");
    assert_eq!(x.len(), q.len(), "one output per input");
    for ((p, q), &x) in calls_mut(p)
        .iter_mut()
        .zip(calls_mut(q).iter_mut())
        .zip(calls(x))
    {
        (*p, *q) = black_box(f(x));
    }
}

/// `out = f(v)`, `N` `Vec4`s per call.
fn map_vec4<const N: usize>(v: &[Vec4], out: &mut [Vec4], f: impl Fn([Vec4; N]) -> [Vec4; N]) {
    assert_eq!(v.len(), out.len(), "one output per input");
    for (out, &v) in calls_mut(out).iter_mut().zip(calls(v)) {
        *out = black_box(f(v));
    }
}

/// A value of renderer magnitude: uniform in `[−1, 1)` scaled by a power of
/// two in `[2⁻¹⁰, 2¹⁰]`, never zero. The same as in `benches/kernels.rs`.
fn value(rng: &mut Rng) -> f32 {
    let magnitude = f32::from_bits((rng.uniform_u32() % 21 + 117) << 23);
    let u = (rng.uniform_f32() * 2.0 - 1.0) * magnitude;
    if u == 0.0 { magnitude } else { u }
}

/// [`BATCH`] values of renderer magnitude.
fn values(seed: u64) -> Vec<f32> {
    let mut rng = Rng::new(seed);
    (0..BATCH).map(|_| value(&mut rng)).collect()
}

/// [`BATCH`] values uniform in `[low, high)`.
fn uniform(seed: u64, low: f32, high: f32) -> Vec<f32> {
    let mut rng = Rng::new(seed);
    (0..BATCH)
        .map(|_| low + (high - low) * rng.uniform_f32())
        .collect()
}

/// [`BATCH`] spectra of four lanes uniform in `[low, high)`.
fn spectra(seed: u64, low: f32, high: f32) -> Vec<Vec4> {
    let v = uniform(seed, low, high);
    (0..BATCH)
        .map(|i| {
            Vec4::new(
                v[i],
                v[(i + 1) % BATCH],
                v[(i + 2) % BATCH],
                v[(i + 3) % BATCH],
            )
        })
        .collect()
}

/// Angles of a full turn and a bit, as the warps and `spherical_direction`
/// take them.
pub(crate) fn angles() -> Vec<f32> {
    uniform(49, -7.0, 7.0)
}

/// The `y` and `x` of `atan2`: values of renderer magnitude.
pub(crate) fn atan2_args() -> (Vec<f32>, Vec<f32>) {
    (values(50), values(51))
}

/// Over the range where `erf` is not yet ±1.
pub(crate) fn erf_args() -> Vec<f32> {
    uniform(52, -4.0, 4.0)
}

/// Optical depths, as transmittance takes `SampledSpectrum::exp` of them.
pub(crate) fn optical_depths() -> Vec<Vec4> {
    spectra(47, -20.0, 2.0)
}

/// Reflectances, as `SampledSpectrum::pow` takes them with [`GAMMA`]: never
/// negative.
pub(crate) fn reflectances() -> Vec<Vec4> {
    spectra(48, 0.0, 4.0)
}
