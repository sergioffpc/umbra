//! `SampledSpectrum` and `SampledWavelengths` on worked examples, the edge
//! cases of pbrt-v4's definitions included: its `operator bool`, `SafeDiv`,
//! `Clamp` and `ClampZero` on zeros and NaNs, and the order of its
//! reductions.

use umbra_spectrum::{SampledSpectrum, SampledWavelengths};

fn s(v: [f32; 4]) -> SampledSpectrum {
    SampledSpectrum::from(v)
}

#[test]
fn arithmetic_is_lane_wise() {
    let a = s([1.0, 2.0, 3.0, 4.0]);
    let b = s([0.5, -1.0, 2.0, 8.0]);
    assert_eq!((a + b).to_array(), [1.5, 1.0, 5.0, 12.0]);
    assert_eq!((a - b).to_array(), [0.5, 3.0, 1.0, -4.0]);
    assert_eq!((a * b).to_array(), [0.5, -2.0, 6.0, 32.0]);
    assert_eq!((a / b).to_array(), [2.0, -2.0, 1.5, 0.5]);
    assert_eq!((2.0 * a).to_array(), [2.0, 4.0, 6.0, 8.0]);
    assert_eq!((a / 2.0).to_array(), [0.5, 1.0, 1.5, 2.0]);
    assert_eq!((1.0 - a).to_array(), [0.0, -1.0, -2.0, -3.0]);
    assert_eq!((-a).to_array(), [-1.0, -2.0, -3.0, -4.0]);
    assert_eq!(a[2], 3.0);
}

#[test]
fn nonzero_and_nans() {
    assert!(!SampledSpectrum::splat(0.0).is_nonzero());
    assert!(!SampledSpectrum::splat(-0.0).is_nonzero());
    assert!(s([0.0, 0.0, 0.0, 1e-30]).is_nonzero());
    // pbrt-v4's `operator bool` tests `!= 0`, which a NaN passes.
    assert!(s([0.0, f32::NAN, 0.0, 0.0]).is_nonzero());
    assert!(s([0.0, f32::NAN, 0.0, 0.0]).has_nans());
    assert!(!s([f32::INFINITY, 0.0, 0.0, 0.0]).has_nans());
}

#[test]
fn reductions() {
    let v = s([3.0, -1.0, 4.0, 1.5]);
    assert_eq!(v.min_component_value(), -1.0);
    assert_eq!(v.max_component_value(), 4.0);
    assert_eq!(v.average(), 1.875);
    // Summed in order: (1e8 + 1) + (−1e8) + 1 is 1, not 2.
    assert_eq!(s([1e8, 1.0, -1e8, 1.0]).average(), 0.25);
}

#[test]
fn safe_div_is_zero_where_the_divisor_is() {
    let q = s([1.0, 2.0, -3.0, 0.0]).safe_div(s([2.0, 0.0, -0.0, 0.0]));
    assert_eq!(q.to_array(), [0.5, 0.0, 0.0, 0.0]);
}

#[test]
fn clamps() {
    let v = s([-1.0, 0.5, 2.0, -0.0]);
    assert_eq!(v.clamp(0.0, 1.0).to_array(), [0.0, 0.5, 1.0, -0.0]);
    // `std::max(0, v)`: 0 unless 0 < v.
    let z = s([-1.0, 0.5, f32::NAN, -0.0]).clamp_zero().to_array();
    assert_eq!(z.map(f32::to_bits), [0.0, 0.5, 0.0, 0.0].map(f32::to_bits));
    assert_eq!(
        s([-1e-4, 4.0, 9.0, 0.0]).safe_sqrt().to_array(),
        [0.0, 2.0, 3.0, 0.0]
    );
}

#[test]
fn interpolation() {
    let (a, b) = (s([0.0, 1.0, 2.0, 3.0]), s([4.0, 5.0, 6.0, 7.0]));
    assert_eq!(
        SampledSpectrum::lerp(0.25, a, b).to_array(),
        [1.0, 2.0, 3.0, 4.0]
    );
    let corners = [0.0, 1.0, 2.0, 3.0].map(SampledSpectrum::splat);
    assert_eq!(
        SampledSpectrum::bilerp([0.5, 0.5], corners).to_array(),
        [1.5; 4]
    );
}

#[test]
fn uniform_wavelengths_wrap_around() {
    let lambda = SampledWavelengths::sample_uniform(0.5, 400.0..800.0);
    assert_eq!(
        [0, 1, 2, 3].map(|i| lambda[i]),
        [600.0, 700.0, 800.0, 500.0]
    );
    assert_eq!(lambda.pdf().to_array(), [1.0 / 400.0; 4]);
}

#[test]
fn terminate_secondary_keeps_the_first_wavelength() {
    let mut lambda = SampledWavelengths::sample_uniform(0.0, 400.0..800.0);
    assert!(!lambda.secondary_terminated());
    lambda.terminate_secondary();
    assert!(lambda.secondary_terminated());
    assert_eq!(lambda.pdf().to_array(), [1.0 / 1600.0, 0.0, 0.0, 0.0]);
    // A second termination changes nothing.
    lambda.terminate_secondary();
    assert_eq!(lambda.pdf().to_array(), [1.0 / 1600.0, 0.0, 0.0, 0.0]);
}

#[test]
fn wavelengths_come_apart_into_their_parts_and_back() {
    let lambda =
        SampledWavelengths::from_parts([400.0, 500.0, 600.0, 700.0], s([0.5, 1.0, 2.0, 4.0]));
    assert_eq!(
        [lambda[0], lambda[1], lambda[2], lambda[3]],
        [400.0, 500.0, 600.0, 700.0]
    );
    assert_eq!(lambda.pdf().to_array(), [0.5, 1.0, 2.0, 4.0]);
    let (wavelengths, pdf) = lambda.to_parts();
    assert_eq!(wavelengths, [400.0, 500.0, 600.0, 700.0]);
    assert_eq!(pdf.to_array(), [0.5, 1.0, 2.0, 4.0]);
}
