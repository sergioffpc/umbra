//! The optimised kernels against their safe references, side by side
//! (arc42 §8.4, ADR 0007): every bench is generic over [`Kernels`], run once
//! for [`Reference`] (`tests/umbra_math/reference/`, pbrt-v4's scalar code
//! over `libm`) and once for [`Optimised`] (the crate's public interface).
//!
//! Each iteration runs a kernel over a batch of inputs of the magnitudes a
//! renderer meets, generated once with the PCG32 of the tests; the
//! `ItemsCount` counter gives the throughput per call. Run with
//! `cargo bench -p umbra-math`; the build uses the local
//! `target-cpu=native` of `.cargo/config.toml`, so the numbers are for this
//! machine's instruction set.

#[path = "../tests/umbra_math/reference.rs"]
#[expect(dead_code, reason = "the benchmarks time only the hot kernels")]
mod reference;
#[path = "../tests/umbra_math/rng.rs"]
mod rng;

use divan::counter::ItemsCount;
use divan::{Bencher, black_box};
use umbra_math::{
    Interval, Normal3f, Point3f, Point3fi, SquareMatrix, Transform, Vector3f, Vector3fi, look_at,
    perspective, rotate, translate,
};

use crate::rng::Rng;

fn main() {
    divan::main();
}

/// The inputs per iteration.
const BATCH: usize = 1024;

/// The operations with a reference and an optimised implementation.
trait Kernels {
    fn fma(a: f32, b: f32, c: f32) -> f32;
    fn sqrt(x: f32) -> f32;
    fn difference_of_products(a: f32, b: f32, c: f32, d: f32) -> f32;
    fn round_up_pow2(v: u32) -> u32;
    fn interval_add(a: Interval, b: Interval) -> Interval;
    fn interval_sub(a: Interval, b: Interval) -> Interval;
    fn interval_mul(a: Interval, b: Interval) -> Interval;
    fn interval_div(a: Interval, b: Interval) -> Interval;
    fn interval_fma(a: Interval, b: Interval, c: Interval) -> Interval;
    fn interval_sqr(a: Interval) -> Interval;
    fn interval_difference_of_products(
        a: Interval,
        b: Interval,
        c: Interval,
        d: Interval,
    ) -> Interval;
    fn interval_from_value_and_error(v: f32, err: f32) -> Interval;
    fn matrix3_mul(a: &SquareMatrix<3>, b: &SquareMatrix<3>) -> SquareMatrix<3>;
    fn matrix4_mul(a: &SquareMatrix<4>, b: &SquareMatrix<4>) -> SquareMatrix<4>;
    fn matrix4_inverse(m: &SquareMatrix<4>) -> Option<SquareMatrix<4>>;
    fn apply_point(t: &Transform, p: Point3f) -> Point3f;
    fn apply_inverse_point(t: &Transform, p: Point3f) -> Point3f;
    fn apply_vector(t: &Transform, v: Vector3f) -> Vector3f;
    fn apply_normal(t: &Transform, n: Normal3f) -> Normal3f;
    fn apply_point3fi(t: &Transform, p: Point3fi) -> Point3fi;
    fn apply_vector3fi(t: &Transform, v: Vector3fi) -> Vector3fi;
}

/// The safe references of `tests/umbra_math/reference/`: pbrt-v4's scalar
/// code over `libm`.
struct Reference;

/// The crate's public interface: the optimised kernels.
struct Optimised;

