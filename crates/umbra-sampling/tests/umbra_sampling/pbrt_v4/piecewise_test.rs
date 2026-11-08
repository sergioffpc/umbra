//! pbrt-v4 `src/pbrt/util/sampling_test.cpp`, the cases of
//! `PiecewiseConstant1D` and `PiecewiseConstant2D` and those that compare a
//! closed-form warp with them, and the two distributions against pbrt-v4's
//! values.

use core::ops::Range;

use umbra_math::{Point2f, lerp, quadratic};
use umbra_sampling::{
    PiecewiseConstant1D, PiecewiseConstant2D, Rng, bilinear_pdf, invert_bilinear_sample,
    invert_linear_sample, linear_pdf, sample_bilinear, sample_discrete, sample_linear,
};

use super::known_values::{
    PIECEWISE_CONSTANT_1D, PIECEWISE_CONSTANT_1D_CDF, PIECEWISE_CONSTANT_2D,
    PIECEWISE_CONSTANT_2D_INTEGRAL,
};
use super::{assert_ulps, p2};
use crate::generators::{radical_inverse, uniform_1d, uniform_2d};

/// The unit square, pbrt-v4's default domain of a `PiecewiseConstant2D`.
const UNIT: [Range<f32>; 2] = [0.0..1.0, 0.0..1.0];

/// A `PiecewiseConstant2D` of `f`, `nu` values per row, over `domain`, in a
/// `Vec`.
fn distribution_2d(f: &[f32], nu: usize, domain: [Range<f32>; 2]) -> PiecewiseConstant2D<Vec<f32>> {
    PiecewiseConstant2D::new_in(f, nu, domain, |n| vec![0.0; n])
}

/// pbrt-v4 `Sample2DFunction`: the largest `|f|` over each of `nu × nv`
/// cells of `domain`, at `n_samples` Hammersley points and three corners.
#[expect(clippy::cast_precision_loss, reason = "as pbrt-v4: int + Float, / int")]
fn sample_2d_function(
    f: impl Fn(f32, f32) -> f32,
    [nu, nv]: [u32; 2],
    n_samples: u64,
    domain: &[Range<f32>; 2],
) -> Vec<f32> {
    let samples: Vec<Point2f> = (0..n_samples)
        .map(|i| Point2f::new(radical_inverse(0, i), radical_inverse(1, i)))
        .chain([
            Point2f::new(0.0, 1.0),
            Point2f::new(1.0, 0.0),
            Point2f::new(1.0, 1.0),
        ])
        .collect();
    let cell = |u: u32, v: u32| {
        let max = samples
            .iter()
            .map(|s| {
                let x = lerp((u as f32 + s.x) / nu as f32, domain[0].start, domain[0].end);
                let y = lerp((v as f32 + s.y) / nv as f32, domain[1].start, domain[1].end);
                f64::from(f(x, y).abs())
            })
            .fold(0.0, f64::max);
        #[expect(
            clippy::cast_possible_truncation,
            reason = "as pbrt-v4: double to Float"
        )]
        let max = max as f32;
        max
    };
    (0..nv)
        .flat_map(|v| (0..nu).map(move |u| cell(u, v)))
        .collect()
}

/// A `PiecewiseConstant1D` of `f` over `domain`, in a `Vec`.
fn distribution_1d(f: &[f32], domain: Range<f32>) -> PiecewiseConstant1D<Vec<f32>> {
    PiecewiseConstant1D::new_in(f, domain, |n| vec![0.0; n])
}

/// pbrt-v4 `Sample1DFunction`: the largest `|f|` of `n_samples + 1` points
/// across each of `n_steps` steps over `[min, max]`.
#[expect(clippy::cast_precision_loss, reason = "as pbrt-v4: Float(int)")]
fn sample_1d_function(
    f: impl Fn(f32) -> f32,
    n_steps: u32,
    n_samples: u32,
    domain: &Range<f32>,
) -> Vec<f32> {
    let step = |i: u32| {
        let max = (0..=n_samples)
            .map(|j| {
                let delta = j as f32 / n_samples as f32;
                let v = lerp(
                    (i as f32 + delta) / n_steps as f32,
                    domain.start,
                    domain.end,
                );
                f64::from(f(v).abs())
            })
            .fold(0.0, f64::max);
        #[expect(
            clippy::cast_possible_truncation,
            reason = "as pbrt-v4: double to Float"
        )]
        let max = max as f32;
        max
    };
    (0..n_steps).map(step).collect()
}

