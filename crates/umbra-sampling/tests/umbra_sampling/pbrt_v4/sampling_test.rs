//! pbrt-v4 `src/pbrt/util/sampling_test.cpp`, the cases of the routines
//! `umbra-sampling` ports, and `sampling.h` against pbrt-v4's values. The
//! low-discrepancy cases are ported with the sequences.

use umbra_math::{
    Point2f, Point3f, Vector3f, abs_dot, cross, distance_squared, lerp, spherical_triangle_area,
};
use umbra_sampling::{
    Rng, balance_heuristic, bilinear_pdf, cosine_hemisphere_pdf, exponential_pdf,
    henyey_greenstein, invert_bilinear_sample, invert_cosine_hemisphere_sample,
    invert_exponential_sample, invert_linear_sample, invert_spherical_triangle_sample,
    invert_uniform_cone_sample, invert_uniform_disk_concentric_sample,
    invert_uniform_disk_polar_sample, invert_uniform_hemisphere_sample,
    invert_uniform_sphere_sample, invert_uniform_triangle_sample, linear_pdf, power_heuristic,
    sample_bilinear, sample_cosine_hemisphere, sample_discrete, sample_exponential,
    sample_henyey_greenstein, sample_linear, sample_spherical_triangle, sample_uniform_cone,
    sample_uniform_disk_concentric, sample_uniform_disk_polar, sample_uniform_hemisphere,
    sample_uniform_sphere, sample_uniform_triangle, sample_visible_wavelengths, uniform_cone_pdf,
    uniform_hemisphere_pdf, uniform_sphere_pdf, visible_wavelengths_pdf,
};

use super::known_values::{
    HENYEY_GREENSTEIN, HEURISTICS, SAMPLE_BILINEAR, SAMPLE_COSINE_HEMISPHERE, SAMPLE_DISCRETE,
    SAMPLE_EXPONENTIAL, SAMPLE_HENYEY_GREENSTEIN, SAMPLE_LINEAR, SAMPLE_SPHERICAL_TRIANGLE,
    SAMPLE_UNIFORM_CONE, SAMPLE_UNIFORM_DISK_CONCENTRIC, SAMPLE_UNIFORM_DISK_POLAR,
    SAMPLE_UNIFORM_HEMISPHERE, SAMPLE_UNIFORM_SPHERE, SAMPLE_UNIFORM_TRIANGLE,
    SAMPLE_VISIBLE_WAVELENGTHS, WARP_PDFS,
};
use super::{assert_ulps, p2};
use crate::generators::{radical_inverse, uniform_2d};

/// `TEST(SampleDiscrete, Basics)`.
#[test]
fn sample_discrete_basics() {
    let s = sample_discrete(&[5.0], 0.251).unwrap();
    assert_eq!((s.index, s.pmf), (0, 1.0));

    let s = sample_discrete(&[0.5, 0.5], 0.0).unwrap();
    assert_eq!((s.index, s.pmf), (0, 0.5));

    let s = sample_discrete(&[0.5, 0.5], 0.499).unwrap();
    assert_eq!((s.index, s.pmf), (0, 0.5));

    let s = sample_discrete(&[0.5, 0.5], 0.5).unwrap();
    assert_eq!((s.index, s.pmf, s.u_remapped), (1, 0.5, 0.0));

    assert_eq!(sample_discrete(&[], 0.5), None);
}

/// `u` within 1e-3 of `up`, component-wise.
#[track_caller]
fn assert_round_trip(u: Point2f, up: Point2f) {
    assert!(
        (u.x - up.x).abs() < 1e-3 && (u.y - up.y).abs() < 1e-3,
        "u {u:?} -> {up:?}"
    );
}

/// `[x, y]` of `p`.
fn xy(p: Point2f) -> [f32; 2] {
    [p.x, p.y]
}

/// `[x, y, z]` of `v`.
fn xyz(v: Vector3f) -> [f32; 3] {
    [v.x, v.y, v.z]
}

/// The vector whose components have the bits `bits`.
fn v3(bits: [u32; 3]) -> Vector3f {
    Vector3f::new(
        f32::from_bits(bits[0]),
        f32::from_bits(bits[1]),
        f32::from_bits(bits[2]),
    )
}