impl Kernels for Reference {
    fn fma(a: f32, b: f32, c: f32) -> f32 {
        reference::float::fma(a, b, c)
    }
    fn sqrt(x: f32) -> f32 {
        reference::float::sqrt(x)
    }
    fn difference_of_products(a: f32, b: f32, c: f32, d: f32) -> f32 {
        reference::math::difference_of_products(a, b, c, d)
    }
    fn round_up_pow2(v: u32) -> u32 {
        reference::math::round_up_pow2_u32(v)
    }
    fn interval_add(a: Interval, b: Interval) -> Interval {
        reference::interval::add(a, b)
    }
    fn interval_sub(a: Interval, b: Interval) -> Interval {
        reference::interval::sub(a, b)
    }
    fn interval_mul(a: Interval, b: Interval) -> Interval {
        reference::interval::mul(a, b)
    }
    fn interval_div(a: Interval, b: Interval) -> Interval {
        reference::interval::div(a, b)
    }
    fn interval_fma(a: Interval, b: Interval, c: Interval) -> Interval {
        reference::interval::fma(a, b, c)
    }
    fn interval_sqr(a: Interval) -> Interval {
        reference::interval::sqr(a)
    }
    fn interval_difference_of_products(
        a: Interval,
        b: Interval,
        c: Interval,
        d: Interval,
    ) -> Interval {
        reference::interval::difference_of_products(a, b, c, d)
    }
    fn interval_from_value_and_error(v: f32, err: f32) -> Interval {
        reference::interval::from_value_and_error(v, err)
    }
    fn matrix3_mul(a: &SquareMatrix<3>, b: &SquareMatrix<3>) -> SquareMatrix<3> {
        reference::square_matrix::mul3(a, b)
    }
    fn matrix4_mul(a: &SquareMatrix<4>, b: &SquareMatrix<4>) -> SquareMatrix<4> {
        reference::square_matrix::mul4(a, b)
    }
    fn matrix4_inverse(m: &SquareMatrix<4>) -> Option<SquareMatrix<4>> {
        reference::square_matrix::inverse4(m)
    }
    fn apply_point(t: &Transform, p: Point3f) -> Point3f {
        reference::transform::apply_point(t, p)
    }
    fn apply_inverse_point(t: &Transform, p: Point3f) -> Point3f {
        reference::transform::apply_inverse_point(t, p)
    }
    fn apply_vector(t: &Transform, v: Vector3f) -> Vector3f {
        reference::transform::apply_vector(t, v)
    }
    fn apply_normal(t: &Transform, n: Normal3f) -> Normal3f {
        reference::transform::apply_normal(t, n)
    }
    fn apply_point3fi(t: &Transform, p: Point3fi) -> Point3fi {
        reference::transform::apply_point3fi(t, p)
    }
    fn apply_vector3fi(t: &Transform, v: Vector3fi) -> Vector3fi {
        reference::transform::apply_vector3fi(t, v)
    }
}

impl Kernels for Optimised {
    fn fma(a: f32, b: f32, c: f32) -> f32 {
        umbra_math::fma(a, b, c)
    }
    fn sqrt(x: f32) -> f32 {
        umbra_math::sqrt(x)
    }
    fn difference_of_products(a: f32, b: f32, c: f32, d: f32) -> f32 {
        umbra_math::difference_of_products(a, b, c, d)
    }
    fn round_up_pow2(v: u32) -> u32 {
        umbra_math::round_up_pow2(v)
    }
    fn interval_add(a: Interval, b: Interval) -> Interval {
        a + b
    }
    fn interval_sub(a: Interval, b: Interval) -> Interval {
        a - b
    }
    fn interval_mul(a: Interval, b: Interval) -> Interval {
        a * b
    }
    fn interval_div(a: Interval, b: Interval) -> Interval {
        a / b
    }
    fn interval_fma(a: Interval, b: Interval, c: Interval) -> Interval {
        Interval::fma(a, b, c)
    }
    fn interval_sqr(a: Interval) -> Interval {
        a.sqr()
    }
    fn interval_difference_of_products(
        a: Interval,
        b: Interval,
        c: Interval,
        d: Interval,
    ) -> Interval {
        umbra_math::difference_of_products(a, b, c, d)
    }
    fn interval_from_value_and_error(v: f32, err: f32) -> Interval {
        Interval::from_value_and_error(v, err)
    }
    fn matrix3_mul(a: &SquareMatrix<3>, b: &SquareMatrix<3>) -> SquareMatrix<3> {
        *a * *b
    }
    fn matrix4_mul(a: &SquareMatrix<4>, b: &SquareMatrix<4>) -> SquareMatrix<4> {
        *a * *b
    }
    fn matrix4_inverse(m: &SquareMatrix<4>) -> Option<SquareMatrix<4>> {
        m.inverse()
    }
    fn apply_point(t: &Transform, p: Point3f) -> Point3f {
        t.apply(p)
    }
    fn apply_inverse_point(t: &Transform, p: Point3f) -> Point3f {
        t.apply_inverse(p)
    }
    fn apply_vector(t: &Transform, v: Vector3f) -> Vector3f {
        t.apply(v)
    }
    fn apply_normal(t: &Transform, n: Normal3f) -> Normal3f {
        t.apply(n)
    }
    fn apply_point3fi(t: &Transform, p: Point3fi) -> Point3fi {
        t.apply(p)
    }
    fn apply_vector3fi(t: &Transform, v: Vector3fi) -> Vector3fi {
        t.apply(v)
    }
}

