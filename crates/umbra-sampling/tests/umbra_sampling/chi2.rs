//! The χ² goodness-of-fit test of a sampling routine against its PDF
//! (arc42 §8.11), after pbrt-v3's and Mitsuba's: samples are binned on a
//! grid, each cell's expected count is the PDF integrated over it, cells
//! expected to get fewer than [`MIN_EXPECTED`] samples are pooled, and the
//! routine fails if the p-value is below the significance level corrected
//! for the number of χ² tests in the suite.

use core::ops::Range;

use umbra_math::double;

/// The significance level of the suite as a whole.
const SIGNIFICANCE: f64 = 0.01;
/// The number of χ² tests in the suite, for the Šidák correction.
const TEST_COUNT: i32 = 20;
/// Cells expected to receive fewer samples are pooled together.
const MIN_EXPECTED: f64 = 5.0;
/// How far outside the grid, relative to its size, a sample still counts as
/// on its edge: a warp computed in `f32` can land a rounding error beyond
/// the boundary of its domain.
const EDGE_TOLERANCE: f64 = 1e-5;

/// The nodes and weights of 5-point Gauss–Legendre quadrature on `[-1, 1]`.
/// It never evaluates at a cell's edges, so a PDF that jumps there (a
/// piecewise-constant function, the rim of a hemisphere) integrates exactly.
const GAUSS_LEGENDRE_5: [(f64, f64); 5] = [
    (-0.906_179_845_938_664, 0.236_926_885_056_189_1),
    (-0.538_469_310_105_683_1, 0.478_628_670_499_366_5),
    (0.0, 0.568_888_888_888_888_9),
    (0.538_469_310_105_683_1, 0.478_628_670_499_366_5),
    (0.906_179_845_938_664, 0.236_926_885_056_189_1),
];

/// The integral of `f` over `[a, b]` by 5-point Gauss–Legendre on `n` equal
/// sub-intervals.
pub(crate) fn integrate(f: impl Fn(f64) -> f64, a: f64, b: f64, n: u32) -> f64 {
    let h = (b - a) / f64::from(n);
    (0..n)
        .map(|i| {
            let mid = a + h * (f64::from(i) + 0.5);
            GAUSS_LEGENDRE_5
                .iter()
                .map(|&(x, w)| w * f(mid + 0.5 * h * x))
                .sum::<f64>()
                * 0.5
                * h
        })
        .sum()
}

/// The integral of `f` over the rectangle `x × y`, `n × n` sub-rectangles.
pub(crate) fn integrate_2d(
    f: impl Fn(f64, f64) -> f64,
    x: &Range<f64>,
    y: &Range<f64>,
    n: u32,
) -> f64 {
    integrate(
        |xi| integrate(|yi| f(xi, yi), y.start, y.end, n),
        x.start,
        x.end,
        n,
    )
}

/// A grid of `res[0] × res[1]` equal cells over `x × y`.
#[derive(Debug, Clone)]
pub(crate) struct Grid {
    pub(crate) x: Range<f64>,
    pub(crate) y: Range<f64>,
    pub(crate) res: [usize; 2],
}

impl Grid {
    /// A one-dimensional grid of `n` cells over `x`.
    pub(crate) fn line(x: Range<f64>, n: usize) -> Self {
        Self {
            x,
            y: 0.0..1.0,
            res: [n, 1],
        }
    }

    /// The cell along one axis of `t` in `range`, `None` if outside.
    fn axis_cell(t: f64, range: &Range<f64>, n: usize) -> Option<usize> {
        let size = range.end - range.start;
        let f = (t - range.start) / size;
        if !(-EDGE_TOLERANCE..=1.0 + EDGE_TOLERANCE).contains(&f) {
            return None;
        }
        #[expect(
            clippy::cast_possible_truncation,
            clippy::cast_sign_loss,
            reason = "f is in [0, 1]"
        )]
        #[expect(clippy::cast_precision_loss, reason = "a few hundred cells")]
        let i = (f.max(0.0) * n as f64) as usize;
        Some(i.min(n - 1))
    }

    /// The index of the cell `p` is in, `None` if outside the grid.
    fn cell(&self, [px, py]: [f64; 2]) -> Option<usize> {
        let column = Self::axis_cell(px, &self.x, self.res[0])?;
        let row = Self::axis_cell(py, &self.y, self.res[1])?;
        Some(row * self.res[0] + column)
    }

    /// The integral of `pdf` over each cell, in [`Grid::cell`] order.
    #[expect(clippy::cast_precision_loss, reason = "a few hundred cells")]
    fn integrals(&self, pdf: &impl Fn(f64, f64) -> f64) -> Vec<f64> {
        let [nx, ny] = self.res;
        let dx = (self.x.end - self.x.start) / nx as f64;
        let dy = (self.y.end - self.y.start) / ny as f64;
        (0..ny)
            .flat_map(|row| (0..nx).map(move |column| (column, row)))
            .map(|(column, row)| {
                let x0 = self.x.start + dx * column as f64;
                let y0 = self.y.start + dy * row as f64;
                integrate_2d(pdf, &(x0..x0 + dx), &(y0..y0 + dy), 2)
            })
            .collect()
    }
}