/// `TEST(Sampling, InvertUniformHemisphere)`.
#[test]
fn sampling_invert_uniform_hemisphere() {
    for u in uniform_2d(1000, 0) {
        assert_round_trip(
            u,
            invert_uniform_hemisphere_sample(sample_uniform_hemisphere(u)),
        );
    }
}

/// `TEST(Sampling, InvertCosineHemisphere)`.
#[test]
fn sampling_invert_cosine_hemisphere() {
    for u in uniform_2d(1000, 0) {
        assert_round_trip(
            u,
            invert_cosine_hemisphere_sample(sample_cosine_hemisphere(u)),
        );
    }
}

/// `TEST(Sampling, InvertUniformSphere)`.
#[test]
fn sampling_invert_uniform_sphere() {
    for u in uniform_2d(1000, 0) {
        assert_round_trip(u, invert_uniform_sphere_sample(sample_uniform_sphere(u)));
    }
}

/// `TEST(Sampling, InvertUniformTriangle)`.
#[test]
fn sampling_invert_uniform_triangle() {
    for u in uniform_2d(1000, 0) {
        assert_round_trip(
            u,
            invert_uniform_triangle_sample(sample_uniform_triangle(u)),
        );
    }
}

/// `TEST(Sampling, InvertUniformCone)`.
#[test]
fn sampling_invert_uniform_cone() {
    let mut rng = Rng::default();
    for u in uniform_2d(1000, 0) {
        let cos_theta_max = rng.uniform();
        let w = sample_uniform_cone(u, cos_theta_max);
        assert_round_trip(u, invert_uniform_cone_sample(w, cos_theta_max));
    }
}

/// `TEST(Sampling, InvertUniformDiskPolar)`.
#[test]
fn sampling_invert_uniform_disk_polar() {
    for u in uniform_2d(1000, 0) {
        assert_round_trip(
            u,
            invert_uniform_disk_polar_sample(sample_uniform_disk_polar(u)),
        );
    }
}

/// `TEST(Sampling, InvertUniformDiskConcentric)`.
#[test]
fn sampling_invert_uniform_disk_concentric() {
    for u in uniform_2d(1000, 0) {
        let p = sample_uniform_disk_concentric(u);
        assert_round_trip(u, invert_uniform_disk_concentric_sample(p));
    }
}

/// The disk warps and their inverses give pbrt-v4's values.
///
/// Here and below, a budget above 0 ulps is the largest difference measured
/// on these inputs, all of it from the transcendentals (`sinf`, `cosf`,
/// `atan2f`, `expf`, `logf`, `coshf`), which glibc and the `libm` crate do
/// not always round alike (either one can be the
/// correctly rounded result), amplified where the routine subtracts nearly
/// equal values: an inverse subtracting the angle from π/2 or π, Arvo's
/// construction of the spherical triangle sample.
#[test]
fn known_values_disk() {
    for &(u, p, inverse) in SAMPLE_UNIFORM_DISK_CONCENTRIC {
        let ours = sample_uniform_disk_concentric(p2(u));
        assert_ulps(xy(ours), p, 0, "SampleUniformDiskConcentric");
        assert_ulps(
            xy(invert_uniform_disk_concentric_sample(p2(p))),
            inverse,
            3,
            "InvertUniformDiskConcentricSample",
        );
    }
    for &(u, p, inverse) in SAMPLE_UNIFORM_DISK_POLAR {
        assert_ulps(
            xy(sample_uniform_disk_polar(p2(u))),
            p,
            1,
            "SampleUniformDiskPolar",
        );
        assert_ulps(
            xy(invert_uniform_disk_polar_sample(p2(p))),
            inverse,
            2,
            "InvertUniformDiskPolarSample",
        );
    }
}

/// Asserts that a warp to directions and its inverse give the values of
/// `table`, within `[sample_ulps, invert_ulps]`.
#[track_caller]
fn assert_direction_warp(
    table: &[([u32; 2], [u32; 3], [u32; 2])],
    sample: fn(Point2f) -> Vector3f,
    invert: fn(Vector3f) -> Point2f,
    name: &str,
    [sample_ulps, invert_ulps]: [u32; 2],
) {
    for &(u, w, inverse) in table {
        assert_ulps(xyz(sample(p2(u))), w, sample_ulps, &format!("Sample{name}"));
        let what = format!("Invert{name}Sample");
        assert_ulps(xy(invert(v3(w))), inverse, invert_ulps, &what);
    }
}

