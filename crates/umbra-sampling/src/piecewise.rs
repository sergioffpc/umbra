//! Piecewise-constant distributions (pbrt-v4 `src/pbrt/util/sampling.h`),
//! which sample tabulated functions such as a Filter or an image by
//! inverting their CDF.
//!
//! The core has no allocator (ADR 0014), so a distribution lives in storage
//! its caller provides: `new_in` takes an `alloc` that returns a buffer of
//! the length asked for, as pbrt-v4's constructors take an `Allocator`. A
//! `std` crate passes `|n| vec![0.0; n]`; per-sample code samples a
//! distribution over a borrowed slice, without allocating.

use core::ops::Range;

use umbra_math::{Point2f, clamp, find_interval, lerp};

/// One piecewise-constant function and its CDF, borrowed from the storage
/// of a [`PiecewiseConstant1D`] or of a row of a [`PiecewiseConstant2D`].
#[derive(Clone, Copy)]
struct Distribution1D<'a> {
    func: &'a [f32],
    cdf: &'a [f32],
    min: f32,
    max: f32,
    func_int: f32,
}

/// `n` as a float, pbrt-v4's `Float(size_t)`: exact for the sizes of a
/// tabulated function, below 2²⁴.
#[expect(clippy::cast_precision_loss, reason = "as pbrt-v4: Float(size_t)")]
fn float(n: usize) -> f32 {
    n as f32
}

/// The non-negative `x` truncated to an index, pbrt-v4's `int(x)`.
#[expect(
    clippy::cast_possible_truncation,
    clippy::cast_sign_loss,
    reason = "as pbrt-v4: int(x), here of a non-negative x"
)]
fn truncate(x: f32) -> usize {
    debug_assert!(x >= 0.0 || x.is_nan(), "truncate: negative {x}");
    x as usize
}

/// Replaces the function values in `func` by their absolute values and
/// fills `cdf`, one longer, with their normalised running integral over
/// `[min, max]`; returns the integral.
fn build_1d(func: &mut [f32], cdf: &mut [f32], min: f32, max: f32) -> f32 {
    assert!(max > min, "the domain [{min}, {max}] is empty");
    assert_eq!(
        cdf.len(),
        func.len() + 1,
        "a CDF has one value more than its function"
    );
    let n = float(func.len());
    for f in func.iter_mut() {
        *f = f.abs();
    }

    cdf[0] = 0.0;
    for (i, &f) in func.iter().enumerate() {
        cdf[i + 1] = cdf[i] + f * (max - min) / n;
    }

    let func_int = cdf[func.len()];
    if func_int == 0.0 {
        for (i, c) in cdf.iter_mut().enumerate().skip(1) {
            *c = float(i) / n;
        }
    } else {
        for c in cdf.iter_mut().skip(1) {
            *c /= func_int;
        }
    }
    func_int
}

/// A value drawn from a [`PiecewiseConstant1D`].
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct PiecewiseConstant1DSample {
    /// The sampled point of the domain.
    pub x: f32,
    /// The PDF at `x`, 0 if the function is zero everywhere.
    pub pdf: f32,
    /// The index of the piece `x` is in.
    pub offset: usize,
}

impl Distribution1D<'_> {
    /// pbrt-v4 `PiecewiseConstant1D::Sample`.
    fn sample(self, u: f32) -> PiecewiseConstant1DSample {
        let cdf = self.cdf;
        let o = find_interval(cdf.len(), |index| cdf[index] <= u);
        assert!(
            o + 1 < cdf.len(),
            "find_interval returns an interval of the CDF"
        );

        let mut du = u - cdf[o];
        if cdf[o + 1] - cdf[o] > 0.0 {
            du /= cdf[o + 1] - cdf[o];
        }
        debug_assert!(!du.is_nan(), "the CDF offset of {u} is NaN");

        let pdf = if self.func_int > 0.0 {
            self.func[o] / self.func_int
        } else {
            0.0
        };
        let x = lerp((float(o) + du) / float(self.func.len()), self.min, self.max);
        PiecewiseConstant1DSample { x, pdf, offset: o }
    }

    /// pbrt-v4 `PiecewiseConstant1D::Invert`.
    fn invert(self, x: f32) -> Option<f32> {
        if x < self.min || x > self.max {
            return None;
        }
        let n = self.func.len();
        let c = (x - self.min) / (self.max - self.min) * float(n);
        let offset = clamp(truncate(c), 0, n - 1);
        let delta = c - float(offset);
        Some(lerp(delta, self.cdf[offset], self.cdf[offset + 1]))
    }
}

