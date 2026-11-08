//! One-dimensional distributions: linear on `[0, 1]`, exponential, and the
//! visible-wavelength distribution Films sample their wavelengths from.

use umbra_math::{ONE_MINUS_EPSILON, lerp, sqr, sqrt};

/// The PDF on `[0, 1]` of the linear function from `a` at 0 to `b` at 1,
/// both non-negative; 0 outside `[0, 1]`.
///
/// PBR 4e §A.4.2, pbrt-v4 `LinearPDF`.
#[must_use]
pub fn linear_pdf(x: f32, a: f32, b: f32) -> f32 {
    debug_assert!(a >= 0.0 && b >= 0.0, "linear_pdf: negative end {a}, {b}");
    // pbrt-v4's comparisons, so a NaN `x` propagates.
    if x < 0.0 || x > 1.0 {
        return 0.0;
    }
    2.0 * lerp(x, a, b) / (a + b)
}

/// A sample of [`linear_pdf`] in `[0, 1)`, by inverting its CDF in a form
/// that is stable when `a ≈ b`.
///
/// PBR 4e §A.4.2, pbrt-v4 `SampleLinear`.
#[must_use]
pub fn sample_linear(u: f32, a: f32, b: f32) -> f32 {
    debug_assert!(a >= 0.0 && b >= 0.0, "sample_linear: negative end {a}, {b}");
    if u == 0.0 && a == 0.0 {
        return 0.0;
    }
    let x = u * (a + b) / (a + sqrt(lerp(u, sqr(a), sqr(b))));
    umbra_math::min(x, ONE_MINUS_EPSILON)
}

/// The `u` that [`sample_linear`] maps to `x`: the CDF at `x`.
///
/// pbrt-v4 `InvertLinearSample`.
#[must_use]
pub fn invert_linear_sample(x: f32, a: f32, b: f32) -> f32 {
    x * (a * (2.0 - x) + b * x) / (a + b)
}

/// The PDF `a·e^(−a·x)` of the exponential distribution of rate `a > 0`.
///
/// PBR 4e §A.4.5, pbrt-v4 `ExponentialPDF`.
#[must_use]
pub fn exponential_pdf(x: f32, a: f32) -> f32 {
    debug_assert!(a > 0.0, "exponential_pdf: rate {a}");
    a * libm::expf(-a * x)
}

/// A sample of [`exponential_pdf`] by inverting its CDF.
///
/// PBR 4e §A.4.5, pbrt-v4 `SampleExponential`.
#[must_use]
pub fn sample_exponential(u: f32, a: f32) -> f32 {
    debug_assert!(a > 0.0, "sample_exponential: rate {a}");
    -libm::logf(1.0 - u) / a
}

/// The `u` that [`sample_exponential`] maps to `x`: the CDF at `x`.
///
/// pbrt-v4 `InvertExponentialSample`.
#[must_use]
pub fn invert_exponential_sample(x: f32, a: f32) -> f32 {
    debug_assert!(a > 0.0, "invert_exponential_sample: rate {a}");
    1.0 - libm::expf(-a * x)
}

/// The PDF over wavelengths in nm of [`sample_visible_wavelengths`]:
/// proportional to `1 / cosh²(0.0072 (λ − 538))` on `[360, 830]`, a fit to
/// the human visual response; 0 outside it.
///
/// PBR 4e §5.4.2, pbrt-v4 `VisibleWavelengthsPDF`.
#[must_use]
pub fn visible_wavelengths_pdf(lambda: f32) -> f32 {
    // pbrt-v4's comparisons, so a NaN `lambda` propagates.
    if lambda < 360.0 || lambda > 830.0 {
        return 0.0;
    }
    0.003_939_804 / sqr(libm::coshf(0.0072 * (lambda - 538.0)))
}

/// A wavelength in nm distributed as [`visible_wavelengths_pdf`].
///
/// PBR 4e §5.4.2, pbrt-v4 `SampleVisibleWavelengths`.
#[must_use]
pub fn sample_visible_wavelengths(u: f32) -> f32 {
    538.0 - 138.888_89 * libm::atanhf(0.856_910_65 - 1.827_502 * u)
}