/// A value of renderer magnitude: uniform in `[−1, 1)` scaled by a power of
/// two in `[2⁻¹⁰, 2¹⁰]`, never zero.
fn value(rng: &mut Rng) -> f32 {
    let magnitude = f32::from_bits((rng.uniform_u32() % 21 + 117) << 23);
    let u = (rng.uniform_f32() * 2.0 - 1.0) * magnitude;
    if u == 0.0 { magnitude } else { u }
}

/// An interval of small width around a value, as error bounds are.
fn interval(rng: &mut Rng) -> Interval {
    let v = value(rng);
    Interval::from_value_and_error(v, v.abs() * rng.uniform_f32() * 1e-5)
}

fn values(seed: u64) -> Vec<f32> {
    let mut rng = Rng::new(seed);
    (0..BATCH).map(|_| value(&mut rng)).collect()
}

fn intervals(seed: u64) -> Vec<Interval> {
    let mut rng = Rng::new(seed);
    (0..BATCH).map(|_| interval(&mut rng)).collect()
}

fn points(seed: u64) -> Vec<Point3f> {
    let mut rng = Rng::new(seed);
    (0..BATCH)
        .map(|_| Point3f::new(value(&mut rng), value(&mut rng), value(&mut rng)))
        .collect()
}

/// Matrices close to the identity, as transforms are.
fn matrices<const N: usize>(seed: u64) -> Vec<SquareMatrix<N>> {
    let mut rng = Rng::new(seed);
    (0..BATCH)
        .map(|_| {
            SquareMatrix::new(core::array::from_fn(|i| {
                core::array::from_fn(|j| f32::from(i == j) + rng.uniform_f32() - 0.5)
            }))
        })
        .collect()
}

/// Rigid, projective and look-at transforms, by turns.
fn transforms(seed: u64) -> Vec<Transform> {
    let mut rng = Rng::new(seed);
    (0..BATCH)
        .map(|_| match rng.uniform_u32_below(3) {
            0 => {
                translate(Vector3f::new(
                    value(&mut rng),
                    value(&mut rng),
                    value(&mut rng),
                )) * rotate(
                    rng.uniform_f32() * 360.0,
                    Vector3f::new(value(&mut rng), value(&mut rng), value(&mut rng)),
                )
            }
            1 => perspective(30.0 + rng.uniform_f32() * 60.0, 0.01, 1000.0),
            _ => look_at(
                Point3f::new(value(&mut rng), value(&mut rng), value(&mut rng)),
                Point3f::new(value(&mut rng), value(&mut rng), value(&mut rng)),
                Vector3f::new(0.0, 1.0, 0.0),
            )
            .unwrap_or_default(),
        })
        .collect()
}

/// Runs `f` over every element of a batch per iteration, reporting the
/// throughput per element.
fn batch<T, R>(bencher: Bencher<'_, '_>, data: &[T], f: impl Fn(&T) -> R) {
    bencher
        .counter(ItemsCount::new(data.len()))
        .bench_local(|| {
            for x in data {
                black_box(f(x));
            }
        });
}

#[divan::bench(types = [Reference, Optimised])]
fn fma<K: Kernels>(bencher: Bencher<'_, '_>) {
    let (a, b, c) = (values(1), values(2), values(3));
    let data: Vec<(f32, f32, f32)> = (0..BATCH).map(|i| (a[i], b[i], c[i])).collect();
    batch(bencher, &data, |&(a, b, c)| K::fma(a, b, c));
}

