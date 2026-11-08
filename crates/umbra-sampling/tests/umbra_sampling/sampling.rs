//! The sampling tests of arc42 §8.11 for every warp and distribution, with
//! fixed seeds: a χ² goodness-of-fit of the samples against the PDF, the
//! PDF agreeing with the sampling routine (`pdf(T(u)) · |det ∂T/∂u| = 1`,
//! and the PDF a routine returns equal to its PDF function), and the PDF
//! integrating to 1.
//!
//! The χ² tests run on a grid of the warp's natural coordinates, (θ, φ) for
//! directions, with the PDF converted to them; a warp without a PDF function
//! in pbrt-v4 is tested against its analytic density, which for the disk and
//! the triangle by area is a constant, normalised by definition.

use core::f64::consts::PI;

use umbra_math::{Point2f, Point3f, Vector3f};
use umbra_sampling::{
    PiecewiseConstant1D, PiecewiseConstant2D, Rng, bilinear_pdf, cosine_hemisphere_pdf,
    exponential_pdf, henyey_greenstein, invert_uniform_triangle_sample, linear_pdf,
    sample_bilinear, sample_cosine_hemisphere, sample_discrete, sample_exponential,
    sample_henyey_greenstein, sample_linear, sample_spherical_triangle, sample_uniform_cone,
    sample_uniform_disk_concentric, sample_uniform_disk_polar, sample_uniform_hemisphere,
    sample_uniform_sphere, sample_uniform_triangle, sample_visible_wavelengths, uniform_cone_pdf,
    uniform_hemisphere_pdf, uniform_sphere_pdf, visible_wavelengths_pdf,
};

use crate::chi2::{Grid, assert_chi2, assert_chi2_counts, integrate, integrate_2d};
use crate::generators::{uniform_1d, uniform_2d};

/// Samples per χ² test.
const SAMPLE_COUNT: usize = 1 << 20;
/// The step of the central differences that estimate a warp's Jacobian.
const H: f32 = 1e-3;
/// How far `pdf(T(u)) · |det ∂T/∂u|` may be from 1: the finite differences
/// are accurate to about `1e-4` at step [`H`] in `f32`.
const AGREEMENT_TOLERANCE: f64 = 2e-3;

/// `u` drawn from sequence `sequence_index`, squeezed into `[0.05, 0.95]²`
/// so that the finite differences stay away from the edges of the domain.
fn interior_points(sequence_index: u64) -> impl Iterator<Item = Point2f> {
    uniform_2d(1000, sequence_index).map(|u| Point2f::new(0.05 + 0.9 * u.x, 0.05 + 0.9 * u.y))
}

/// Whether `u` is within a finite-difference step of a seam of the
/// concentric disk map, the diagonals of the square, where its Jacobian
/// jumps.
fn near_concentric_seam(u: Point2f) -> bool {
    ((2.0 * u.x - 1.0).abs() - (2.0 * u.y - 1.0).abs()).abs() < 8.0 * H
}

/// `v` in `f64`.
fn f64x3(v: Vector3f) -> [f64; 3] {
    [v.x, v.y, v.z].map(f64::from)
}

/// The length of the cross product of `a` and `b`.
fn cross_length(a: [f64; 3], b: [f64; 3]) -> f64 {
    let c = cross3(a, b);
    dot3(c, c).sqrt()
}

/// The partial derivatives of `warp` at `u`, by central differences.
fn partials<const N: usize>(warp: impl Fn(Point2f) -> [f64; N], u: Point2f) -> [[f64; N]; 2] {
    let h = f64::from(H);
    let d = |a: [f64; N], b: [f64; N]| core::array::from_fn(|i| (a[i] - b[i]) / (2.0 * h));
    [
        d(
            warp(Point2f::new(u.x + H, u.y)),
            warp(Point2f::new(u.x - H, u.y)),
        ),
        d(
            warp(Point2f::new(u.x, u.y + H)),
            warp(Point2f::new(u.x, u.y - H)),
        ),
    ]
}

/// Asserts that `pdf(T(u)) · |det ∂T/∂u|` is 1 within the tolerance.
#[track_caller]
fn assert_agrees(what: &str, u: Point2f, pdf: f64, jacobian: f64) {
    assert!(
        (pdf * jacobian - 1.0).abs() < AGREEMENT_TOLERANCE,
        "{what}: at {u:?}, PDF {pdf} but 1 / |det J| = {}",
        1.0 / jacobian
    );
}

