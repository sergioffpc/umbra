//! `filters_test.cpp`, for the Gaussian filter, the only one ported so far;
//! `TEST(Sinc, ZeroHandling)` is the sinc filter's. Then the `known_values`
//! tests of `GaussianFilter` against `known_values.rs`.

use umbra_camera::{Filter, FilterKind, GaussianFilter};
use umbra_math::{Point2f, Vector2f, lerp};

use super::known_values::{GAUSSIAN_EVALUATE, GAUSSIAN_INTEGRAL, GAUSSIAN_SAMPLE};
use super::{assert_ulps, floats};

/// pbrt-v4's `GaussianFilter(radius, sigma)`, its table in `Vec`s.
fn gaussian(radius: Vector2f, sigma: f32) -> FilterKind<Vec<f32>> {
    GaussianFilter::new_in(radius, sigma, |n| vec![0.0; n]).into()
}

#[test]
fn filter_zero_past_radius() {
    for r in [
        Vector2f::new(1.0, 1.0),
        Vector2f::new(1.5, 0.25),
        Vector2f::new(0.33, 5.2),
        Vector2f::new(0.1, 0.1),
        Vector2f::new(3.0, 3.0),
    ] {
        let f = gaussian(r, 0.5);
        let e = 1e-3;
        for p in [
            (0.0, r.y + e),
            (r.x, r.y + e),
            (-r.x, r.y + e),
            (0.0, -r.y - e),
            (r.x, -r.y - e),
            (-r.x, -r.y - e),
            (r.x + e, 0.0),
            (r.x + e, r.y),
            (r.x + e, -r.y),
            (-r.x - e, 0.0),
            (-r.x - e, r.y),
            (-r.x - e, -r.y),
        ] {
            let p = Point2f::new(p.0, p.1);
            assert_eq!(f.evaluate(p), 0.0, "radius {r:?} at {p:?}");
        }
    }
}

/// The midpoint-rule integral of `f` over its support, on a 256 × 256 grid.
///
/// pbrt-v4 `integrateFilter`.
fn integrate_filter(f: &impl Filter) -> f32 {
    const SQRT_SAMPLES: u16 = 256;
    let r = f.radius();
    let area = 2.0 * r.x * 2.0 * r.y;
    let centre = |i: u16| (f32::from(i) + 0.5) / f32::from(SQRT_SAMPLES);
    let sum: f32 = (0..SQRT_SAMPLES)
        .flat_map(|y| (0..SQRT_SAMPLES).map(move |x| (centre(x), centre(y))))
        .map(|(ux, uy)| f.evaluate(Point2f::new(lerp(ux, -r.x, r.x), lerp(uy, -r.y, r.y))))
        .sum();
    sum / f32::from(SQRT_SAMPLES) / f32::from(SQRT_SAMPLES) * area
}

#[test]
fn filter_integral() {
    let approx_equal = |a: f32, b: f32| {
        if a.abs().max(b.abs()) < 1e-3 {
            (a - b).abs() < 1e-5
        } else {
            2.0 * (a - b).abs() / (a + b).abs() < 1e-2
        }
    };
    for r in [(1.0, 1.0), (2.5, 1.0), (1.0, 2.5), (3.4, 2.5)] {
        let f = gaussian(Vector2f::new(r.0, r.1), 0.5);
        let (integral, estimate) = (f.integral(), integrate_filter(&f));
        assert!(
            approx_equal(integral, estimate),
            "radius {r:?}: integral {integral}, estimate {estimate}"
        );
    }
}

/// The filter of a `[radius.x, radius.y, sigma]` row of the tables.
fn filter_of(params: [u32; 3]) -> FilterKind<Vec<f32>> {
    let [rx, ry, sigma] = floats(params);
    gaussian(Vector2f::new(rx, ry), sigma)
}

#[test]
fn known_values_gaussian() {
    for &(params, p, value) in GAUSSIAN_EVALUATE {
        let [x, y] = floats(p);
        let actual = filter_of(params).evaluate(Point2f::new(x, y));
        assert_ulps([actual], [value], 4, "Evaluate");
    }
    for &(params, integral) in GAUSSIAN_INTEGRAL {
        assert_ulps([filter_of(params).integral()], [integral], 4, "Integral");
    }
}

#[test]
fn known_values_gaussian_sample() {
    for &(params, u, expected) in GAUSSIAN_SAMPLE {
        let [ux, uy] = floats(u);
        let filter = filter_of(params);
        let s = filter.view().sample(Point2f::new(ux, uy));
        assert_ulps([s.p.x, s.p.y, s.weight], expected, 8, "Sample");
    }
}
