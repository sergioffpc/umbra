//! Filters (pbrt-v4 `src/pbrt/base/filter.h`, `filters.h`, `filters.cpp`):
//! how much a Pixel Sample counts in its pixel, by where in the pixel it
//! falls, through the closed [`FilterKind`] (ADR 0006).
//!
//! Filters are importance-sampled (ADR 0001): a Pixel Sample's film position
//! is drawn from the filter's distribution, and it carries the filter's
//! value over that distribution's PDF as its weight. Each Pixel Sample then
//! contributes to its own pixel only.
//!
//! The core has no allocator (ADR 0014): a filter's tabulated distribution
//! lives in storage `S` its caller provides, as `umbra-sampling`'s
//! piecewise-constant distributions do. The Scene owns a `FilterKind<Vec<f32>>`;
//! per-sample code samples its [`view`](FilterKind::view), a
//! `FilterKind<&[f32]>`.

use umbra_geometry::Bounds2f;
use umbra_math::{Point2f, Vector2f, gaussian, gaussian_integral};
use umbra_sampling::PiecewiseConstant2D;

/// A point drawn from a filter, relative to the pixel's centre, and its
/// weight: the filter's value there over the PDF it was drawn with.
///
/// PBR 4e §8.8.1, pbrt-v4 `FilterSample`.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct FilterSample {
    /// The offset from the pixel's centre.
    pub p: Point2f,
    /// The filter's value at `p` over the PDF of `p`.
    pub weight: f32,
}

/// A pixel reconstruction filter.
///
/// PBR 4e §8.8.1, pbrt-v4 `Filter`.
pub trait Filter {
    /// The half-widths of the filter's support, around the origin.
    ///
    /// pbrt-v4 `Filter::Radius`.
    fn radius(&self) -> Vector2f;

    /// The filter's value at `p`, an offset from the pixel's centre.
    ///
    /// pbrt-v4 `Filter::Evaluate`.
    fn evaluate(&self, p: Point2f) -> f32;

    /// The integral of the filter over its support.
    ///
    /// pbrt-v4 `Filter::Integral`.
    fn integral(&self) -> f32;

    /// The point drawn from the filter's distribution with `u`.
    ///
    /// pbrt-v4 `Filter::Sample`.
    fn sample(&self, u: Point2f) -> FilterSample;
}

/// A filter tabulated at 32 points per unit of radius in each direction,
/// and the piecewise-constant distribution of the table.
///
/// PBR 4e §8.8.3, pbrt-v4 `FilterSampler`.
// `repr(C)` for every `S`, as every type of the filter: the GPU Worker
// copies it to the device byte for byte, with its storage in device memory
// (ADR 0028, arc42 §8.12).
#[derive(Debug, Clone, PartialEq)]
#[repr(C)]
pub struct FilterSampler<S> {
    /// The tabulated values, rows of `nu` one after the other.
    f: S,
    nu: usize,
    distrib: PiecewiseConstant2D<S>,
}

impl<S> FilterSampler<S> {
    /// The same sampler, its storage moved by `f`: to device memory, for the
    /// GPU Worker (ADR 0028).
    #[must_use]
    pub fn map_storage<T>(self, mut f: impl FnMut(S) -> T) -> FilterSampler<T> {
        FilterSampler {
            f: f(self.f),
            nu: self.nu,
            distrib: self.distrib.map_storage(f),
        }
    }
}

impl<S: AsRef<[f32]> + AsMut<[f32]>> FilterSampler<S> {
    /// The table of `evaluate` over `[-radius, radius]`, in storage of the
    /// lengths `alloc` is given, called once for the table and once for the
    /// distribution.
    ///
    /// # Panics
    ///
    /// If either radius is below 1/32, or `alloc` returns storage of
    /// another length.
    ///
    /// pbrt-v4 `FilterSampler(filter)`.
    pub fn new_in(
        radius: Vector2f,
        evaluate: impl Fn(Point2f) -> f32,
        mut alloc: impl FnMut(usize) -> S,
    ) -> Self {
        let domain = Bounds2f::new(Point2f::new(-radius.x, -radius.y), radius.into());
        let [nu, nv] = [radius.x, radius.y].map(table_size);
        let mut f = alloc(nu * nv);
        assert_eq!(f.as_ref().len(), nu * nv, "storage of the wrong length");
        let cells = f
            .as_mut()
            .chunks_exact_mut(nu)
            .zip(0..)
            .flat_map(|(row, y)| row.iter_mut().zip(0..).map(move |(value, x)| (value, x, y)));
        for (value, x, y) in cells {
            let t = Point2f::new(cell_centre(x, nu), cell_centre(y, nv));
            *value = evaluate(domain.lerp(t));
        }
        let ranges = [
            domain.p_min.x..domain.p_max.x,
            domain.p_min.y..domain.p_max.y,
        ];
        let distrib = PiecewiseConstant2D::new_in(f.as_ref(), nu, ranges, alloc);
        Self { f, nu, distrib }
    }
}