/// Asserts that `integral` is 1 within `tolerance`.
#[track_caller]
fn assert_normalised(what: &str, integral: f64, tolerance: f64) {
    assert!(
        (integral - 1.0).abs() < tolerance,
        "{what}: the PDF integrates to {integral}"
    );
}

/// The polar angle and azimuth of the direction `w`, φ in `[0, 2π)`.
fn theta_phi(w: Vector3f) -> [f64; 2] {
    let [x, y, z] = f64x3(w);
    let phi = libm::atan2(y, x);
    let theta = libm::acos(z.clamp(-1.0, 1.0));
    [theta, if phi < 0.0 { phi + 2.0 * PI } else { phi }]
}

/// The direction with polar angle `theta` and azimuth `phi`, in `f32`.
#[expect(clippy::cast_possible_truncation, reason = "the PDFs take f32")]
fn direction(theta: f64, phi: f64) -> Vector3f {
    let s = libm::sin(theta);
    Vector3f::new(
        (s * libm::cos(phi)) as f32,
        (s * libm::sin(phi)) as f32,
        libm::cos(theta) as f32,
    )
}

/// A warp to directions within `theta_max` of `+z`, and its PDF with respect
/// to solid angle.
struct DirectionWarp<W, P> {
    name: &'static str,
    sample: W,
    pdf: P,
    theta_max: f64,
}

impl<W: Fn(Point2f) -> Vector3f, P: Fn(Vector3f) -> f32> DirectionWarp<W, P> {
    /// The PDF with respect to `(θ, φ)`.
    fn pdf_theta_phi(&self, theta: f64, phi: f64) -> f64 {
        f64::from((self.pdf)(direction(theta, phi))) * libm::sin(theta)
    }

    /// χ² over a 10 × 20 grid of `[0, θmax] × [0, 2π)`.
    #[track_caller]
    fn assert_chi2(&self, sequence_index: u64) {
        let samples = uniform_2d(SAMPLE_COUNT, sequence_index).map(|u| theta_phi((self.sample)(u)));
        let grid = Grid {
            x: 0.0..self.theta_max,
            y: 0.0..2.0 * PI,
            res: [10, 20],
        };
        assert_chi2(self.name, samples, |t, p| self.pdf_theta_phi(t, p), &grid);
    }

    /// `pdf · |∂w/∂u₀ × ∂w/∂u₁| = 1`, skipping the points `skip` rejects.
    #[track_caller]
    fn assert_agreement(&self, sequence_index: u64, skip: impl Fn(Point2f) -> bool) {
        for u in interior_points(sequence_index).filter(|&u| !skip(u)) {
            let [du, dv] = partials(|u| f64x3((self.sample)(u)), u);
            let pdf = f64::from((self.pdf)((self.sample)(u)));
            assert_agrees(self.name, u, pdf, cross_length(du, dv));
        }
    }

    /// The PDF integrates to 1 over the directions within `θmax`.
    #[track_caller]
    fn assert_normalised(&self) {
        let integral = integrate_2d(
            |t, p| self.pdf_theta_phi(t, p),
            &(0.0..self.theta_max),
            &(0.0..2.0 * PI),
            64,
        );
        assert_normalised(self.name, integral, 1e-5);
    }

    /// All three sampling tests.
    #[track_caller]
    fn assert_all(&self, sequence_index: u64, skip: impl Fn(Point2f) -> bool) {
        self.assert_chi2(sequence_index);
        self.assert_agreement(sequence_index, skip);
        self.assert_normalised();
    }
}

/// `sample_uniform_sphere` and `uniform_sphere_pdf`.
#[test]
fn uniform_sphere() {
    DirectionWarp {
        name: "uniform sphere",
        sample: sample_uniform_sphere,
        pdf: |_| uniform_sphere_pdf(),
        theta_max: PI,
    }
    .assert_all(1, |_| false);
}

/// `sample_uniform_hemisphere` and `uniform_hemisphere_pdf`.
#[test]
fn uniform_hemisphere() {
    DirectionWarp {
        name: "uniform hemisphere",
        sample: sample_uniform_hemisphere,
        pdf: |_| uniform_hemisphere_pdf(),
        theta_max: PI / 2.0,
    }
    .assert_all(2, |_| false);
}