/// A piecewise-constant function of `n` equal pieces over `[min, max]`, and
/// its CDF, to sample it by inversion. Negative values count as their
/// absolute value. `S` is the storage: `2n + 1` values, the function then
/// the CDF.
///
/// PBR 4e §A.4.1, pbrt-v4 `PiecewiseConstant1D`.
#[derive(Debug, Clone, PartialEq)]
pub struct PiecewiseConstant1D<S> {
    storage: S,
    n: usize,
    min: f32,
    max: f32,
    func_int: f32,
}

impl<S: AsRef<[f32]> + AsMut<[f32]>> PiecewiseConstant1D<S> {
    /// The distribution of `f` over `domain`, in storage of the length
    /// `alloc` is given.
    ///
    /// # Panics
    ///
    /// If `f` is empty, the domain is empty, or `alloc` returns storage of
    /// another length.
    ///
    /// pbrt-v4 `PiecewiseConstant1D(f, min, max, alloc)`.
    pub fn new_in(f: &[f32], domain: Range<f32>, alloc: impl FnOnce(usize) -> S) -> Self {
        assert!(!f.is_empty(), "a piecewise-constant function has a piece");
        let n = f.len();
        let mut storage = alloc(2 * n + 1);
        let (func, cdf) = storage.as_mut().split_at_mut(n);
        func.copy_from_slice(f);
        let func_int = build_1d(func, cdf, domain.start, domain.end);
        Self {
            storage,
            n,
            min: domain.start,
            max: domain.end,
            func_int,
        }
    }
}

impl<S: AsRef<[f32]>> PiecewiseConstant1D<S> {
    /// The borrowed function and CDF.
    fn view(&self) -> Distribution1D<'_> {
        let (func, cdf) = self.storage.as_ref().split_at(self.n);
        Distribution1D {
            func,
            cdf,
            min: self.min,
            max: self.max,
            func_int: self.func_int,
        }
    }

    /// The integral of the function over its domain.
    ///
    /// pbrt-v4 `PiecewiseConstant1D::Integral`.
    #[must_use]
    pub fn integral(&self) -> f32 {
        self.func_int
    }

    /// The number of pieces.
    ///
    /// pbrt-v4 `PiecewiseConstant1D::size`.
    #[must_use]
    pub fn size(&self) -> usize {
        self.n
    }

    /// The absolute values of the function, one per piece.
    ///
    /// pbrt-v4 `PiecewiseConstant1D::func`.
    #[must_use]
    pub fn func(&self) -> &[f32] {
        self.view().func
    }

    /// The CDF at the `n + 1` ends of the pieces, from 0 to 1.
    ///
    /// pbrt-v4 `PiecewiseConstant1D::cdf`.
    #[must_use]
    pub fn cdf(&self) -> &[f32] {
        self.view().cdf
    }

    /// The point of the domain the CDF maps to `u`, with its PDF; for a
    /// function that is zero everywhere, uniformly with PDF 0.
    ///
    /// PBR 4e §A.4.1, pbrt-v4 `PiecewiseConstant1D::Sample`.
    #[must_use]
    pub fn sample(&self, u: f32) -> PiecewiseConstant1DSample {
        self.view().sample(u)
    }

    /// The `u` that [`sample`](Self::sample) maps to `x`, or `None` if `x`
    /// is outside the domain.
    ///
    /// pbrt-v4 `PiecewiseConstant1D::Invert`.
    #[must_use]
    pub fn invert(&self, x: f32) -> Option<f32> {
        self.view().invert(x)
    }
}