/// The number of table entries across a radius: pbrt-v4's
/// `int(32 * radius)`.
#[expect(
    clippy::cast_possible_truncation,
    clippy::cast_sign_loss,
    reason = "as pbrt-v4: int(32 * radius) of a positive radius"
)]
fn table_size(radius: f32) -> usize {
    let n = (32.0 * radius) as usize;
    assert!(n > 0, "a filter radius of {radius} has no table entry");
    n
}

/// The centre of cell `i` of `n`, as a fraction of the domain: pbrt-v4's
/// `(i + 0.5f) / n`.
#[expect(
    clippy::cast_precision_loss,
    reason = "as pbrt-v4: int to Float, exact for a table size"
)]
fn cell_centre(i: u32, n: usize) -> f32 {
    (i as f32 + 0.5) / n as f32
}

impl<S: AsRef<[f32]>> FilterSampler<S> {
    /// The point drawn from the distribution with `u`, and the tabulated
    /// value of its cell over its PDF.
    ///
    /// pbrt-v4 `FilterSampler::Sample`.
    #[must_use]
    pub fn sample(&self, u: Point2f) -> FilterSample {
        let s = self.distrib.sample(u);
        let [x, y] = s.offset;
        FilterSample {
            p: s.p,
            weight: self.f.as_ref()[y * self.nu + x] / s.pdf,
        }
    }

    /// The same sampler, borrowed.
    #[must_use]
    pub fn view(&self) -> FilterSampler<&[f32]> {
        FilterSampler {
            f: self.f.as_ref(),
            nu: self.nu,
            distrib: self.distrib.view(),
        }
    }
}

/// The parameters of a [`GaussianFilter`]'s function, which its sampler is
/// tabulated from.
#[derive(Debug, Clone, Copy, PartialEq)]
#[repr(C)]
struct Gaussian {
    radius: Vector2f,
    sigma: f32,
    exp_x: f32,
    exp_y: f32,
}

impl Gaussian {
    /// pbrt-v4 `GaussianFilter::Evaluate`.
    fn evaluate(&self, p: Point2f) -> f32 {
        (gaussian(p.x, 0.0, self.sigma) - self.exp_x).max(0.0)
            * (gaussian(p.y, 0.0, self.sigma) - self.exp_y).max(0.0)
    }
}

/// The Gaussian of standard deviation `sigma`, shifted down to 0 at the
/// radius so it has no discontinuity there.
///
/// PBR 4e §8.8.4, pbrt-v4 `GaussianFilter`.
#[derive(Debug, Clone, PartialEq)]
#[repr(C)]
pub struct GaussianFilter<S> {
    sampler: FilterSampler<S>,
    gaussian: Gaussian,
}

impl<S> GaussianFilter<S> {
    /// The same filter, its storage moved by `f`: to device memory, for the
    /// GPU Worker (ADR 0028).
    #[must_use]
    pub fn map_storage<T>(self, f: impl FnMut(S) -> T) -> GaussianFilter<T> {
        GaussianFilter {
            sampler: self.sampler.map_storage(f),
            gaussian: self.gaussian,
        }
    }
}

impl<S: AsRef<[f32]> + AsMut<[f32]>> GaussianFilter<S> {
    /// The filter of `radius` and `sigma`, its sampler in storage `alloc`
    /// returns (see [`FilterSampler::new_in`]).
    ///
    /// pbrt-v4 `GaussianFilter(radius, sigma)`; pbrt-v4's defaults are a
    /// radius of 1.5 and a `sigma` of 0.5.
    pub fn new_in(radius: Vector2f, sigma: f32, alloc: impl FnMut(usize) -> S) -> Self {
        let gaussian = Gaussian {
            radius,
            sigma,
            exp_x: gaussian(radius.x, 0.0, sigma),
            exp_y: gaussian(radius.y, 0.0, sigma),
        };
        let sampler = FilterSampler::new_in(radius, |p| gaussian.evaluate(p), alloc);
        Self { sampler, gaussian }
    }
}

impl<S: AsRef<[f32]>> GaussianFilter<S> {
    /// The same filter, borrowed.
    #[must_use]
    pub fn view(&self) -> GaussianFilter<&[f32]> {
        GaussianFilter {
            gaussian: self.gaussian,
            sampler: self.sampler.view(),
        }
    }
}

