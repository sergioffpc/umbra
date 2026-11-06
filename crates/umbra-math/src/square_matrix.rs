//! `N × N` matrices (pbrt-v4 `SquareMatrix`, `src/pbrt/util/math.h`).

use core::ops::{Add, Div, Index, IndexMut, Mul};

use crate::float::fma;
use crate::math::{difference_of_products, inner_product};
use crate::vecmath::{Vector2f, Vector3f};

/// An `N × N` matrix of `f32`, stored by rows.
///
/// PBR 4e §B.2.12, pbrt-v4 `SquareMatrix`.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct SquareMatrix<const N: usize> {
    m: [[f32; N]; N],
}

impl<const N: usize> Default for SquareMatrix<N> {
    /// The identity, as pbrt-v4's default constructor.
    fn default() -> Self {
        Self::diag([1.0; N])
    }
}

impl<const N: usize> From<[[f32; N]; N]> for SquareMatrix<N> {
    fn from(m: [[f32; N]; N]) -> Self {
        Self { m }
    }
}

impl<const N: usize> SquareMatrix<N> {
    /// The matrix with the given rows.
    #[must_use]
    pub const fn new(rows: [[f32; N]; N]) -> Self {
        Self { m: rows }
    }

    /// The matrix whose element `(i, j)` is `f(i, j)`.
    pub(crate) fn from_fn(f: impl Fn(usize, usize) -> f32) -> Self {
        Self {
            m: core::array::from_fn(|i| core::array::from_fn(|j| f(i, j))),
        }
    }

    /// pbrt-v4 `SquareMatrix::Zero`.
    #[must_use]
    pub const fn zero() -> Self {
        Self { m: [[0.0; N]; N] }
    }

    /// The diagonal matrix with the given diagonal.
    ///
    /// pbrt-v4 `SquareMatrix::Diag`.
    #[must_use]
    pub fn diag(d: [f32; N]) -> Self {
        Self::from_fn(|i, j| if i == j { d[i] } else { 0.0 })
    }

    /// The rows.
    #[must_use]
    pub const fn rows(&self) -> &[[f32; N]; N] {
        &self.m
    }

    /// pbrt-v4 `SquareMatrix::IsIdentity`.
    #[must_use]
    pub fn is_identity(&self) -> bool {
        *self == Self::default()
    }

    /// pbrt-v4 `Transpose`.
    #[must_use]
    pub fn transpose(&self) -> Self {
        Self::from_fn(|i, j| self.m[j][i])
    }

    /// Each element combined with `f`.
    fn map(&self, f: impl Fn(f32) -> f32) -> Self {
        Self {
            m: self.m.map(|row| row.map(&f)),
        }
    }
}

impl<const N: usize> Index<usize> for SquareMatrix<N> {
    type Output = [f32; N];
    /// Row `i`.
    fn index(&self, i: usize) -> &[f32; N] {
        &self.m[i]
    }
}

impl<const N: usize> IndexMut<usize> for SquareMatrix<N> {
    fn index_mut(&mut self, i: usize) -> &mut [f32; N] {
        &mut self.m[i]
    }
}

impl<const N: usize> Add for SquareMatrix<N> {
    type Output = Self;
    fn add(self, o: Self) -> Self {
        Self::from_fn(|i, j| self.m[i][j] + o.m[i][j])
    }
}

impl<const N: usize> Mul<f32> for SquareMatrix<N> {
    type Output = Self;
    fn mul(self, s: f32) -> Self {
        self.map(|v| v * s)
    }
}

impl<const N: usize> Div<f32> for SquareMatrix<N> {
    type Output = Self;
    fn div(self, s: f32) -> Self {
        debug_assert!(s != 0.0, "division of a matrix by zero");
        self.map(|v| v / s)
    }
}

impl<const N: usize> Mul<[f32; N]> for SquareMatrix<N> {
    type Output = [f32; N];
    /// The matrix-vector product, each row summed left to right from 0.
    ///
    /// pbrt-v4 `Mul(SquareMatrix, T)`.
    fn mul(self, v: [f32; N]) -> [f32; N] {
        self.m
            .map(|row| row.iter().zip(&v).fold(0.0, |acc, (&a, &b)| acc + a * b))
    }
}

impl Mul<Vector2f> for SquareMatrix<2> {
    type Output = Vector2f;
    fn mul(self, v: Vector2f) -> Vector2f {
        let [x, y] = self * [v.x, v.y];
        Vector2f::new(x, y)
    }
}