/// `x` relative to the domain `[min, max]`, in `[0, 1]` inside it (pbrt-v4
/// `Bounds2f::Offset`, one axis).
fn domain_offset(x: f32, [min, max]: [f32; 2]) -> f32 {
    if max > min {
        (x - min) / (max - min)
    } else {
        x - min
    }
}

/// The index of the cell of `n` that the offset `t` falls in, clamped to
/// the grid (pbrt-v4 `Clamp(int(t * n), 0, n - 1)`).
#[expect(clippy::cast_possible_truncation, reason = "as pbrt-v4: int(Float)")]
fn cell(t: f32, n: usize) -> usize {
    let i = (t * float(n)) as i64;
    usize::try_from(i).map_or(0, |i| i.min(n - 1))
}

/// A value drawn from a [`PiecewiseConstant2D`].
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct PiecewiseConstant2DSample {
    /// The sampled point of the domain.
    pub p: Point2f,
    /// The PDF at `p`.
    pub pdf: f32,
    /// The column and row of the cell `p` is in.
    pub offset: [usize; 2],
}

/// A piecewise-constant function of `nu × nv` equal cells over a rectangle,
/// sampled as a marginal distribution over rows (`v`) and, given the row, a
/// conditional distribution over its cells (`u`). Negative values count as
/// their absolute value. `S` is the storage: the `nv` conditional functions
/// and their CDFs, then the marginal function and its CDF, `2 nu nv + 3 nv + 1`
/// values in all.
///
/// PBR 4e §A.5.7, pbrt-v4 `PiecewiseConstant2D`.
#[derive(Debug, Clone, PartialEq)]
pub struct PiecewiseConstant2D<S> {
    storage: S,
    nu: usize,
    nv: usize,
    u_domain: [f32; 2],
    v_domain: [f32; 2],
    integral: f32,
}

impl<S: AsRef<[f32]> + AsMut<[f32]>> PiecewiseConstant2D<S> {
    /// The distribution of `func`, rows of `nu` values one after the other,
    /// over `domain` (`u` then `v`), in storage of the length `alloc` is
    /// given.
    ///
    /// # Panics
    ///
    /// If `func` is empty or not whole rows of `nu` values, either side of
    /// the domain is empty, or `alloc` returns storage of another length.
    ///
    /// pbrt-v4 `PiecewiseConstant2D(func, nu, nv, domain, alloc)`.
    pub fn new_in(
        func: &[f32],
        nu: usize,
        domain: [Range<f32>; 2],
        alloc: impl FnOnce(usize) -> S,
    ) -> Self {
        assert!(
            nu > 0 && !func.is_empty(),
            "a piecewise-constant function has a piece"
        );
        assert_eq!(
            func.len() % nu,
            0,
            "{} values are not rows of {nu}",
            func.len()
        );
        let nv = func.len() / nu;
        let [u, v] = domain;
        let mut storage = alloc(2 * nu * nv + 3 * nv + 1);

        let (conditional_func, rest) = storage.as_mut().split_at_mut(nu * nv);
        let (conditional_cdf, marginal) = rest.split_at_mut(nv * (nu + 1));
        let (marginal_func, marginal_cdf) = marginal.split_at_mut(nv);
        conditional_func.copy_from_slice(func);
        let rows = conditional_func
            .chunks_exact_mut(nu)
            .zip(conditional_cdf.chunks_exact_mut(nu + 1));
        for ((row_func, row_cdf), row_int) in rows.zip(marginal_func.iter_mut()) {
            *row_int = build_1d(row_func, row_cdf, u.start, u.end);
        }
        let integral = build_1d(marginal_func, marginal_cdf, v.start, v.end);

        Self {
            storage,
            nu,
            nv,
            u_domain: [u.start, u.end],
            v_domain: [v.start, v.end],
            integral,
        }
    }
}