impl<S: AsRef<[f32]>> Filter for GaussianFilter<S> {
    fn radius(&self) -> Vector2f {
        self.gaussian.radius
    }

    fn evaluate(&self, p: Point2f) -> f32 {
        self.gaussian.evaluate(p)
    }

    fn integral(&self) -> f32 {
        let Gaussian {
            radius,
            sigma,
            exp_x,
            exp_y,
        } = self.gaussian;
        (gaussian_integral(-radius.x, radius.x, 0.0, sigma) - 2.0 * radius.x * exp_x)
            * (gaussian_integral(-radius.y, radius.y, 0.0, sigma) - 2.0 * radius.y * exp_y)
    }

    fn sample(&self, u: Point2f) -> FilterSample {
        self.sampler.sample(u)
    }
}

// The GPU Worker copies the filter to the device byte for byte, its tables
// in device memory (ADR 0028, arc42 §8.12): its layout is the same on
// `x86_64` and `nvptx64`.
const _: () = {
    use core::mem::offset_of;

    // The storage as the GPU Worker's: a device address and a length, 16
    // bytes aligned to 8 (`umbra-gpu-programs`' `DeviceSlice`).
    type DeviceStorage = [u64; 2];

    assert!(size_of::<Gaussian>() == 20, "`Gaussian` is 20 bytes");
    assert!(
        offset_of!(Gaussian, sigma) == 8,
        "`Gaussian::sigma` is at byte 8"
    );
    assert!(
        offset_of!(Gaussian, exp_x) == 12,
        "`Gaussian::exp_x` is at byte 12"
    );
    assert!(
        offset_of!(Gaussian, exp_y) == 16,
        "`Gaussian::exp_y` is at byte 16"
    );
    assert!(
        size_of::<FilterSampler<DeviceStorage>>() == 80,
        "`FilterSampler` is its table, `nu` and its distribution"
    );
    assert!(
        offset_of!(FilterSampler<DeviceStorage>, nu) == 16,
        "`FilterSampler::nu` is at byte 16"
    );
    assert!(
        offset_of!(FilterSampler<DeviceStorage>, distrib) == 24,
        "`FilterSampler::distrib` is at byte 24"
    );
    assert!(
        size_of::<GaussianFilter<DeviceStorage>>() == 104,
        "`GaussianFilter` is 104 bytes, the last 4 padding"
    );
    assert!(
        offset_of!(GaussianFilter<DeviceStorage>, gaussian) == 80,
        "`GaussianFilter::gaussian` is at byte 80"
    );
    assert!(
        size_of::<FilterKind<DeviceStorage>>() == 112,
        "`FilterKind` is its `u32` tag, padded to 8, then the filter"
    );
};

/// The filters, one per Render Job, held by value and matched on every call
/// (ADR 0006). Only pbrt-v4's `gaussian` so far.
///
/// pbrt-v4 `Filter`'s tagged pointer.
#[derive(Debug, Clone, PartialEq)]
#[repr(C, u32)]
pub enum FilterKind<S> {
    /// pbrt-v4 `"gaussian"`.
    Gaussian(GaussianFilter<S>),
}

impl<S> From<GaussianFilter<S>> for FilterKind<S> {
    fn from(filter: GaussianFilter<S>) -> Self {
        Self::Gaussian(filter)
    }
}

impl<S> FilterKind<S> {
    /// The same filter, its storage moved by `f`: to device memory, for the
    /// GPU Worker (ADR 0028).
    #[must_use]
    pub fn map_storage<T>(self, f: impl FnMut(S) -> T) -> FilterKind<T> {
        match self {
            Self::Gaussian(filter) => FilterKind::Gaussian(filter.map_storage(f)),
        }
    }
}

impl<S: AsRef<[f32]>> FilterKind<S> {
    /// The same filter, borrowed: what per-sample code samples.
    #[must_use]
    pub fn view(&self) -> FilterKind<&[f32]> {
        match self {
            Self::Gaussian(f) => FilterKind::Gaussian(f.view()),
        }
    }
}

impl<S: AsRef<[f32]>> Filter for FilterKind<S> {
    #[inline]
    fn radius(&self) -> Vector2f {
        match self {
            Self::Gaussian(f) => f.radius(),
        }
    }

    #[inline]
    fn evaluate(&self, p: Point2f) -> f32 {
        match self {
            Self::Gaussian(f) => f.evaluate(p),
        }
    }

    #[inline]
    fn integral(&self) -> f32 {
        match self {
            Self::Gaussian(f) => f.integral(),
        }
    }

    #[inline]
    fn sample(&self, u: Point2f) -> FilterSample {
        match self {
            Self::Gaussian(f) => f.sample(u),
        }
    }
}