#[divan::bench(types = [Reference, Optimised])]
fn sqrt<K: Kernels>(bencher: Bencher<'_, '_>) {
    let data: Vec<f32> = values(4).iter().map(|v| v.abs()).collect();
    batch(bencher, &data, |&x| K::sqrt(x));
}

#[divan::bench(types = [Reference, Optimised])]
fn difference_of_products<K: Kernels>(bencher: Bencher<'_, '_>) {
    let (a, b, c, d) = (values(7), values(8), values(9), values(10));
    let data: Vec<[f32; 4]> = (0..BATCH).map(|i| [a[i], b[i], c[i], d[i]]).collect();
    batch(bencher, &data, |&[a, b, c, d]| {
        K::difference_of_products(a, b, c, d)
    });
}

#[divan::bench(types = [Reference, Optimised])]
fn round_up_pow2<K: Kernels>(bencher: Bencher<'_, '_>) {
    let mut rng = Rng::new(11);
    let data: Vec<u32> = (0..BATCH).map(|_| rng.uniform_u32() >> 8).collect();
    batch(bencher, &data, |&v| K::round_up_pow2(v));
}

#[divan::bench(types = [Reference, Optimised])]
fn interval_add<K: Kernels>(bencher: Bencher<'_, '_>) {
    let (a, b) = (intervals(12), intervals(13));
    let data: Vec<(Interval, Interval)> = (0..BATCH).map(|i| (a[i], b[i])).collect();
    batch(bencher, &data, |&(a, b)| K::interval_add(a, b));
}

#[divan::bench(types = [Reference, Optimised])]
fn interval_sub<K: Kernels>(bencher: Bencher<'_, '_>) {
    let (a, b) = (intervals(14), intervals(15));
    let data: Vec<(Interval, Interval)> = (0..BATCH).map(|i| (a[i], b[i])).collect();
    batch(bencher, &data, |&(a, b)| K::interval_sub(a, b));
}

#[divan::bench(types = [Reference, Optimised])]
fn interval_mul<K: Kernels>(bencher: Bencher<'_, '_>) {
    let (a, b) = (intervals(16), intervals(17));
    let data: Vec<(Interval, Interval)> = (0..BATCH).map(|i| (a[i], b[i])).collect();
    batch(bencher, &data, |&(a, b)| K::interval_mul(a, b));
}

#[divan::bench(types = [Reference, Optimised])]
fn interval_div<K: Kernels>(bencher: Bencher<'_, '_>) {
    let (a, b) = (intervals(18), intervals(19));
    let data: Vec<(Interval, Interval)> = (0..BATCH).map(|i| (a[i], b[i])).collect();
    batch(bencher, &data, |&(a, b)| K::interval_div(a, b));
}

#[divan::bench(types = [Reference, Optimised])]
fn interval_fma<K: Kernels>(bencher: Bencher<'_, '_>) {
    let (a, b, c) = (intervals(20), intervals(21), intervals(22));
    let data: Vec<(Interval, Interval, Interval)> =
        (0..BATCH).map(|i| (a[i], b[i], c[i])).collect();
    batch(bencher, &data, |&(a, b, c)| K::interval_fma(a, b, c));
}

#[divan::bench(types = [Reference, Optimised])]
fn interval_sqr<K: Kernels>(bencher: Bencher<'_, '_>) {
    batch(bencher, &intervals(23), |&a| K::interval_sqr(a));
}

#[divan::bench(types = [Reference, Optimised])]
fn interval_difference_of_products<K: Kernels>(bencher: Bencher<'_, '_>) {
    let (a, b, c, d) = (intervals(24), intervals(25), intervals(26), intervals(27));
    let data: Vec<[Interval; 4]> = (0..BATCH).map(|i| [a[i], b[i], c[i], d[i]]).collect();
    batch(bencher, &data, |&[a, b, c, d]| {
        K::interval_difference_of_products(a, b, c, d)
    });
}

#[divan::bench(types = [Reference, Optimised])]
fn interval_from_value_and_error<K: Kernels>(bencher: Bencher<'_, '_>) {
    let (v, e) = (values(28), values(29));
    let data: Vec<(f32, f32)> = (0..BATCH).map(|i| (v[i], e[i].abs() * 1e-6)).collect();
    batch(bencher, &data, |&(v, e)| {
        K::interval_from_value_and_error(v, e)
    });
}