impl<S: AsRef<[f32]>> PiecewiseConstant2D<S> {
    /// The same distribution, borrowed: what per-sample code samples.
    #[must_use]
    pub fn view(&self) -> PiecewiseConstant2D<&[f32]> {
        PiecewiseConstant2D {
            storage: self.storage.as_ref(),
            nu: self.nu,
            nv: self.nv,
            u_domain: self.u_domain,
            v_domain: self.v_domain,
            integral: self.integral,
        }
    }

    /// The marginal distribution over rows.
    fn marginal(&self) -> Distribution1D<'_> {
        let (nu, nv) = (self.nu, self.nv);
        let (func, cdf) = self.storage.as_ref()[2 * nu * nv + nv..].split_at(nv);
        Distribution1D {
            func,
            cdf,
            min: self.v_domain[0],
            max: self.v_domain[1],
            func_int: self.integral,
        }
    }

    /// The conditional distribution over row `v`.
    fn conditional(&self, v: usize) -> Distribution1D<'_> {
        let (nu, nv) = (self.nu, self.nv);
        let storage = self.storage.as_ref();
        let cdf_start = nu * nv + v * (nu + 1);
        Distribution1D {
            func: &storage[v * nu..(v + 1) * nu],
            cdf: &storage[cdf_start..=cdf_start + nu],
            min: self.u_domain[0],
            max: self.u_domain[1],
            func_int: storage[2 * nu * nv + nv + v],
        }
    }

    /// The integral of the function over its domain.
    ///
    /// pbrt-v4 `PiecewiseConstant2D::Integral`.
    #[must_use]
    pub fn integral(&self) -> f32 {
        self.integral
    }

    /// A point of the domain drawn by the marginal distribution with `u.y`
    /// and then the conditional one with `u.x`, with its PDF.
    ///
    /// PBR 4e §A.5.7, pbrt-v4 `PiecewiseConstant2D::Sample`.
    #[must_use]
    pub fn sample(&self, u: Point2f) -> PiecewiseConstant2DSample {
        let d1 = self.marginal().sample(u.y);
        let d0 = self.conditional(d1.offset).sample(u.x);
        PiecewiseConstant2DSample {
            p: Point2f::new(d0.x, d1.x),
            pdf: d0.pdf * d1.pdf,
            offset: [d0.offset, d1.offset],
        }
    }

    /// The PDF of [`sample`](Self::sample) at `p`, clamped into the domain.
    ///
    /// pbrt-v4 `PiecewiseConstant2D::PDF`.
    #[must_use]
    pub fn pdf(&self, p: Point2f) -> f32 {
        let iu = cell(domain_offset(p.x, self.u_domain), self.nu);
        let iv = cell(domain_offset(p.y, self.v_domain), self.nv);
        self.conditional(iv).func[iu] / self.integral
    }

    /// The `u` that [`sample`](Self::sample) maps to `p`, or `None` if `p` is
    /// outside the domain.
    ///
    /// pbrt-v4 `PiecewiseConstant2D::Invert`.
    #[must_use]
    pub fn invert(&self, p: Point2f) -> Option<Point2f> {
        let m_inv = self.marginal().invert(p.y)?;
        let [v_min, v_max] = self.v_domain;
        let p1o = (p.y - v_min) / (v_max - v_min);
        // pbrt-v4's comparisons, as in the 1D inverse.
        if p1o < 0.0 || p1o > 1.0 {
            return None;
        }
        let nv = float(self.nv);
        let row = truncate(clamp(p1o * nv, 0.0, nv - 1.0));
        let c_inv = self.conditional(row).invert(p.x)?;
        Some(Point2f::new(c_inv, m_inv))
    }
}