/// The sphere and hemisphere warps and their inverses give pbrt-v4's values.
#[test]
fn known_values_sphere() {
    assert_direction_warp(
        SAMPLE_UNIFORM_SPHERE,
        sample_uniform_sphere,
        invert_uniform_sphere_sample,
        "UniformSphere",
        [0, 2],
    );
    assert_direction_warp(
        SAMPLE_UNIFORM_HEMISPHERE,
        sample_uniform_hemisphere,
        invert_uniform_hemisphere_sample,
        "UniformHemisphere",
        [1, 2],
    );
    assert_direction_warp(
        SAMPLE_COSINE_HEMISPHERE,
        sample_cosine_hemisphere,
        invert_cosine_hemisphere_sample,
        "CosineHemisphere",
        [0, 3],
    );
}

/// The cone warp and its inverse, and the direction PDFs, give pbrt-v4's
/// values.
#[test]
fn known_values_cone() {
    for &(u, w, inverse) in SAMPLE_UNIFORM_CONE {
        let ours = sample_uniform_cone(p2(u), 0.3);
        assert_ulps(xyz(ours), w, 0, "SampleUniformCone");
        let ours = invert_uniform_cone_sample(v3(w), 0.3);
        assert_ulps(xy(ours), inverse, 2, "InvertUniformConeSample");
    }
    let pdfs = [
        uniform_sphere_pdf(),
        uniform_hemisphere_pdf(),
        cosine_hemisphere_pdf(0.6),
        uniform_cone_pdf(0.3),
    ];
    assert_eq!(pdfs.map(f32::to_bits), WARP_PDFS);
}

/// The point of the triangle `v` with barycentrics `b`.
fn barycentric_point(v: [Point3f; 3], b: [f32; 3]) -> Point3f {
    b[0] * v[0] + b[1] * v[1] + b[2] * v[2]
}

/// `TEST(Sampling, SphericalTriangle)`: integrating over the triangle by
/// solid angle and by area gives the same result.
#[test]
fn sampling_spherical_triangle() {
    let count = 1024 * 1024;
    let v = [
        Point3f::new(4.0, 1.0, 1.0),
        Point3f::new(-10.0, 3.0, 3.0),
        Point3f::new(-2.0, -8.0, 10.0),
    ];
    let area = 0.5 * cross(v[1] - v[0], v[2] - v[0]).length();
    let n = cross(v[1] - v[0], v[2] - v[0]).normalize();
    let p = Point3f::new(0.5, -0.4, 0.7);
    let f = |p: Point3f| p.x * p.y * p.z;

    let (mut sph_sum, mut area_sum) = (0.0_f32, 0.0_f32);
    for i in 0..count {
        let u = Point2f::new(radical_inverse(0, i), radical_inverse(1, i));

        let s = sample_spherical_triangle(v, p, u);
        sph_sum += f(barycentric_point(v, s.barycentrics)) / s.pdf;

        let b_area = sample_uniform_triangle(u);
        let pdf = 1.0 / area;
        let p_tri = barycentric_point(v, [b_area[0], b_area[1], 1.0 - b_area[0] - b_area[1]]);
        area_sum +=
            f(p_tri) * abs_dot(n, (p_tri - p).normalize()) / (pdf * distance_squared(p, p_tri));
    }
    #[expect(clippy::cast_precision_loss, reason = "2^20 is exact")]
    let count = count as f32;
    let (sph_int, area_int) = (sph_sum / count, area_sum / count);
    assert!((area_int - sph_int).abs() < 1e-3, "{area_int} vs {sph_int}");
}

