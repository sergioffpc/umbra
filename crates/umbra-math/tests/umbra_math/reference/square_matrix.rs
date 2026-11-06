//! The reference of `umbra_math`'s `square_matrix` module: pbrt-v4's `SquareMatrix` product,
//! determinant and inverse, one compensated `InnerProduct` per element over
//! [`crate::reference::math`].

use crate::reference::math::{difference_of_products, inner_product};
use umbra_math::SquareMatrix;

/// pbrt-v4 `operator*(SquareMatrix<3>, SquareMatrix<3>)`.
#[must_use]
pub(crate) fn mul3(a: &SquareMatrix<3>, b: &SquareMatrix<3>) -> SquareMatrix<3> {
    let (a, b) = (a.rows(), b.rows());
    SquareMatrix::new(core::array::from_fn(|i| {
        core::array::from_fn(|j| {
            inner_product([(a[i][0], b[0][j]), (a[i][1], b[1][j]), (a[i][2], b[2][j])])
        })
    }))
}

/// pbrt-v4 `operator*(SquareMatrix<4>, SquareMatrix<4>)`.
#[must_use]
pub(crate) fn mul4(a: &SquareMatrix<4>, b: &SquareMatrix<4>) -> SquareMatrix<4> {
    let (a, b) = (a.rows(), b.rows());
    SquareMatrix::new(core::array::from_fn(|i| {
        core::array::from_fn(|j| {
            inner_product([
                (a[i][0], b[0][j]),
                (a[i][1], b[1][j]),
                (a[i][2], b[2][j]),
                (a[i][3], b[3][j]),
            ])
        })
    }))
}

/// The 2 × 2 minors of the top two rows (`s`) and the bottom two rows (`c`)
/// that the 4 × 4 determinant and inverse share.
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

/// pbrt-v4 `Determinant(SquareMatrix<4>)`.
#[must_use]
pub(crate) fn determinant4(m: &SquareMatrix<4>) -> f32 {
    let Minors4 { s, c } = Minors4::new(m.rows());
    difference_of_products(s[0], c[5], s[1], c[4])
        + difference_of_products(s[2], c[3], -s[3], c[2])
        + difference_of_products(s[5], c[0], s[4], c[1])
}

/// pbrt-v4 `Inverse(SquareMatrix<4>)`: `None` for a singular matrix.
#[must_use]
pub(crate) fn inverse4(m: &SquareMatrix<4>) -> Option<SquareMatrix<4>> {
    let Minors4 { s, c } = Minors4::new(m.rows());
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
    Some(scaled_adjugate4(m.rows(), &s, &c, 1.0 / det))
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