/// `sample_cosine_hemisphere` and `cosine_hemisphere_pdf`.
#[test]
fn cosine_hemisphere() {
    DirectionWarp {
        name: "cosine hemisphere",
        sample: sample_cosine_hemisphere,
        pdf: |w: Vector3f| cosine_hemisphere_pdf(w.z),
        theta_max: PI / 2.0,
    }
    .assert_all(3, near_concentric_seam);
}

/// `sample_uniform_cone` and `uniform_cone_pdf`, for a wide and a narrow
/// cone.
#[test]
fn uniform_cone() {
    for (cos_theta_max, sequence_index) in [(0.3_f32, 4), (0.95, 5)] {
        DirectionWarp {
            name: "uniform cone",
            sample: |u| sample_uniform_cone(u, cos_theta_max),
            pdf: |_| uniform_cone_pdf(cos_theta_max),
            theta_max: libm::acos(f64::from(cos_theta_max)),
        }
        .assert_all(sequence_index, |_| false);
    }
}

/// `sample_henyey_greenstein` and `henyey_greenstein`, isotropic, backward
/// and forward, about `+z` so that `(θ, φ)` are those of the phase function;
/// and the PDF the sample returns equal to the phase function.
#[test]
fn henyey_greenstein_phase() {
    let wo = Vector3f::new(0.0, 0.0, 1.0);
    for (g, sequence_index) in [(0.0_f32, 19), (-0.6, 20), (0.85, 21)] {
        DirectionWarp {
            name: "Henyey–Greenstein",
            sample: |u| sample_henyey_greenstein(wo, g, u).wi,
            pdf: |w: Vector3f| henyey_greenstein(w.z, g),
            theta_max: PI,
        }
        .assert_all(sequence_index, |_| false);
        for u in interior_points(sequence_index) {
            let s = sample_henyey_greenstein(wo, g, u);
            let pdf = henyey_greenstein(s.wi.z, g);
            assert!(
                (s.pdf - pdf).abs() <= 1e-5 * pdf,
                "g {g}: PDF {} vs {pdf}",
                s.pdf
            );
        }
    }
}

/// χ² of a warp to the unit disk against the uniform density `1 / π`, in
/// polar coordinates `(r, φ)` where it is `r / π`; and the warp agreeing
/// with that density.
#[track_caller]
fn assert_uniform_disk(name: &str, sample: impl Fn(Point2f) -> Point2f, sequence_index: u64) {
    let polar = |p: Point2f| {
        let (x, y) = (f64::from(p.x), f64::from(p.y));
        let phi = libm::atan2(y, x);
        [
            libm::hypot(x, y),
            if phi < 0.0 { phi + 2.0 * PI } else { phi },
        ]
    };
    let samples = uniform_2d(SAMPLE_COUNT, sequence_index).map(|u| polar(sample(u)));
    let grid = Grid {
        x: 0.0..1.0,
        y: 0.0..2.0 * PI,
        res: [10, 20],
    };
    assert_chi2(name, samples, |r, _| r / PI, &grid);

    for u in interior_points(sequence_index).filter(|&u| !near_concentric_seam(u)) {
        let [du, dv] = partials(|u| f64x3(in_plane(sample(u))), u);
        assert_agrees(name, u, 1.0 / PI, cross_length(du, dv));
    }
}

/// The point `p` of the plane as a vector of the `z = 0` plane, so a planar
/// warp's Jacobian is the length of a cross product, as a direction warp's.
fn in_plane(p: Point2f) -> Vector3f {
    Vector3f::new(p.x, p.y, 0.0)
}

/// `sample_uniform_disk_concentric`.
#[test]
fn uniform_disk_concentric() {
    assert_uniform_disk("concentric disk", sample_uniform_disk_concentric, 6);
}

/// `sample_uniform_disk_polar`.
#[test]
fn uniform_disk_polar() {
    assert_uniform_disk("polar disk", sample_uniform_disk_polar, 7);
}