/// `TEST(Sampling, SphericalTriangleInverse)`.
#[test]
fn sampling_spherical_triangle_inverse() {
    let mut rng = Rng::default();
    let rp = |rng: &mut Rng, low: f32, high: f32| {
        let [x, y, z] = [(); 3].map(|()| lerp(rng.uniform(), low, high));
        Point3f::new(x, y, z)
    };
    let err = |a: f32, reference: f32| {
        if reference < 0.05 {
            (a - reference).abs()
        } else {
            ((a - reference) / reference).abs()
        }
    };

    for _ in 0..100 {
        let v = [(); 3].map(|()| rp(&mut rng, -10.0, 10.0));
        let p = rp(&mut rng, -1.0, 1.0);
        let [a, b, c] = v.map(|vi| (vi - p).normalize());
        if spherical_triangle_area(a, b, c) < 5e-3 {
            continue;
        }
        for _ in 0..100 {
            let ux = rng.uniform();
            let u = Point2f::new(ux, rng.uniform());
            let s = sample_spherical_triangle(v, p, u);
            let p_tri = barycentric_point(v, s.barycentrics);
            let ui = invert_spherical_triangle_sample(v, p, (p_tri - p).normalize());
            // The tolerance has to be fairly high, unfortunately (pbrt-v4).
            assert!((ui.x - u.x).abs() < 0.025, "{u:?} vs inverse {ui:?}");
            assert!(err(ui.y, u.y) < 0.005, "{u:?} vs inverse {ui:?}");
        }
    }
}

/// The triangle warps and their inverses give pbrt-v4's values.
#[test]
fn known_values_triangle() {
    for &(u, b, inverse) in SAMPLE_UNIFORM_TRIANGLE {
        assert_ulps(
            sample_uniform_triangle(p2(u)),
            b,
            0,
            "SampleUniformTriangle",
        );
        let b = b.map(f32::from_bits);
        assert_ulps(
            xy(invert_uniform_triangle_sample(b)),
            inverse,
            0,
            "InvertUniformTriangleSample",
        );
    }

    let v = [
        Point3f::new(4.0, 1.0, 1.0),
        Point3f::new(-10.0, 3.0, 3.0),
        Point3f::new(-2.0, -8.0, 10.0),
    ];
    let p = Point3f::new(0.5, -0.4, 0.7);
    for &(u, b, pdf, inverse) in SAMPLE_SPHERICAL_TRIANGLE {
        let s = sample_spherical_triangle(v, p, p2(u));
        // 14 ulps on one input, the others exact: glibc's `sinf` is 2 ulps
        // off the correctly rounded value at its sampled area (the `libm`
        // crate's is exact there), and Arvo's construction amplifies that.
        assert_ulps(s.barycentrics, b, 14, "SampleSphericalTriangle");
        assert_ulps([s.pdf], [pdf], 0, "SampleSphericalTriangle pdf");
        // At `u = (0, 0)` the sample is the vertex `b`, along which the
        // inverse is undefined: pbrt-v4 computes NaN, failing its `DCHECK`s.
        if inverse.map(f32::from_bits).iter().any(|x| x.is_nan()) {
            continue;
        }
        let w = (barycentric_point(v, b.map(f32::from_bits)) - p).normalize();
        let ui = invert_spherical_triangle_sample(v, p, w);
        assert_ulps(xy(ui), inverse, 3, "InvertSphericalTriangleSample");
    }
}

/// The linear and bilinear warps, their PDFs and inverses give pbrt-v4's
/// values.
#[test]
fn known_values_linear() {
    for &(u, x, pdf, inverse) in SAMPLE_LINEAR {
        let ours = sample_linear(f32::from_bits(u), 0.5, 2.0);
        assert_ulps([ours], [x], 0, "SampleLinear");
        let x = f32::from_bits(x);
        assert_ulps([linear_pdf(x, 0.5, 2.0)], [pdf], 0, "LinearPDF");
        let ours = invert_linear_sample(x, 0.5, 2.0);
        assert_ulps([ours], [inverse], 0, "InvertLinearSample");
    }

    let w = [0.1, 0.5, 1.3, 4.7];
    for &(u, q, pdf, inverse) in SAMPLE_BILINEAR {
        assert_ulps(xy(sample_bilinear(p2(u), w)), q, 0, "SampleBilinear");
        assert_ulps([bilinear_pdf(p2(q), w)], [pdf], 0, "BilinearPDF");
        let ours = invert_bilinear_sample(p2(q), w);
        assert_ulps(xy(ours), inverse, 0, "InvertBilinearSample");
    }
}

