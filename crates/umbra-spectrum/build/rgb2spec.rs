//! The Jakob–Hanika RGB-to-spectrum optimiser, pbrt-v4
//! `src/pbrt/cmd/rgb2spec_opt.cpp` at the pinned commit (Apache-2.0, see
//! `NOTICE`), ported operation for operation in `f64`.
//!
//! For each cell of a `3 × RES × RES × RES` grid over RGB (which component
//! is largest, then the largest value `z` and the other two relative to it),
//! Gauss–Newton fits the coefficients of a sigmoid of a quadratic in λ
//! whose reflectance under the colour space's illuminant has that RGB,
//! minimising the error in CIELAB (Jakob and Hanika 2019, PBR 4e §4.6.6).
//!
//! The result is a pure function of these sources (ADR 0022): IEEE `f64`
//! arithmetic, which Rust never contracts into FMAs, the correctly rounded
//! hardware square root, and glibc's cube root, through the wrapper of
//! `umbra_math::cmath`, whose bits are fixed by the image's Ubuntu digest.
//! pbrt-v4's own table can differ in the last bits of some coefficients: its
//! build lets the C++ compiler contract FMAs, and its glibc may differ.
//! Cells are spread over threads, but each cell's value does not depend on
//! which thread computes it or when.

use std::num::NonZeroUsize;
use std::sync::Mutex;
use std::sync::atomic::{AtomicU32, Ordering};
use std::thread;

use crate::cie_5nm::{CIE_SAMPLES, CIE_X, CIE_Y, CIE_Z};
use crate::cmath;

/// The table resolution along each axis (pbrt-v4 `RGBToSpectrumTableRes`).
const RES: u32 = 64;
/// [`RES`] as an index bound.
const N: usize = RES as usize;

const CIE_LAMBDA_MIN: f64 = 360.0;
const CIE_LAMBDA_MAX: f64 = 830.0;
/// The quadrature points: three per 5 nm interval, Simpson's 3/8 rule.
const CIE_FINE_SAMPLES: usize = (CIE_SAMPLES - 1) * 3 + 1;
/// The step of the central differences of the Jacobian.
const RGB2SPEC_EPSILON: f64 = 1e-4;
/// The Gauss–Newton iterations at most per cell.
const ITERATIONS: u32 = 15;

/// A colour space to optimise for: its illuminant at the CIE 5 nm samples
/// and its matrices to and from XYZ.
pub(crate) struct Gamut {
    /// The illuminant, already normalised.
    pub(crate) illuminant: [f64; CIE_SAMPLES],
    /// XYZ to linear RGB.
    pub(crate) xyz_to_rgb: [[f64; 3]; 3],
    /// Linear RGB to XYZ.
    pub(crate) rgb_to_xyz: [[f64; 3]; 3],
}

/// The table: the `RES` values of `z` the grid is spaced at, then the
/// three coefficients of every cell, indexed `[maxc][z][y][x][c]`, all as
/// `f32` (pbrt-v4 `*ToSpectrumTable_Scale` and `*ToSpectrumTable_Data`).
pub(crate) struct Table {
    /// The `z` nodes.
    pub(crate) scale: Vec<f32>,
    /// The coefficients.
    pub(crate) data: Vec<f32>,
}

/// The precomputed quadrature of `rgb2spec_opt`'s `init_tables`.
struct Tables {
    /// The wavelength of each quadrature point.
    lambda: [f64; CIE_FINE_SAMPLES],
    /// The weights that integrate a reflectance into each RGB component.
    rgb: [[f64; CIE_FINE_SAMPLES]; 3],
    rgb_to_xyz: [[f64; 3]; 3],
    whitepoint: [f64; 3],
}

/// The cell `(maxc, j, i)` of a grid column: which component is largest,
/// and the indices of the other two.
type ColumnIndex = (usize, u32, u32);

/// One column of the grid, the coefficients of one [`ColumnIndex`] for
/// every `z`.
type Column = Vec<[f32; 3]>;

/// pbrt-v4's `smoothstep`.
fn smoothstep(x: f64) -> f64 {
    x * x * (3.0 - 2.0 * x)
}

/// pbrt-v4's `sqr`.
fn sqr(x: f64) -> f64 {
    x * x
}

/// pbrt-v4's `sigmoid`.
fn sigmoid(x: f64) -> f64 {
    0.5 * x / (1.0 + x * x).sqrt() + 0.5
}

/// C++ `std::max`: `b` if `a < b`, otherwise `a`.
fn cpp_max(a: f64, b: f64) -> f64 {
    if a < b { b } else { a }
}