/// `sample_uniform_triangle`: χ² against the uniform density over the
/// barycentrics `(b₀, b₁)`, `2`, in the coordinates `s = b₀ + b₁`,
/// `t = b₁ / s` of the unit square, where it is `2s`; and the warp agreeing
/// with that density away from the seam `u₀ = u₁` of Heitz's map.
#[test]
fn uniform_triangle() {
    let samples = uniform_2d(SAMPLE_COUNT, 8).map(|u| {
        let [b0, b1, _] = sample_uniform_triangle(u).map(f64::from);
        let s = b0 + b1;
        [s, if s > 0.0 { b1 / s } else { 0.0 }]
    });
    let grid = Grid {
        x: 0.0..1.0,
        y: 0.0..1.0,
        res: [16, 16],
    };
    assert_chi2("uniform triangle", samples, |s, _| 2.0 * s, &grid);

    for u in interior_points(8).filter(|u| (u.x - u.y).abs() > 2.0 * H) {
        let barycentrics = |u| {
            let [b0, b1, _] = sample_uniform_triangle(u);
            f64x3(in_plane(Point2f::new(b0, b1)))
        };
        let [du, dv] = partials(barycentrics, u);
        assert_agrees("uniform triangle", u, 2.0, cross_length(du, dv));
    }
}

/// The vertices and reference point of pbrt-v4's `TEST(Sampling,
/// SphericalTriangle)`.
const TRIANGLE: [[f32; 3]; 3] = [[4.0, 1.0, 1.0], [-10.0, 3.0, 3.0], [-2.0, -8.0, 10.0]];
const REFERENCE_POINT: [f32; 3] = [0.5, -0.4, 0.7];

/// The dot product in `f64`.
fn dot3(a: [f64; 3], b: [f64; 3]) -> f64 {
    a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
}

/// The cross product in `f64`.
fn cross3(a: [f64; 3], b: [f64; 3]) -> [f64; 3] {
    [
        a[1] * b[2] - a[2] * b[1],
        a[2] * b[0] - a[0] * b[2],
        a[0] * b[1] - a[1] * b[0],
    ]
}

/// `d` normalised, in `f64`.
fn unit(d: [f64; 3]) -> [f64; 3] {
    let l = dot3(d, d).sqrt();
    d.map(|c| c / l)
}

/// pbrt-v4's triangle and reference point, and what the tests of
/// `sample_spherical_triangle` need of their geometry, in `f64`.
struct SphericalTriangleCase {
    v: [Point3f; 3],
    p: Point3f,
    area: f64,
    solid_angle: f64,
    normal: [f64; 3],
}

impl SphericalTriangleCase {
    fn new() -> Self {
        let v = TRIANGLE.map(|[x, y, z]| Point3f::new(x, y, z));
        let p = Point3f::new(REFERENCE_POINT[0], REFERENCE_POINT[1], REFERENCE_POINT[2]);
        let n = cross3(f64x3(v[1] - v[0]), f64x3(v[2] - v[0]));
        let [a, b, c] = v.map(|vi| unit(f64x3(vi - p)));
        let solid_angle = (2.0
            * libm::atan2(
                dot3(a, cross3(b, c)),
                1.0 + dot3(a, b) + dot3(a, c) + dot3(b, c),
            ))
        .abs();
        Self {
            v,
            p,
            area: dot3(n, n).sqrt() / 2.0,
            solid_angle,
            normal: unit(n),
        }
    }

    /// The density at `(s, t)` of the unit square that
    /// `sample_uniform_triangle` maps onto the triangle, for solid-angle
    /// samples of PDF `pdf`: `pdf · |cos θ| A / r²`.
    fn density(&self, pdf: f64, s: f64, t: f64) -> f64 {
        #[expect(clippy::cast_possible_truncation, reason = "the warp takes f32")]
        let u = Point2f::new(s as f32, t as f32);
        let [b0, b1, b2] = sample_uniform_triangle(u).map(f64::from);
        let [v0, v1, v2] = self.v.map(|vi| f64x3(vi.into()));
        let p = f64x3(self.p.into());
        let d: [f64; 3] = core::array::from_fn(|i| b0 * v0[i] + b1 * v1[i] + b2 * v2[i] - p[i]);
        let r2 = dot3(d, d);
        pdf * dot3(self.normal, d).abs() / r2.sqrt() * self.area / r2
    }