/// The visible-wavelength and exponential warps, their PDFs and inverses
/// give pbrt-v4's values.
#[test]
fn known_values_wavelengths_and_exponential() {
    for &(u, lambda, pdf) in SAMPLE_VISIBLE_WAVELENGTHS {
        let ours = sample_visible_wavelengths(f32::from_bits(u));
        assert_ulps([ours], [lambda], 0, "SampleVisibleWavelengths");
        let ours = visible_wavelengths_pdf(f32::from_bits(lambda));
        assert_ulps([ours], [pdf], 3, "VisibleWavelengthsPDF");
    }

    for &(u, x, pdf, inverse) in SAMPLE_EXPONENTIAL {
        let ours = sample_exponential(f32::from_bits(u), 1.7);
        assert_ulps([ours], [x], 1, "SampleExponential");
        let x = f32::from_bits(x);
        assert_ulps([exponential_pdf(x, 1.7)], [pdf], 2, "ExponentialPDF");
        let ours = invert_exponential_sample(x, 1.7);
        assert_ulps([ours], [inverse], 0, "InvertExponentialSample");
    }
}

/// Discrete sampling and the multiple importance sampling heuristics give
/// pbrt-v4's values.
#[test]
fn known_values_discrete() {
    let weights = [0.1, 0.4, 0.0, 0.25, 0.25];
    for &(u, index, pmf, u_remapped) in SAMPLE_DISCRETE {
        let s = sample_discrete(&weights, f32::from_bits(u)).unwrap();
        assert_eq!(s.index, index, "SampleDiscrete index");
        let ours = [s.pmf, s.u_remapped];
        assert_ulps(ours, [pmf, u_remapped], 0, "SampleDiscrete");
    }

    for &(nf, f_pdf, ng, g_pdf, balance, power) in HEURISTICS {
        let (f_pdf, g_pdf) = (f32::from_bits(f_pdf), f32::from_bits(g_pdf));
        let ours = [
            balance_heuristic(nf, f_pdf, ng, g_pdf),
            power_heuristic(nf, f_pdf, ng, g_pdf),
        ];
        assert_ulps(
            ours,
            [balance, power],
            0,
            "BalanceHeuristic, PowerHeuristic",
        );
    }
}

/// `TEST(Sampling, HGExtremes)`: no NaN for `g` at or near ±1, at either
/// end of the sample range.
#[test]
fn sampling_hg_extremes() {
    for g in [-0.999, 0.999, -1.0, 1.0] {
        for u in [0.0, umbra_math::ONE_MINUS_EPSILON] {
            let s =
                sample_henyey_greenstein(Vector3f::new(0.0, 0.0, -1.0), g, Point2f::new(u, 0.5));
            assert!(!s.wi.has_nan(), "g {g}, u {u}: {:?}", s.wi);
        }
    }
}

/// The Henyey–Greenstein phase function and its sampling give pbrt-v4's
/// values.
#[test]
fn known_values_henyey_greenstein() {
    let wo = Vector3f::new(0.3, -0.5, 0.8).normalize();
    for &(g, u, wi, pdf) in SAMPLE_HENYEY_GREENSTEIN {
        let s = sample_henyey_greenstein(wo, f32::from_bits(g), p2(u));
        assert_ulps(xyz(s.wi), wi, 2, "SampleHenyeyGreenstein");
        assert_ulps([s.pdf], [pdf], 0, "SampleHenyeyGreenstein pdf");
    }
    for &(cos_theta, g, value) in HENYEY_GREENSTEIN {
        let ours = henyey_greenstein(f32::from_bits(cos_theta), f32::from_bits(g));
        assert_ulps([ours], [value], 0, "HenyeyGreenstein");
    }
}

/// A NaN argument propagates through the PDFs' range checks as in pbrt-v4,
/// whose `x < a || x > b` is false for NaN, rather than reading as outside.
#[test]
fn pdfs_propagate_nan() {
    assert!(linear_pdf(f32::NAN, 0.5, 2.0).is_nan());
    assert!(visible_wavelengths_pdf(f32::NAN).is_nan());
    assert!(bilinear_pdf(Point2f::new(f32::NAN, 0.5), [0.1, 0.5, 1.3, 4.7]).is_nan());
}