/// `data`, sampled at 5 nm from 360 nm, linearly interpolated at `x` nm
/// (pbrt-v4 `cie_interp`).
fn cie_interp(data: &[f64; CIE_SAMPLES], x: f64) -> f64 {
    #[expect(clippy::cast_precision_loss, reason = "94 intervals")]
    let x = (x - CIE_LAMBDA_MIN) * ((CIE_SAMPLES - 1) as f64 / (CIE_LAMBDA_MAX - CIE_LAMBDA_MIN));
    #[expect(
        clippy::cast_possible_truncation,
        clippy::cast_possible_wrap,
        reason = "pbrt-v4's `(int)x`, clamped to the 94 intervals"
    )]
    let offset = (x as i32).clamp(0, CIE_SAMPLES as i32 - 2);
    let weight = x - f64::from(offset);
    let offset = offset.unsigned_abs() as usize;
    (1.0 - weight) * data[offset] + weight * data[offset + 1]
}

/// Simpson's 3/8 weight factor of quadrature point `i`: 1 at the ends, 2 at
/// the joints of the 5 nm intervals, 3 inside them.
fn simpson_3_8_factor(i: usize) -> f64 {
    if i == 0 || i == CIE_FINE_SAMPLES - 1 {
        1.0
    } else if (i - 1) % 3 == 2 {
        2.0
    } else {
        3.0
    }
}

/// `Σ_j row[j] · xyz[j] · illuminant · weight`, summed in order from 0.
fn weighted_sum(row: [f64; 3], xyz: [f64; 3], illuminant: f64, weight: f64) -> f64 {
    row.iter()
        .zip(xyz)
        .fold(0.0, |acc, (m, x)| acc + m * x * illuminant * weight)
}

/// The quadrature of `gamut` (pbrt-v4 `init_tables(gamut)`).
fn tables(gamut: &Gamut) -> Tables {
    let mut tables = Tables {
        lambda: [0.0; CIE_FINE_SAMPLES],
        rgb: [[0.0; CIE_FINE_SAMPLES]; 3],
        rgb_to_xyz: gamut.rgb_to_xyz,
        whitepoint: [0.0; 3],
    };
    #[expect(clippy::cast_precision_loss, reason = "282 intervals")]
    let h = (CIE_LAMBDA_MAX - CIE_LAMBDA_MIN) / (CIE_FINE_SAMPLES - 1) as f64;
    for (i, step) in (0..CIE_FINE_SAMPLES).zip(0_u32..) {
        let lambda = CIE_LAMBDA_MIN + f64::from(step) * h;
        let xyz = [CIE_X, CIE_Y, CIE_Z].map(|cmf| cie_interp(&cmf, lambda));
        let illuminant = cie_interp(&gamut.illuminant, lambda);
        let weight = 3.0 / 8.0 * h * simpson_3_8_factor(i);

        tables.lambda[i] = lambda;
        let rgb = gamut
            .xyz_to_rgb
            .map(|row| weighted_sum(row, xyz, illuminant, weight));
        for (row, v) in tables.rgb.iter_mut().zip(rgb) {
            row[i] = v;
        }
        for (w, x) in tables.whitepoint.iter_mut().zip(xyz) {
            *w += x * illuminant * weight;
        }
    }
    tables
}

/// The CIELAB `f`: a cube root, linear near 0.
fn lab_f(t: f64) -> f64 {
    let delta = 6.0 / 29.0;
    if t > delta * delta * delta {
        cmath::cbrt(t)
    } else {
        t / (delta * delta * 3.0) + 4.0 / 29.0
    }
}

/// The CIELAB of linear RGB `p` (pbrt-v4 `cie_lab`).
fn cie_lab(tables: &Tables, p: [f64; 3]) -> [f64; 3] {
    let [x, y, z] = tables
        .rgb_to_xyz
        .map(|row| row.iter().zip(p).fold(0.0, |acc, (m, c)| acc + c * m));
    let [xw, yw, zw] = tables.whitepoint;
    [
        116.0 * lab_f(y / yw) - 16.0,
        500.0 * (lab_f(x / xw) - lab_f(y / yw)),
        200.0 * (lab_f(y / yw) - lab_f(z / zw)),
    ]
}

/// The CIELAB of `rgb` minus that of the reflectance with `coeffs`, the
/// coefficients of the quadratic in λ scaled to `[0, 1]`, highest degree
/// first (pbrt-v4 `eval_residual`).
fn residual(tables: &Tables, coeffs: [f64; 3], rgb: [f64; 3]) -> [f64; 3] {
    let mut out = [0.0; 3];
    for (i, &lambda) in tables.lambda.iter().enumerate() {
        let lambda = (lambda - CIE_LAMBDA_MIN) / (CIE_LAMBDA_MAX - CIE_LAMBDA_MIN);
        // Horner's rule from zero, as pbrt-v4's loop.
        let x = coeffs.iter().fold(0.0, |x, &c| x * lambda + c);
        let s = sigmoid(x);
        for (o, row) in out.iter_mut().zip(&tables.rgb) {
            *o += row[i] * s;
        }
    }
    let out = cie_lab(tables, out);
    let target = cie_lab(tables, rgb);
    [0, 1, 2].map(|j| target[j] - out[j])
}