    /// χ² on the unit square against the density of the uniform solid-angle
    /// PDF `1 / Ω`.
    fn assert_chi2(&self) {
        let samples = uniform_2d(SAMPLE_COUNT, 9).map(|u| {
            let b = sample_spherical_triangle(self.v, self.p, u).barycentrics;
            let s = invert_uniform_triangle_sample(b);
            [f64::from(s.x), f64::from(s.y)]
        });
        let grid = Grid {
            x: 0.0..1.0,
            y: 0.0..1.0,
            res: [16, 16],
        };
        let pdf = 1.0 / self.solid_angle;
        assert_chi2(
            "spherical triangle",
            samples,
            |s, t| self.density(pdf, s, t),
            &grid,
        );
    }

    /// The PDF the routine returns integrates to 1 over the triangle.
    fn assert_normalised(&self) {
        let sample = sample_spherical_triangle(self.v, self.p, Point2f::new(0.5, 0.5));
        let pdf = f64::from(sample.pdf);
        let integral = integrate_2d(|s, t| self.density(pdf, s, t), &(0.0..1.0), &(0.0..1.0), 64);
        assert_normalised("spherical triangle", integral, 1e-5);
    }

    /// The PDF the routine returns agrees with its warp to directions.
    fn assert_agreement(&self) {
        let direction = |u: Point2f| {
            let [b0, b1, b2] = sample_spherical_triangle(self.v, self.p, u).barycentrics;
            let x = b0 * self.v[0] + b1 * self.v[1] + b2 * self.v[2];
            f64x3((x - self.p).normalize())
        };
        for u in interior_points(9) {
            let [du, dv] = partials(direction, u);
            let pdf = f64::from(sample_spherical_triangle(self.v, self.p, u).pdf);
            assert_agrees("spherical triangle", u, pdf, cross_length(du, dv));
        }
    }
}

/// `sample_spherical_triangle`, on pbrt-v4's triangle. Its samples are
/// compared on the unit square that `sample_uniform_triangle` maps onto the
/// triangle, where the density of uniform solid-angle samples is
/// `|cos θ| A / (Ω r²)` (area `A`, solid angle `Ω`, distance `r`, angle `θ`
/// to the triangle's normal).
#[test]
fn spherical_triangle() {
    let case = SphericalTriangleCase::new();
    case.assert_chi2();
    case.assert_normalised();
    case.assert_agreement();
}

/// A one-dimensional warp over `domain` and its PDF: χ² over `bins` cells
/// (samples beyond the domain in one more, expected to get what the PDF
/// leaves outside it), `pdf(T(u)) · T′(u) = 1`, and the PDF integrating to 1
/// over the domain within `tolerance`.
#[track_caller]
fn assert_1d(
    name: &str,
    sample: impl Fn(f32) -> f32,
    pdf: impl Fn(f32) -> f32,
    (domain, bins): (core::ops::Range<f64>, usize),
    (sequence_index, tolerance): (u64, f64),
) {
    #[expect(clippy::cast_possible_truncation, reason = "the PDFs take f32")]
    let pdf64 = |x: f64, _| f64::from(pdf(x as f32));
    let samples = uniform_1d(SAMPLE_COUNT, sequence_index).map(|u| [f64::from(sample(u)), 0.5]);
    assert_chi2(name, samples, pdf64, &Grid::line(domain.clone(), bins));

    for u in interior_points(sequence_index).map(|u| u.x) {
        let derivative = f64::from(sample(u + H) - sample(u - H)) / (2.0 * f64::from(H));
        assert_agrees(
            name,
            Point2f::new(u, 0.0),
            f64::from(pdf(sample(u))),
            derivative,
        );
    }

    let integral = integrate(|x| pdf64(x, 0.0), domain.start, domain.end, 1024);
    assert_normalised(name, integral, tolerance);
}

/// `sample_linear` and `linear_pdf`, increasing and decreasing.
#[test]
fn linear() {
    for (a, b, sequence_index) in [(0.5, 2.0, 10), (3.0, 0.25, 11)] {
        assert_1d(
            "linear",
            |u| sample_linear(u, a, b),
            |x| linear_pdf(x, a, b),
            (0.0..1.0, 32),
            (sequence_index, 1e-6),
        );
    }
}