#[divan::bench(types = [Reference, Optimised])]
fn matrix3_mul<K: Kernels>(bencher: Bencher<'_, '_>) {
    let (a, b) = (matrices::<3>(30), matrices::<3>(31));
    let data: Vec<(SquareMatrix<3>, SquareMatrix<3>)> = (0..BATCH).map(|i| (a[i], b[i])).collect();
    batch(bencher, &data, |(a, b)| K::matrix3_mul(a, b));
}

#[divan::bench(types = [Reference, Optimised])]
fn matrix4_mul<K: Kernels>(bencher: Bencher<'_, '_>) {
    let (a, b) = (matrices::<4>(32), matrices::<4>(33));
    let data: Vec<(SquareMatrix<4>, SquareMatrix<4>)> = (0..BATCH).map(|i| (a[i], b[i])).collect();
    batch(bencher, &data, |(a, b)| K::matrix4_mul(a, b));
}

#[divan::bench(types = [Reference, Optimised])]
fn matrix4_inverse<K: Kernels>(bencher: Bencher<'_, '_>) {
    batch(bencher, &matrices::<4>(34), |m| K::matrix4_inverse(m));
}

#[divan::bench(types = [Reference, Optimised])]
fn apply_point<K: Kernels>(bencher: Bencher<'_, '_>) {
    let (t, p) = (transforms(35), points(36));
    let data: Vec<(Transform, Point3f)> = (0..BATCH).map(|i| (t[i], p[i])).collect();
    batch(bencher, &data, |(t, p)| K::apply_point(t, *p));
}

#[divan::bench(types = [Reference, Optimised])]
fn apply_inverse_point<K: Kernels>(bencher: Bencher<'_, '_>) {
    let (t, p) = (transforms(37), points(38));
    let data: Vec<(Transform, Point3f)> = (0..BATCH).map(|i| (t[i], p[i])).collect();
    batch(bencher, &data, |(t, p)| K::apply_inverse_point(t, *p));
}

#[divan::bench(types = [Reference, Optimised])]
fn apply_vector<K: Kernels>(bencher: Bencher<'_, '_>) {
    let (t, p) = (transforms(39), points(40));
    let data: Vec<(Transform, Vector3f)> =
        (0..BATCH).map(|i| (t[i], Vector3f::from(p[i]))).collect();
    batch(bencher, &data, |(t, v)| K::apply_vector(t, *v));
}

#[divan::bench(types = [Reference, Optimised])]
fn apply_normal<K: Kernels>(bencher: Bencher<'_, '_>) {
    let (t, p) = (transforms(41), points(42));
    let data: Vec<(Transform, Normal3f)> = (0..BATCH)
        .map(|i| (t[i], Normal3f::from(Vector3f::from(p[i]))))
        .collect();
    batch(bencher, &data, |(t, n)| K::apply_normal(t, *n));
}

#[divan::bench(types = [Reference, Optimised])]
fn apply_point3fi<K: Kernels>(bencher: Bencher<'_, '_>) {
    let (t, i) = (transforms(43), intervals(44));
    let data: Vec<(Transform, Point3fi)> = (0..BATCH)
        .map(|k| {
            (
                t[k],
                Point3fi::new(i[k], i[(k + 1) % BATCH], i[(k + 2) % BATCH]),
            )
        })
        .collect();
    batch(bencher, &data, |(t, p)| K::apply_point3fi(t, *p));
}

#[divan::bench(types = [Reference, Optimised])]
fn apply_vector3fi<K: Kernels>(bencher: Bencher<'_, '_>) {
    let (t, i) = (transforms(45), intervals(46));
    let data: Vec<(Transform, Vector3fi)> = (0..BATCH)
        .map(|k| {
            (
                t[k],
                Vector3fi::new(i[k], i[(k + 1) % BATCH], i[(k + 2) % BATCH]),
            )
        })
        .collect();
    batch(bencher, &data, |(t, v)| K::apply_vector3fi(t, *v));
}