/// The Jacobian of [`residual`] by central differences, `[j][i]` the
/// derivative of component `j` by coefficient `i` (pbrt-v4
/// `eval_jacobian`).
fn jacobian(tables: &Tables, coeffs: [f64; 3], rgb: [f64; 3]) -> [[f64; 3]; 3] {
    let mut jac = [[0.0; 3]; 3];
    for i in 0..3 {
        let mut tmp = coeffs;
        tmp[i] -= RGB2SPEC_EPSILON;
        let r0 = residual(tables, tmp, rgb);

        let mut tmp = coeffs;
        tmp[i] += RGB2SPEC_EPSILON;
        let r1 = residual(tables, tmp, rgb);

        for (row, (a, b)) in jac.iter_mut().zip(r0.into_iter().zip(r1)) {
            row[i] = (b - a) * 1.0 / (2.0 * RGB2SPEC_EPSILON);
        }
    }
    jac
}

/// One Gauss–Newton step from `coeffs` towards `rgb`, with the
/// coefficients scaled down if the largest exceeds 200; the squared norm of
/// the residual before the step (pbrt-v4 `gauss_newton`'s loop body).
///
/// # Panics
///
/// If the Jacobian is singular, where pbrt-v4 throws.
fn gauss_newton_step(tables: &Tables, rgb: [f64; 3], coeffs: &mut [f64; 3]) -> f64 {
    let residual = residual(tables, *coeffs, rgb);
    let mut jac = jacobian(tables, *coeffs, rgb);
    let Some(permutation) = lup_decompose(&mut jac, 1e-15) else {
        panic!("RGB {rgb:?} -> {coeffs:?}: LU decomposition failed");
    };
    let x = lup_solve(&jac, permutation, residual);

    let mut r = 0.0;
    for ((c, x), res) in coeffs.iter_mut().zip(x).zip(residual) {
        *c -= x;
        r += res * res;
    }
    let max = cpp_max(cpp_max(coeffs[0], coeffs[1]), coeffs[2]);
    if max > 200.0 {
        *coeffs = coeffs.map(|c| c * (200.0 / max));
    }
    r
}

/// Refines `coeffs` towards `rgb` until the residual is small (pbrt-v4
/// `gauss_newton`).
fn gauss_newton(tables: &Tables, rgb: [f64; 3], coeffs: &mut [f64; 3]) {
    for _ in 0..ITERATIONS {
        if gauss_newton_step(tables, rgb, coeffs) < 1e-6 {
            break;
        }
    }
}

/// The pivot row for column `i` of `a`: the first row from `i` with the
/// largest magnitude, and that magnitude, 0 if none is positive.
fn pivot(a: &[[f64; 3]; 3], i: usize) -> (usize, f64) {
    let (mut imax, mut max_a) = (i, 0.0);
    for (k, row) in a.iter().enumerate().skip(i) {
        if row[i].abs() > max_a {
            (imax, max_a) = (k, row[i].abs());
        }
    }
    (imax, max_a)
}

/// Eliminates column `i` of `a` below the diagonal, in place, keeping the
/// multipliers there.
fn eliminate(a: &mut [[f64; 3]; 3], i: usize) {
    for j in i + 1..3 {
        a[j][i] /= a[i][i];
        for k in i + 1..3 {
            a[j][k] -= a[j][i] * a[i][k];
        }
    }
}

/// The LU decomposition with partial pivoting of `a`, in place, and its row
/// permutation, or `None` if `a` is degenerate to within `tol` (pbrt-v4
/// `LUPDecompose`, which swaps row pointers where this swaps rows).
fn lup_decompose(a: &mut [[f64; 3]; 3], tol: f64) -> Option<[usize; 3]> {
    let mut p = [0, 1, 2];
    for i in 0..3 {
        let (imax, max_a) = pivot(a, i);
        if max_a < tol {
            return None;
        }
        p.swap(i, imax);
        a.swap(i, imax);
        eliminate(a, i);
    }
    Some(p)
}

