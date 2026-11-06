//! `SquareMatrix` products, determinants and inverses against
//! `crate::reference::square_matrix`.

use crate::reference::square_matrix as reference;
use umbra_math::SquareMatrix;

use super::{COUNT, finite_samples, same};

/// Whether every element is [`same`].
fn same_matrix<const N: usize>(a: &SquareMatrix<N>, b: &SquareMatrix<N>) -> bool {
    a.rows()
        .iter()
        .zip(b.rows())
        .all(|(x, y)| x.iter().zip(y).all(|(&p, &q)| same(p, q)))
}

/// Consecutive `N × N` matrices from the samples; every fourth is close to
/// orthonormal, as transforms are.
fn matrices<const N: usize>(seed: u64, n: usize) -> Vec<SquareMatrix<N>> {
    let s = finite_samples(seed, n * N * N);
    s.chunks_exact(N * N)
        .enumerate()
        .map(|(k, chunk)| {
            SquareMatrix::new(core::array::from_fn(|i| {
                core::array::from_fn(|j| element(k, i, j, chunk[i * N + j]))
            }))
        })
        .collect()
}

/// Element `(i, j)` of matrix `k`: near the identity for every fourth
/// matrix, the sample itself otherwise.
fn element(k: usize, i: usize, j: usize, sample: f32) -> f32 {
    if k.is_multiple_of(4) {
        f32::from(i == j) + sample.rem_euclid(0.25)
    } else {
        sample
    }
}

#[test]
fn products_match() {
    let m3 = matrices::<3>(53, COUNT / 10);
    for &[a, b] in m3.as_chunks::<2>().0 {
        let (opt, reference) = (a * b, reference::mul3(&a, &b));
        assert!(same_matrix(&opt, &reference), "{a:?} * {b:?}");
    }
    let m4 = matrices::<4>(59, COUNT / 10);
    for &[a, b] in m4.as_chunks::<2>().0 {
        let (opt, reference) = (a * b, reference::mul4(&a, &b));
        assert!(same_matrix(&opt, &reference), "{a:?} * {b:?}");
    }
}

#[test]
fn determinants_and_inverses_match() {
    for m in matrices::<4>(61, COUNT / 5) {
        assert!(
            same(m.determinant(), reference::determinant4(&m)),
            "determinant of {m:?}"
        );
        match (m.inverse(), reference::inverse4(&m)) {
            (Some(opt), Some(reference)) => {
                assert!(same_matrix(&opt, &reference), "inverse of {m:?}");
            }
            (None, None) => {}
            (opt, reference) => panic!("inverse of {m:?}: {opt:?} vs {reference:?}"),
        }
    }
}