impl Mul<Vector3f> for SquareMatrix<3> {
    type Output = Vector3f;
    fn mul(self, v: Vector3f) -> Vector3f {
        let [x, y, z] = self * [v.x, v.y, v.z];
        Vector3f::new(x, y, z)
    }
}

impl Mul for SquareMatrix<2> {
    type Output = Self;
    /// Each element accumulated with FMA, as pbrt-v4's generic product.
    fn mul(self, o: Self) -> Self {
        Self::from_fn(|i, j| (0..2).fold(0.0, |r, k| fma(self.m[i][k], o.m[k][j], r)))
    }
}

impl Mul for SquareMatrix<3> {
    type Output = Self;
    /// Each element by `InnerProduct`.
    fn mul(self, o: Self) -> Self {
        let (a, b) = (self.m, o.m);
        Self::from_fn(|i, j| {
            inner_product([(a[i][0], b[0][j]), (a[i][1], b[1][j]), (a[i][2], b[2][j])])
        })
    }
}

impl Mul for SquareMatrix<4> {
    type Output = Self;
    /// Each element by `InnerProduct`.
    fn mul(self, o: Self) -> Self {
        let (a, b) = (self.m, o.m);
        Self::from_fn(|i, j| {
            inner_product([
                (a[i][0], b[0][j]),
                (a[i][1], b[1][j]),
                (a[i][2], b[2][j]),
                (a[i][3], b[3][j]),
            ])
        })
    }
}

impl SquareMatrix<2> {
    /// pbrt-v4 `Determinant(SquareMatrix<2>)`.
    #[must_use]
    pub fn determinant(&self) -> f32 {
        let m = &self.m;
        difference_of_products(m[0][0], m[1][1], m[0][1], m[1][0])
    }

    /// The inverse, or `None` for a singular matrix.
    ///
    /// Not pbrt-v4's algorithm: pbrt-v4 inverts a 2 × 2 matrix by its generic
    /// Gauss–Jordan elimination; Umbra uses the adjugate over the determinant,
    /// as it does for 3 × 3.
    #[must_use]
    pub fn inverse(&self) -> Option<Self> {
        let det = self.determinant();
        if det == 0.0 {
            return None;
        }
        let inv_det = 1.0 / det;
        let m = &self.m;
        Some(Self::new([
            [inv_det * m[1][1], -inv_det * m[0][1]],
            [-inv_det * m[1][0], inv_det * m[0][0]],
        ]))
    }
}

impl SquareMatrix<3> {
    /// pbrt-v4 `Determinant(SquareMatrix<3>)`.
    #[must_use]
    pub fn determinant(&self) -> f32 {
        let m = &self.m;
        let minor12 = difference_of_products(m[1][1], m[2][2], m[1][2], m[2][1]);
        let minor02 = difference_of_products(m[1][0], m[2][2], m[1][2], m[2][0]);
        let minor01 = difference_of_products(m[1][0], m[2][1], m[1][1], m[2][0]);
        fma(
            m[0][2],
            minor01,
            difference_of_products(m[0][0], minor12, m[0][1], minor02),
        )
    }

    /// The inverse, or `None` for a singular matrix.
    ///
    /// pbrt-v4 `Inverse(SquareMatrix<3>)`.
    #[must_use]
    pub fn inverse(&self) -> Option<Self> {
        let det = self.determinant();
        if det == 0.0 {
            return None;
        }
        let inv_det = 1.0 / det;
        let m = &self.m;
        let cofactor =
            |a: (usize, usize), b: (usize, usize), c: (usize, usize), d: (usize, usize)| {
                inv_det * difference_of_products(m[a.0][a.1], m[b.0][b.1], m[c.0][c.1], m[d.0][d.1])
            };
        Some(Self::new([
            [
                cofactor((1, 1), (2, 2), (1, 2), (2, 1)),
                cofactor((0, 2), (2, 1), (0, 1), (2, 2)),
                cofactor((0, 1), (1, 2), (0, 2), (1, 1)),
            ],
            [
                cofactor((1, 2), (2, 0), (1, 0), (2, 2)),
                cofactor((0, 0), (2, 2), (0, 2), (2, 0)),
                cofactor((0, 2), (1, 0), (0, 0), (1, 2)),
            ],
            [
                cofactor((1, 0), (2, 1), (1, 1), (2, 0)),
                cofactor((0, 1), (2, 0), (0, 0), (2, 1)),
                cofactor((0, 0), (1, 1), (0, 1), (1, 0)),
            ],
        ]))
    }
}