/// `TEST(PiecewiseConstant1D, Continuous)`.
#[test]
fn piecewise_constant_1d_continuous() {
    let dist = distribution_1d(&[1.0, 1.0, 2.0, 4.0, 8.0], 0.0..1.0);
    assert_eq!(5, dist.size());

    let s = dist.sample(0.0);
    assert_eq!((s.x, s.offset), (0.0, 0));
    assert!((s.pdf - 5.0 / 16.0).abs() < 1e-6, "{}", s.pdf);

    // Right at the boundary between the 4 and the 8 segments.
    assert!((dist.sample(0.5).x - 0.8).abs() < 1e-6);

    // Middle of the 8 segment.
    let s = dist.sample(0.75);
    assert!((s.x - 0.9).abs() < 1e-6, "{}", s.x);
    assert!((s.pdf - 5.0 * 8.0 / 16.0).abs() < 1e-6, "{}", s.pdf);
    assert_eq!(4, s.offset);

    assert_eq!(0.0, dist.sample(0.0).x);
    assert!((1.0 - dist.sample(1.0).x).abs() < 1e-6);
}

/// `TEST(PiecewiseConstant1D, Range)`.
#[test]
fn piecewise_constant_1d_range() {
    let values = sample_1d_function(|x| 1.0 + x, 65536, 4, &(-1.0..3.0));
    let dist = distribution_1d(&values, -1.0..3.0);
    // p(x) = (1 + x) / 8, so xi = x²/16 + x/8 + 1/16.
    for u in uniform_1d(100, 0) {
        let s = dist.sample(u);
        #[expect(
            clippy::cast_possible_truncation,
            reason = "as pbrt-v4: double to Float"
        )]
        let c = (1.0_f64 / 16.0 - f64::from(u)) as f32;
        let (t0, t1) = quadratic(1.0 / 16.0, 1.0 / 8.0, c).unwrap();
        let xa = if (-1.0..=3.0).contains(&t0) { t0 } else { t1 };
        let pa = (1.0 + xa) / 8.0;
        assert!((s.x - xa).abs() / xa < 2e-3, "{} vs {xa}", s.x);
        assert!((s.pdf - pa).abs() / pa < 2e-3, "{} vs {pa}", s.pdf);
    }
}

/// Asserts that `dist` inverts each `x` of `cases` to exactly its `u`.
#[track_caller]
fn assert_inverts(dist: &PiecewiseConstant1D<Vec<f32>>, cases: &[(f32, f32)]) {
    for &(x, u) in cases {
        assert_eq!(Some(u), dist.invert(x), "Invert({x})");
    }
}

/// `TEST(PiecewiseConstant1D, InverseUniform)`.
#[test]
fn piecewise_constant_1d_inverse_uniform() {
    let values = [1.0, 1.0, 1.0];
    let dist = distribution_1d(&values, 0.0..1.0);
    assert_inverts(&dist, &[(0.0, 0.0), (0.5, 0.5), (0.75, 0.75)]);
    let dist2 = distribution_1d(&values, -1.0..3.0);
    let cases = [
        (-1.0, 0.0),
        (0.0, 0.25),
        (1.0, 0.5),
        (2.0, 0.75),
        (3.0, 1.0),
    ];
    assert_inverts(&dist2, &cases);
}

/// `TEST(PiecewiseConstant1D, InverseGeneral)`.
#[test]
fn piecewise_constant_1d_inverse_general() {
    let values = [0.0, 0.25, 0.5, 0.25];
    let cases = [
        (0.0, 0.0),
        (1.0, 1.0),
        (0.5, 0.25),
        (0.625, 0.5),
        (0.75, 0.75),
    ];
    for domain in [0.0..1.0, -1.0..3.0] {
        let (min, max) = (domain.start, domain.end);
        let dist = distribution_1d(&values, domain);
        assert_inverts(&dist, &cases.map(|(x, u)| (lerp(x, min, max), u)));
        // EXPECT_FLOAT_EQ in pbrt-v4.
        let inv = dist.invert(lerp(0.825, min, max)).unwrap();
        assert!((inv - 0.825).abs() < 1e-6, "Invert(0.825) = {inv}");
    }
}

/// `TEST(PiecewiseConstant1D, InverseRandoms)`. pbrt-v4 builds the second
/// distribution and then samples the first one again; this samples the
/// second.
#[test]
fn piecewise_constant_1d_inverse_randoms() {
    let values = [0.0, 1.25, 0.5, 0.25, 3.7];
    for domain in [0.0..1.0, -1.0..3.0] {
        let dist = distribution_1d(&values, domain);
        for u in uniform_1d(100, 0) {
            let inv = dist.invert(dist.sample(u).x).unwrap();
            let err = (inv - u).abs().min((inv - u).abs() / u);
            assert!(err < 1e-4, "u {u} vs inv {inv}");
        }
    }
}