/// Asserts that `samples`, points of `grid`'s domain, are distributed as
/// `pdf`. Samples outside the grid fall in one extra cell, expected to get
/// what the PDF leaves outside it.
#[track_caller]
pub(crate) fn assert_chi2(
    what: &str,
    samples: impl IntoIterator<Item = [f64; 2]>,
    pdf: impl Fn(f64, f64) -> f64,
    grid: &Grid,
) {
    let cells = grid.res[0] * grid.res[1];
    let mut observed = vec![0.0; cells + 1];
    let mut count = 0.0;
    for p in samples {
        observed[grid.cell(p).unwrap_or(cells)] += 1.0;
        count += 1.0;
    }
    let mut expected: Vec<f64> = grid.integrals(&pdf).iter().map(|e| e * count).collect();
    let inside: f64 = expected.iter().sum();
    expected.push((count - inside).max(0.0));
    assert_chi2_counts(what, &observed, &expected);
}

/// Asserts that the counts `observed` follow the expected counts
/// `expected`, cell for cell.
#[track_caller]
pub(crate) fn assert_chi2_counts(what: &str, observed: &[f64], expected: &[f64]) {
    let count: f64 = observed.iter().sum();
    let mut cells: Vec<(f64, f64)> = expected
        .iter()
        .copied()
        .zip(observed.iter().copied())
        .collect();
    cells.sort_by(|a, b| a.0.total_cmp(&b.0));

    let (mut chi2, mut dof) = (0.0, 0_u32);
    let (mut pooled_expected, mut pooled_observed) = (0.0, 0.0);
    for (e, o) in cells {
        if e <= count * 1e-9 {
            // A rounding error's worth of expectation: a sample here is a
            // sample where the PDF says there is none.
            assert!(
                o <= count * 1e-5,
                "{what}: {o} samples where the PDF is zero"
            );
        } else if e < MIN_EXPECTED || (pooled_expected > 0.0 && pooled_expected < MIN_EXPECTED) {
            pooled_expected += e;
            pooled_observed += o;
        } else {
            chi2 += (o - e) * (o - e) / e;
            dof += 1;
        }
    }
    if pooled_expected > 0.0 {
        chi2 += (pooled_observed - pooled_expected) * (pooled_observed - pooled_expected)
            / pooled_expected;
        dof += 1;
    }
    let dof = dof.saturating_sub(1);
    assert!(dof > 0, "{what}: too few cells for a χ² test");

    let p_value = 1.0 - regularized_gamma_p(f64::from(dof) / 2.0, chi2 / 2.0);
    let alpha = 1.0 - double::pow(1.0 - SIGNIFICANCE, 1.0 / f64::from(TEST_COUNT));
    assert!(
        p_value >= alpha,
        "{what}: χ² = {chi2:.1} with {dof} degrees of freedom, p = {p_value:e} < {alpha:e}"
    );
}

/// The regularised lower incomplete gamma function `P(a, x)`, by its series
/// below `a + 1` and its continued fraction above (Numerical Recipes §6.2).
fn regularized_gamma_p(a: f64, x: f64) -> f64 {
    if x <= 0.0 {
        return 0.0;
    }
    let prefactor = double::exp(a * double::log(x) - x - double::lgamma(a));
    if x < a + 1.0 {
        prefactor * gamma_series(a, x)
    } else {
        1.0 - prefactor * gamma_continued_fraction(a, x)
    }
}

/// The series of `P(a, x)` without its prefactor `xᵃ e⁻ˣ / Γ(a)`.
fn gamma_series(a: f64, x: f64) -> f64 {
    let (mut term, mut sum, mut n) = (1.0 / a, 1.0 / a, a);
    while term.abs() > sum.abs() * 1e-15 {
        n += 1.0;
        term *= x / n;
        sum += term;
    }
    sum
}

/// The continued fraction of `Q(a, x) = 1 − P(a, x)` without its prefactor,
/// by Lentz's method.
fn gamma_continued_fraction(a: f64, x: f64) -> f64 {
    const TINY: f64 = 1e-300;
    let not_tiny = |v: f64| if v.abs() < TINY { TINY } else { v };
    let mut b = x + 1.0 - a;
    let (mut c, mut d) = (1.0 / TINY, 1.0 / b);
    let mut h = d;
    for i in 1..1000 {
        let an = -f64::from(i) * (f64::from(i) - a);
        b += 2.0;
        d = 1.0 / not_tiny(an * d + b);
        c = not_tiny(b + an / c);
        let delta = d * c;
        h *= delta;
        if (delta - 1.0).abs() < 1e-15 {
            break;
        }
    }
    h
}