/// The 2 × 2 minors of the top two rows (`s`) and the bottom two rows (`c`)
/// that the 4 × 4 determinant and inverse share (Eberly, *Laplace Expansion
/// Theorem*).
struct Minors4 {
    s: [f32; 6],
    c: [f32; 6],
}

impl Minors4 {
    fn new(m: &[[f32; 4]; 4]) -> Self {
        let minor = |r0: usize, r1: usize, i: usize, j: usize| {
            difference_of_products(m[r0][i], m[r1][j], m[r1][i], m[r0][j])
        };
        let pairs = [(0, 1), (0, 2), (0, 3), (1, 2), (1, 3), (2, 3)];
        Self {
            s: pairs.map(|(i, j)| minor(0, 1, i, j)),
            c: pairs.map(|(i, j)| minor(2, 3, i, j)),
        }
    }
}

impl SquareMatrix<4> {
    /// pbrt-v4 `Determinant(SquareMatrix<4>)`.
    #[must_use]
    pub fn determinant(&self) -> f32 {
        let Minors4 { s, c } = Minors4::new(&self.m);
        difference_of_products(s[0], c[5], s[1], c[4])
            + difference_of_products(s[2], c[3], -s[3], c[2])
            + difference_of_products(s[5], c[0], s[4], c[1])
    }

    /// The inverse, or `None` for a singular matrix, by shared 2 × 2 minors
    /// and compensated inner products.
    ///
    /// pbrt-v4 `Inverse(SquareMatrix<4>)`.
    #[must_use]
    pub fn inverse(&self) -> Option<Self> {
        let Minors4 { s, c } = Minors4::new(&self.m);
        let det = inner_product([
            (s[0], c[5]),
            (-s[1], c[4]),
            (s[2], c[3]),
            (s[3], c[2]),
            (s[5], c[0]),
            (-s[4], c[1]),
        ]);
        if det == 0.0 {
            return None;
        }
        Some(scaled_adjugate4(&self.m, &s, &c, 1.0 / det))
    }
}

/// The adjugate of `m` from its shared minors, each element scaled by
/// `scale` as pbrt-v4's `s * InnerProduct(…)`.
fn scaled_adjugate4(m: &[[f32; 4]; 4], s: &[f32; 6], c: &[f32; 6], scale: f32) -> SquareMatrix<4> {
    let ip = |a: f32, x: f32, b: f32, y: f32, d: f32, z: f32| {
        scale * inner_product([(a, x), (b, y), (d, z)])
    };
    SquareMatrix::new([
        [
            ip(m[1][1], c[5], m[1][3], c[3], -m[1][2], c[4]),
            ip(-m[0][1], c[5], m[0][2], c[4], -m[0][3], c[3]),
            ip(m[3][1], s[5], m[3][3], s[3], -m[3][2], s[4]),
            ip(-m[2][1], s[5], m[2][2], s[4], -m[2][3], s[3]),
        ],
        [
            ip(-m[1][0], c[5], m[1][2], c[2], -m[1][3], c[1]),
            ip(m[0][0], c[5], m[0][3], c[1], -m[0][2], c[2]),
            ip(-m[3][0], s[5], m[3][2], s[2], -m[3][3], s[1]),
            ip(m[2][0], s[5], m[2][3], s[1], -m[2][2], s[2]),
        ],
        [
            ip(m[1][0], c[4], m[1][3], c[0], -m[1][1], c[2]),
            ip(-m[0][0], c[4], m[0][1], c[2], -m[0][3], c[0]),
            ip(m[3][0], s[4], m[3][3], s[0], -m[3][1], s[2]),
            ip(-m[2][0], s[4], m[2][1], s[2], -m[2][3], s[0]),
        ],
        [
            ip(-m[1][0], c[3], m[1][1], c[1], -m[1][2], c[0]),
            ip(m[0][0], c[3], m[0][2], c[0], -m[0][1], c[1]),
            ip(-m[3][0], s[3], m[3][1], s[1], -m[3][2], s[0]),
            ip(m[2][0], s[3], m[2][2], s[0], -m[2][1], s[1]),
        ],
    ])
}