/// `TEST(PiecewiseConstant1D, Integral)`.
#[test]
fn piecewise_constant_1d_integral() {
    let values = [1.0, 0.5, 0.0, 0.25];
    assert_eq!(0.4375, distribution_1d(&values, 0.0..1.0).integral());
    assert_eq!(1.75, distribution_1d(&values, 0.0..4.0).integral());
    assert_eq!(3.5, distribution_1d(&values, -3.0..5.0).integral());
}

/// `TEST(SampleDiscrete, VsPiecewiseConstant1D)`.
#[test]
fn sample_discrete_vs_piecewise_constant_1d() {
    let mut rng = Rng::default();
    let values: Vec<f32> = (0..15)
        .map(|_| (rng.uniform::<f32>() - 0.2).max(0.0))
        .collect();
    let dist = distribution_1d(&values, 0.0..1.0);
    #[expect(clippy::cast_precision_loss, reason = "15 values")]
    let n = values.len() as f32;

    for _ in 0..100 {
        let u = rng.uniform();
        let sd = sample_discrete(&values, u).unwrap();
        let pc = dist.sample(u);
        #[expect(
            clippy::cast_possible_truncation,
            clippy::cast_sign_loss,
            reason = "as pbrt-v4: int(x)"
        )]
        let pc_index = (pc.x * n) as usize;
        assert_eq!(sd.index, pc_index, "{u}");
        assert!((sd.pmf - pc.pdf / n).abs() < 1e-3, "{u}");
    }
}

/// `TEST(Sampling, Linear)`: bucket counts of a million stratified samples
/// are linear, the warp matches a tabulated distribution of the same
/// function, and its inverse undoes it.
#[test]
fn sampling_linear() {
    for (f0, f1) in [
        (0.0, 1.0),
        (1.0, 2.0),
        (5.0, 50.0),
        (100.0, 0.0),
        (75.0, 50.0),
    ] {
        assert_linear_buckets(f0, f1);
    }
    assert_linear_matches_piecewise();
    assert_linear_inverts();
}

/// The first part of `TEST(Sampling, Linear)`.
#[track_caller]
fn assert_linear_buckets(f0: f32, f1: f32) {
    const N_BUCKETS: usize = 32;
    let mut buckets = [0_u32; N_BUCKETS];
    let n_samples = 1_000_000;
    for i in 0..n_samples {
        #[expect(
            clippy::cast_possible_truncation,
            reason = "as pbrt-v4: double to Float"
        )]
        let u = ((f64::from(i) + 0.5) / f64::from(n_samples)) as f32;
        let t = sample_linear(u, f0, f1);
        #[expect(
            clippy::cast_possible_truncation,
            clippy::cast_sign_loss,
            reason = "as pbrt-v4: int(x)"
        )]
        let bucket = ((t * 32.0) as usize).min(N_BUCKETS - 1);
        buckets[bucket] += 1;
    }
    #[expect(clippy::cast_precision_loss, reason = "a million samples")]
    let (first, last) = (buckets[0] as f32, buckets[N_BUCKETS - 1] as f32);
    for (i, &count) in (0_u8..).zip(&buckets) {
        #[expect(
            clippy::cast_possible_truncation,
            clippy::cast_sign_loss,
            reason = "as pbrt-v4: int(Lerp(...))"
        )]
        let expected = f64::from(lerp(f32::from(i) / 31.0, first, last) as u32);
        let count = f64::from(count);
        assert!(
            (0.99 * expected..=1.01 * expected).contains(&count),
            "({f0}, {f1}) bucket {i}: {count} vs {expected}"
        );
    }
}

/// The second part of `TEST(Sampling, Linear)`.
fn assert_linear_matches_piecewise() {
    let values = sample_1d_function(|v| 1.0 + 3.0 * v, 1024, 64 * 1024, &(0.0..1.0));
    let distrib = distribution_1d(&values, 0.0..1.0);
    for u in uniform_1d(100, 0) {
        let cx = sample_linear(u, 1.0, 4.0);
        let cp = linear_pdf(cx, 1.0, 4.0);
        let d = distrib.sample(u);
        assert!((cx - d.x).abs() < 3e-3, "closed form {cx}, distrib {}", d.x);
        assert!(
            (cp - d.pdf).abs() < 3e-3,
            "closed form PDF {cp}, distrib PDF {}",
            d.pdf
        );
    }
}