/// The solution `x` of `A x = b` from the decomposition of
/// [`lup_decompose`] (pbrt-v4 `LUPSolve`).
fn lup_solve(a: &[[f64; 3]; 3], p: [usize; 3], b: [f64; 3]) -> [f64; 3] {
    let mut x = [0.0; 3];
    for i in 0..3 {
        x[i] = b[p[i]];
        for k in 0..i {
            x[i] -= a[i][k] * x[k];
        }
    }
    for i in (0..3).rev() {
        for k in i + 1..3 {
            x[i] -= a[i][k] * x[k];
        }
        x[i] /= a[i][i];
    }
    x
}

/// The coefficients of the polynomial in λ in nm from those in λ scaled to
/// `[0, 1]`, rounded to `f32`, highest degree first.
fn to_wavelength_coefficients([a, b, c]: [f64; 3]) -> [f32; 3] {
    let c0 = 360.0;
    let c1 = 1.0 / (830.0 - 360.0);
    #[expect(clippy::cast_possible_truncation, reason = "pbrt-v4's `float(...)`")]
    [
        (a * sqr(c1)) as f32,
        (b * c1 - 2.0 * a * c0 * sqr(c1)) as f32,
        (c - b * c0 * c1 + a * sqr(c0 * c1)) as f32,
    ]
}

/// The cells of the column `(maxc, j, i)`: `z` up from [`RES`]` / 5`, each
/// fit starting from the previous one, then down from it to 0, starting
/// again from zero (pbrt-v4 `main`'s inner loops).
fn column(tables: &Tables, scale: &[f32], (maxc, j, i): ColumnIndex) -> Column {
    let y = f64::from(j) / f64::from(RES - 1);
    let x = f64::from(i) / f64::from(RES - 1);
    let mut cells = vec![[0.0; 3]; N];
    let mut fit = |coeffs: &mut [f64; 3], k: usize| {
        let b = f64::from(scale[k]);
        let mut rgb = [0.0; 3];
        rgb[maxc] = b;
        rgb[(maxc + 1) % 3] = x * b;
        rgb[(maxc + 2) % 3] = y * b;
        gauss_newton(tables, rgb, coeffs);
        cells[k] = to_wavelength_coefficients(*coeffs);
    };
    let start = N / 5;
    let mut coeffs = [0.0; 3];
    for k in start..N {
        fit(&mut coeffs, k);
    }
    let mut coeffs = [0.0; 3];
    for k in (0..=start).rev() {
        fit(&mut coeffs, k);
    }
    cells
}

/// The `z` nodes: `smoothstep(smoothstep(k / (RES − 1)))` rounded to `f32`.
fn scale() -> Vec<f32> {
    #[expect(clippy::cast_possible_truncation, reason = "pbrt-v4's `(float)`")]
    (0..RES)
        .map(|k| smoothstep(smoothstep(f64::from(k) / f64::from(RES - 1))) as f32)
        .collect()
}

/// Optimises rows of columns, one `(maxc, j)` at a time, taking the next
/// from `next` until all `3 RES` are done.
fn worker(
    tables: &Tables,
    scale: &[f32],
    next: &AtomicU32,
    columns: &Mutex<Vec<(ColumnIndex, Column)>>,
) {
    loop {
        let row = next.fetch_add(1, Ordering::Relaxed);
        if row >= 3 * RES {
            return;
        }
        let (maxc, j) = ((row / RES) as usize, row % RES);
        let row: Vec<_> = (0..RES)
            .map(|i| ((maxc, j, i), column(tables, scale, (maxc, j, i))))
            .collect();
        columns
            .lock()
            .expect("no worker panics while holding the lock")
            .extend(row);
    }
}

/// The columns laid out as `[maxc][z][y][x][c]`.
fn scatter(columns: Vec<(ColumnIndex, Column)>) -> Vec<f32> {
    let mut data = vec![0.0; 3 * N * N * N * 3];
    for ((maxc, j, i), cells) in columns {
        let (j, i) = (j as usize, i as usize);
        for (k, cell) in cells.into_iter().enumerate() {
            let idx = ((maxc * N + k) * N + j) * N + i;
            data[3 * idx..3 * idx + 3].copy_from_slice(&cell);
        }
    }
    data
}

/// Optimises the table of `gamut` (pbrt-v4 `rgb2spec_opt <RES> <file>
/// <gamut>`), on every available core.
pub(crate) fn optimise(gamut: &Gamut) -> Table {
    let tables = tables(gamut);
    let scale = scale();
    let next = AtomicU32::new(0);
    let columns = Mutex::new(Vec::with_capacity(3 * N * N));
    let threads = thread::available_parallelism().map_or(1, NonZeroUsize::get);
    thread::scope(|s| {
        for _ in 0..threads {
            s.spawn(|| worker(&tables, &scale, &next, &columns));
        }
    });
    let columns = columns
        .into_inner()
        .expect("no worker panicked while holding the lock");
    Table {
        data: scatter(columns),
        scale,
    }
}
