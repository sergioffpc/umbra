//! pbrt-v4 `src/pbrt/util/math_test.cpp`, the `SquareMatrix` tests.

use crate::rng::Rng;
use umbra_math::{SquareMatrix, Vector2f, Vector3f};

/// `TEST(SquareMatrix, Basics2)`.
#[test]
fn square_matrix_basics2() {
    let m2 = SquareMatrix::<2>::default();
    assert!(m2.is_identity());
    assert_eq!(m2, SquareMatrix::new([[1.0, 0.0], [0.0, 1.0]]));
    assert_ne!(m2, SquareMatrix::new([[0.0, 1.0], [1.0, 0.0]]));
    assert_eq!(
        SquareMatrix::new([[2.0, 0.0], [0.0, -1.0]]),
        SquareMatrix::diag([2.0, -1.0])
    );

    let m = SquareMatrix::new([[1.0, 2.0], [3.0, 4.0]]);
    assert!(!m.is_identity());
    assert_eq!(m.transpose(), SquareMatrix::new([[1.0, 3.0], [2.0, 4.0]]));

    assert_eq!([1.0 - 2.0 * 2.0, 3.0 - 4.0 * 2.0], m * [1.0, -2.0]);
    assert_eq!(
        Vector2f::new(1.0 - 2.0 * 2.0, 3.0 - 4.0 * 2.0),
        m * Vector2f::new(1.0, -2.0)
    );
}

/// `TEST(SquareMatrix, Basics2)`, `Inverse`.
#[test]
fn square_matrix_basics2_inverse() {
    let m2 = SquareMatrix::<2>::default();
    assert_eq!(Some(m2), m2.inverse());
    let ms = SquareMatrix::new([[2.0, 4.0], [-4.0, 8.0]]);
    let expected = SquareMatrix::new([[1.0 / 4.0, -1.0 / 8.0], [1.0 / 8.0, 1.0 / 16.0]]);
    assert_eq!(Some(expected), ms.inverse());
    assert_eq!(None, SquareMatrix::new([[0.0, 0.0], [2.0, 0.0]]).inverse());
}

/// `TEST(SquareMatrix, Basics3)`.
#[test]
fn square_matrix_basics3() {
    let m3 = SquareMatrix::<3>::default();
    assert!(m3.is_identity());
    assert_eq!(
        m3,
        SquareMatrix::new([[1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 1.0]])
    );
    assert_ne!(
        m3,
        SquareMatrix::new([[0.0, 1.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 1.0]])
    );
    assert_eq!(
        SquareMatrix::new([[2.0, 0.0, 0.0], [0.0, -1.0, 0.0], [0.0, 0.0, 3.0]]),
        SquareMatrix::diag([2.0, -1.0, 3.0])
    );

    let m = SquareMatrix::new([[1.0, 2.0, 3.0], [4.0, 5.0, 6.0], [7.0, 8.0, 9.0]]);
    assert!(!m.is_identity());
    let mt = SquareMatrix::new([[1.0, 4.0, 7.0], [2.0, 5.0, 8.0], [3.0, 6.0, 9.0]]);
    assert_eq!(m.transpose(), mt);

    let expected = [1.0 - 4.0 + 12.0, 4.0 - 10.0 + 24.0, 7.0 - 16.0 + 36.0];
    assert_eq!(expected, m * [1.0, -2.0, 4.0]);
    assert_eq!(
        Vector3f::new(expected[0], expected[1], expected[2]),
        m * Vector3f::new(1.0, -2.0, 4.0)
    );
}

/// `TEST(SquareMatrix, Basics3)`, `Inverse`.
#[test]
fn square_matrix_basics3_inverse() {
    let m3 = SquareMatrix::<3>::default();
    assert_eq!(Some(m3), m3.inverse());
    let ms = SquareMatrix::new([[2.0, 0.0, 0.0], [0.0, 4.0, 0.0], [0.0, 0.0, -1.0]]);
    let inv = SquareMatrix::new([[0.5, 0.0, 0.0], [0.0, 0.25, 0.0], [0.0, 0.0, -1.0]]);
    assert_eq!(Some(inv), ms.inverse());
    let degen = SquareMatrix::new([[0.0, 0.0, 2.0], [0.0, 0.0, 0.0], [1.0, 1.0, 1.0]]);
    assert_eq!(None, degen.inverse());
}