/// The third part of `TEST(Sampling, Linear)`.
fn assert_linear_inverts() {
    let mut rng = Rng::default();
    let check_err = |a: f32, b: f32| {
        let err = if a.abs().min(b.abs()) < 1e-2 {
            (a - b).abs()
        } else {
            (2.0 * (a - b) / (a + b)).abs()
        };
        err > 1e-2
    };
    for u in uniform_1d(100, 0) {
        let mut low = rng.uniform::<f32>() * 10.0;
        let mut high = rng.uniform::<f32>() * 10.0;
        if low < high {
            core::mem::swap(&mut low, &mut high);
        }
        let x = sample_linear(u, low, high);
        let inverse = invert_linear_sample(x, low, high);
        assert!(
            !check_err(u, inverse),
            "u {u} -> x {x} -> {inverse} over {low} - {high}"
        );
    }
}

/// `TEST(PiecewiseConstant2D, InverseUniform)`.
#[test]
fn piecewise_constant_2d_inverse_uniform() {
    let values = [1.0; 6];
    let dist = distribution_2d(&values, 3, UNIT);
    for p in [(0.0, 0.0), (1.0, 1.0), (0.5, 0.5), (0.25, 0.75)].map(|(x, y)| Point2f::new(x, y)) {
        assert_eq!(Some(p), dist.invert(p));
    }

    let domain = [-1.0..3.0, -0.5..1.5];
    let dist2 = distribution_2d(&values, 3, domain.clone());
    for p in [(0.0, 0.0), (1.0, 1.0), (0.5, 0.5), (0.25, 0.75)].map(|(x, y)| Point2f::new(x, y)) {
        let q = Point2f::new(lerp(p.x, -1.0, 3.0), lerp(p.y, -0.5, 1.5));
        assert_eq!(Some(p), dist2.invert(q), "{domain:?}");
    }
}

/// `TEST(PiecewiseConstant2D, InverseRandoms)`.
#[test]
fn piecewise_constant_2d_inverse_randoms() {
    let (nx, ny) = (4, 5);
    let mut rng = Rng::default();
    let values: Vec<f32> = (0..nx * ny).map(|_| rng.uniform()).collect();
    let rel = |inv: f32, u: f32| (inv - u).abs().min((inv - u).abs() / u);

    let cases = [(UNIT, 0), ([-1.0..3.0, -0.5..1.5], 235_351)];
    for (domain, sequence_index) in cases {
        let dist = distribution_2d(&values, nx, domain);
        for u in uniform_2d(100, sequence_index) {
            let v = dist.sample(u).p;
            let inv = dist.invert(v).unwrap();
            assert!(
                rel(inv.x, u.x) < 1e-4,
                "u.x {} vs inv {} at {}",
                u.x,
                inv.x,
                v.x
            );
            assert!(
                rel(inv.y, u.y) < 1e-4,
                "u.y {} vs inv {} at {}",
                u.y,
                inv.y,
                v.y
            );
        }
    }
}

/// `TEST(PiecewiseConstant2D, FromFuncLInfinity)`. pbrt-v4 compares the
/// marginal and conditional PDFs of the two distributions; this compares
/// their product, the PDF, at the centre of each cell.
#[test]
fn piecewise_constant_2d_from_func_l_infinity() {
    let values = sample_2d_function(|x, y| x * x * y, [4, 2], 1, &UNIT);
    let sampled = distribution_2d(&values, 4, UNIT);

    let exact: Vec<f32> = [0.5_f32, 1.0]
        .iter()
        .flat_map(|&y| [0.25_f32, 0.5, 0.75, 1.0].map(|x| x * x * y))
        .collect();
    let exact = distribution_2d(&exact, 4, UNIT);

    for (iu, iv) in (0..4_u8).flat_map(|u| (0..2_u8).map(move |v| (u, v))) {
        let p = Point2f::new(f32::from(2 * iu + 1) / 8.0, f32::from(2 * iv + 1) / 4.0);
        let (a, b) = (sampled.pdf(p), exact.pdf(p));
        assert!(
            (a - b).abs() / f32::midpoint(a, b) < 1e-5,
            "{a} - {b} at {p:?}"
        );
    }
}