/// `sample_exponential` and `exponential_pdf`, whose tail beyond the grid
/// is one more χ² cell; its integral is checked far enough out that the
/// tail is below the tolerance.
#[test]
fn exponential() {
    assert_1d(
        "exponential",
        |u| sample_exponential(u, 1.7),
        |x| exponential_pdf(x, 1.7),
        (0.0..3.0, 30),
        (12, 1e-2),
    );
    #[expect(clippy::cast_possible_truncation, reason = "the PDF takes f32")]
    let integral = integrate(
        |x| f64::from(exponential_pdf(x as f32, 1.7)),
        0.0,
        30.0,
        4096,
    );
    assert_normalised("exponential", integral, 1e-5);
}

/// `sample_visible_wavelengths` and `visible_wavelengths_pdf`.
#[test]
fn visible_wavelengths() {
    assert_1d(
        "visible wavelengths",
        sample_visible_wavelengths,
        visible_wavelengths_pdf,
        (360.0..830.0, 47),
        (13, 1e-4),
    );
}

/// `sample_bilinear` and `bilinear_pdf`.
#[test]
fn bilinear() {
    let w = [0.1, 0.5, 1.3, 4.7];
    #[expect(clippy::cast_possible_truncation, reason = "the PDF takes f32")]
    let pdf = |x: f64, y: f64| f64::from(bilinear_pdf(Point2f::new(x as f32, y as f32), w));
    let samples = uniform_2d(SAMPLE_COUNT, 14).map(|u| {
        let p = sample_bilinear(u, w);
        [f64::from(p.x), f64::from(p.y)]
    });
    let grid = Grid {
        x: 0.0..1.0,
        y: 0.0..1.0,
        res: [16, 16],
    };
    assert_chi2("bilinear", samples, pdf, &grid);

    for u in interior_points(14) {
        let [du, dv] = partials(|u| f64x3(in_plane(sample_bilinear(u, w))), u);
        let p = sample_bilinear(u, w);
        assert_agrees(
            "bilinear",
            u,
            f64::from(bilinear_pdf(p, w)),
            cross_length(du, dv),
        );
    }

    let integral = integrate_2d(pdf, &(0.0..1.0), &(0.0..1.0), 16);
    assert_normalised("bilinear", integral, 1e-6);
}

/// `sample_discrete`: χ² of the chosen indices against the weights, the
/// PMF it returns, and the PMFs summing to 1.
#[test]
fn discrete() {
    let weights = [0.1, 0.4, 0.0, 0.25, 0.25, 1.5, 0.01];
    let sum: f32 = weights.iter().sum();
    let mut observed = [0.0; 7];
    for u in uniform_1d(SAMPLE_COUNT, 15) {
        let s = sample_discrete(&weights, u).unwrap();
        assert_eq!(
            s.pmf,
            weights[s.index] / sum,
            "the PMF of index {}",
            s.index
        );
        observed[s.index] += 1.0;
    }
    #[expect(clippy::cast_precision_loss, reason = "2^20 samples")]
    let expected = weights.map(|w| f64::from(w / sum) * SAMPLE_COUNT as f64);
    assert_chi2_counts("discrete", &observed, &expected);

    let pmfs: f64 = weights.iter().map(|&w| f64::from(w / sum)).sum();
    assert_normalised("discrete", pmfs, 1e-6);
}

/// Values in `[0, 1)` from `RNG(sequence_index)`, a quarter of them 0.
fn random_values(n: usize, sequence_index: u64) -> Vec<f32> {
    let mut rng = Rng::new(sequence_index);
    (0..n)
        .map(|_| {
            let v: f32 = rng.uniform();
            if v < 0.25 { 0.0 } else { v }
        })
        .collect()
}