/// `TEST(SquareMatrix, Basics4)`.
#[test]
fn square_matrix_basics4() {
    let m4 = SquareMatrix::<4>::default();
    assert!(m4.is_identity());
    assert_eq!(m4, SquareMatrix::diag([1.0, 1.0, 1.0, 1.0]));
    let not_identity = [
        [0.0, 0.0, 0.0, 0.0],
        [0.0, 1.0, 0.0, 0.0],
        [0.0, 0.0, 1.0, 0.0],
        [0.0, 0.0, 1.0, 0.0],
    ];
    assert_ne!(m4, SquareMatrix::new(not_identity));

    let diag = SquareMatrix::new([
        [8.0, 0.0, 0.0, 0.0],
        [0.0, 2.0, 0.0, 0.0],
        [0.0, 0.0, 1.0, 0.0],
        [0.0, 0.0, 0.0, 0.5],
    ]);
    assert_eq!(diag, SquareMatrix::diag([8.0, 2.0, 1.0, 0.5]));

    let m = SquareMatrix::new([
        [1.0, 2.0, 3.0, 4.0],
        [5.0, 6.0, 7.0, 8.0],
        [9.0, 10.0, 11.0, 12.0],
        [13.0, 14.0, 15.0, 16.0],
    ]);
    assert!(!m.is_identity());
    let mt = SquareMatrix::new([
        [1.0, 5.0, 9.0, 13.0],
        [2.0, 6.0, 10.0, 14.0],
        [3.0, 7.0, 11.0, 15.0],
        [4.0, 8.0, 12.0, 16.0],
    ]);
    assert_eq!(m.transpose(), mt);
}

/// `TEST(SquareMatrix, Basics4)`, `Inverse`.
#[test]
fn square_matrix_basics4_inverse() {
    let m4 = SquareMatrix::<4>::default();
    assert_eq!(Some(m4), m4.inverse());
    let diag = SquareMatrix::diag([8.0, 2.0, 1.0, 0.5]);
    assert_eq!(
        Some(SquareMatrix::diag([0.125, 0.5, 1.0, 2.0])),
        diag.inverse()
    );
    let degen = SquareMatrix::new([
        [2.0, 0.0, 0.0, 0.0],
        [0.0, 4.0, 0.0, 0.0],
        [0.0, -3.0, 0.0, 1.0],
        [0.0, 0.0, 0.0, 0.0],
    ]);
    assert_eq!(None, degen.inverse());
}

/// pbrt-v4 `randomMatrix`: entries uniform in `[−10, 10)`.
fn random_matrix<const N: usize>(rng: &mut Rng) -> SquareMatrix<N> {
    SquareMatrix::new(core::array::from_fn(|_| {
        core::array::from_fn(|_| -10.0 + 20.0 * rng.uniform_f32())
    }))
}

/// pbrt-v4's `equal` of `TEST(SquareMatrix, Inverse)`, relative away from 0.
#[expect(clippy::manual_midpoint, reason = "pbrt-v4's formula")]
fn nearly_equal(a: f32, b: f32) -> bool {
    let tol = 1e-4;
    if a.abs() < 1e-5 || b.abs() < 1e-5 {
        return a.abs() - b.abs() < tol;
    }
    (a.abs() - b.abs()) / ((a.abs() + b.abs()) / 2.0) < tol
}

/// Whether `id` is the identity to the tolerances of pbrt-v4's test.
fn is_nearly_identity<const N: usize>(id: SquareMatrix<N>) -> bool {
    let ok = |j: usize, k: usize| {
        if j == k {
            nearly_equal(id[j][k], 1.0)
        } else {
            id[j][k].abs() < 1e-4
        }
    };
    (0..N * N).all(|i| ok(i / N, i % N))
}

/// `TEST(SquareMatrix, Inverse)`, for one size: `m · m⁻¹ ≈ I` for random
/// matrices; returns how many were not invertible.
fn inverse_trials<const N: usize>(inverse: fn(&SquareMatrix<N>) -> Option<SquareMatrix<N>>) -> usize
where
    SquareMatrix<N>: core::ops::Mul<Output = SquareMatrix<N>>,
{
    (0..1000)
        .filter(|&i| {
            let mut rng = Rng::new(i);
            let m = random_matrix::<N>(&mut rng);
            inverse(&m).is_none_or(|inv| {
                assert!(is_nearly_identity(m * inv), "{m:?}, inverse {inv:?}");
                false
            })
        })
        .count()
}

/// `TEST(SquareMatrix, Inverse)`.
#[test]
fn square_matrix_inverse() {
    let failures = inverse_trials::<2>(SquareMatrix::<2>::inverse)
        + inverse_trials::<3>(SquareMatrix::<3>::inverse)
        + inverse_trials::<4>(SquareMatrix::<4>::inverse);
    assert!(failures < 3, "{failures} singular matrices");
}