/// `TEST(PiecewiseConstant2D, Integral)`.
#[test]
fn piecewise_constant_2d_integral() {
    let values = [1.0, 2.0, 0.0, 7.0, 3.0, 1.0, 2.0, 0.0];
    assert_eq!(2.0, distribution_2d(&values, 4, UNIT).integral());
    assert_eq!(2.0, distribution_2d(&values, 2, UNIT).integral());
    assert_eq!(
        16.0,
        distribution_2d(&values, 4, [0.0..4.0, 0.0..2.0]).integral()
    );
    assert_eq!(
        8.0,
        distribution_2d(&values, 4, [-1.0..0.0, 0.0..4.0]).integral()
    );
}

/// `TEST(Sampling, Bilinear)`.
#[test]
fn sampling_bilinear() {
    for v in [
        [0.0, 0.5, 1.3, 4.7],
        [1.0, 1.0, 1.0, 1.0],
        [11.0, 0.25, 1.0, 20.0],
    ] {
        assert_bilinear_matches_piecewise(v);
    }
}

/// One quad of `TEST(Sampling, Bilinear)`: the closed-form warp matches a
/// tabulated distribution of the same function, and its inverse undoes it.
#[track_caller]
fn assert_bilinear_matches_piecewise(v: [f32; 4]) {
    let bilerp = |x: f32, y: f32| {
        (1.0 - x) * (1.0 - y) * v[0] + x * (1.0 - y) * v[1] + (1.0 - x) * y * v[2] + x * y * v[3]
    };
    let values = sample_2d_function(bilerp, [1024, 1024], 16, &UNIT);
    let distrib = distribution_2d(&values, 1024, UNIT);
    let close = |a: f32, b: f32| (a - b).abs() < 3e-3;
    for u in uniform_2d(100, 0) {
        let pb = sample_bilinear(u, v);
        let bp = bilinear_pdf(pb, v);
        let d = distrib.sample(u);
        assert!(
            close(pb.x, d.p.x),
            "x: closed form {}, distrib {}",
            pb.x,
            d.p.x
        );
        assert!(
            close(pb.y, d.p.y),
            "y: closed form {}, distrib {}",
            pb.y,
            d.p.y
        );
        assert!(
            close(bp, d.pdf),
            "closed form PDF {bp}, distrib PDF {}",
            d.pdf
        );

        let up = invert_bilinear_sample(pb, v);
        assert!(
            close(up.x, u.x) && close(up.y, u.y),
            "invert: u {u:?}, p {pb:?}, up {up:?}"
        );
    }
}

/// `PiecewiseConstant2D`'s integral, `Sample`, `PDF` and `Invert` give
/// pbrt-v4's values.
#[test]
fn known_values_2d() {
    let mut rng = Rng::default();
    let values: Vec<f32> = (0..20).map(|_| rng.uniform()).collect();
    let dist = distribution_2d(&values, 4, [-1.0..3.0, -0.5..1.5]);
    assert_eq!(dist.integral().to_bits(), PIECEWISE_CONSTANT_2D_INTEGRAL);

    for &(u, p, pdf, offset, pdf_at_p, inverse) in PIECEWISE_CONSTANT_2D {
        let s = dist.sample(p2(u));
        assert_ulps(
            [s.p.x, s.p.y, s.pdf],
            [p[0], p[1], pdf],
            0,
            "PiecewiseConstant2D::Sample",
        );
        assert_eq!(s.offset, offset);
        assert_ulps([dist.pdf(p2(p))], [pdf_at_p], 0, "PiecewiseConstant2D::PDF");
        let inv = dist.invert(p2(p)).unwrap();
        assert_ulps([inv.x, inv.y], inverse, 0, "PiecewiseConstant2D::Invert");
    }
}

/// `PiecewiseConstant1D`'s CDF, integral, `Sample` and `Invert` give
/// pbrt-v4's values.
#[test]
fn known_values_1d() {
    let dist = distribution_1d(&[0.0, 1.25, 0.5, 0.25, 3.7], -1.0..3.0);
    let (cdf, integral) = PIECEWISE_CONSTANT_1D_CDF.split_at(dist.cdf().len());
    assert_eq!(
        dist.cdf().iter().map(|c| c.to_bits()).collect::<Vec<_>>(),
        cdf
    );
    assert_eq!([dist.integral().to_bits()], integral);

    for &(u, x, pdf, offset, inverse) in PIECEWISE_CONSTANT_1D {
        let s = dist.sample(f32::from_bits(u));
        assert_ulps([s.x, s.pdf], [x, pdf], 0, "PiecewiseConstant1D::Sample");
        assert_eq!(s.offset, offset);
        let inv = dist.invert(f32::from_bits(x)).unwrap();
        assert_ulps([inv], [inverse], 0, "PiecewiseConstant1D::Invert");
    }
}