/// `PiecewiseConstant1D`: χ² on a grid aligned with its pieces, the PDF it
/// returns, its warp agreeing with that PDF inside each piece, and the PDF
/// integrating to 1.
#[test]
fn piecewise_constant_1d() {
    let values = random_values(16, 16);
    let dist = PiecewiseConstant1D::new_in(&values, -1.0..3.0, |n| vec![0.0; n]);
    let pdf_at = |x: f64| {
        #[expect(
            clippy::cast_possible_truncation,
            clippy::cast_sign_loss,
            reason = "x in [-1, 3)"
        )]
        let piece = (((x + 1.0) / 4.0 * 16.0) as usize).min(15);
        f64::from(dist.func()[piece] / dist.integral())
    };

    let samples = uniform_1d(SAMPLE_COUNT, 16).map(|u| [f64::from(dist.sample(u).x), 0.5]);
    assert_chi2(
        "piecewise constant 1D",
        samples,
        |x, _| pdf_at(x),
        &Grid::line(-1.0..3.0, 16),
    );

    assert_piecewise_1d_agreement(&dist, pdf_at);

    assert_normalised(
        "piecewise constant 1D",
        integrate(pdf_at, -1.0, 3.0, 64),
        1e-6,
    );
}

/// The PDF that `dist.sample` returns is `pdf_at` its sample, and agrees
/// with its warp inside each piece.
#[track_caller]
fn assert_piecewise_1d_agreement(
    dist: &PiecewiseConstant1D<Vec<f32>>,
    pdf_at: impl Fn(f64) -> f64,
) {
    for u in interior_points(16).map(|u| u.x) {
        let [s, lo, hi] = [u, u - H, u + H].map(|u| dist.sample(u));
        let pdf = f64::from(s.pdf);
        assert_eq!(pdf, pdf_at(f64::from(s.x)), "the PDF at {}", s.x);
        if lo.offset == s.offset && hi.offset == s.offset {
            let derivative = f64::from(hi.x - lo.x) / (2.0 * f64::from(H));
            assert_agrees(
                "piecewise constant 1D",
                Point2f::new(u, 0.0),
                pdf,
                derivative,
            );
        }
    }
}

/// `PiecewiseConstant2D`: as [`piecewise_constant_1d`], with its PDF
/// function.
#[test]
fn piecewise_constant_2d() {
    let values = random_values(8 * 6, 17);
    let dist = PiecewiseConstant2D::new_in(&values, 8, [-1.0..3.0, -0.5..1.5], |n| vec![0.0; n]);
    #[expect(clippy::cast_possible_truncation, reason = "the PDF takes f32")]
    let pdf = |x: f64, y: f64| f64::from(dist.pdf(Point2f::new(x as f32, y as f32)));

    let samples = uniform_2d(SAMPLE_COUNT, 17).map(|u| {
        let p = dist.sample(u).p;
        [f64::from(p.x), f64::from(p.y)]
    });
    let grid = Grid {
        x: -1.0..3.0,
        y: -0.5..1.5,
        res: [8, 6],
    };
    assert_chi2("piecewise constant 2D", samples, pdf, &grid);

    for u in interior_points(17) {
        let s = dist.sample(u);
        // The marginal times the conditional PDF rounds differently from the
        // cell's value over the integral, in pbrt-v4 too.
        let pdf = dist.pdf(s.p);
        assert!(
            (s.pdf - pdf).abs() <= 1e-6 * pdf,
            "the PDF at {:?}: {} vs {pdf}",
            s.p,
            s.pdf
        );
        let neighbours = [(H, 0.0), (-H, 0.0), (0.0, H), (0.0, -H)]
            .map(|(dx, dy)| dist.sample(Point2f::new(u.x + dx, u.y + dy)).offset);
        if neighbours.iter().all(|&o| o == s.offset) {
            let [du, dv] = partials(|u| f64x3(in_plane(dist.sample(u).p)), u);
            assert_agrees(
                "piecewise constant 2D",
                u,
                f64::from(s.pdf),
                cross_length(du, dv),
            );
        }
    }

    let integral = integrate_2d(pdf, &(-1.0..3.0), &(-0.5..1.5), 48);
    assert_normalised("piecewise constant 2D", integral, 1e-6);
}

/// The χ² test has the power to reject: cosine-weighted directions do not
/// pass as uniform ones.
#[test]
fn chi2_rejects_a_wrong_pdf() {
    let wrong = DirectionWarp {
        name: "cosine-weighted directions as uniform ones",
        sample: sample_cosine_hemisphere,
        pdf: |_| uniform_hemisphere_pdf(),
        theta_max: PI / 2.0,
    };
    let result = std::panic::catch_unwind(|| wrong.assert_chi2(18));
    assert!(result.is_err(), "the χ² test accepted the wrong PDF");
}
