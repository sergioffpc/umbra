//! `Transform::apply`/`apply_inverse` for every transformable type against
//! `crate::reference::transform`.

use crate::reference::transform as reference;
use umbra_math::{
    Interval, Normal3f, Point3f, Point3fi, SquareMatrix, Transform, Vector3f, Vector3fi, look_at,
    perspective, rotate, scale, translate,
};

use super::{COUNT, finite_samples, intervals, same, same_interval};

/// Whether every element of both matrices is finite.
fn finite(t: &Transform) -> bool {
    [t.matrix(), t.inverse_matrix()]
        .iter()
        .all(|m| m.rows().iter().flatten().all(|v| v.is_finite()))
}

/// Transforms of every kind: rigid, scaled, projective, and arbitrary
/// matrices with their inverses; the identity where a matrix is singular.
fn transforms(seed: u64, n: usize) -> Vec<Transform> {
    let s = finite_samples(seed, n * 16);
    s.as_chunks::<16>()
        .0
        .iter()
        .enumerate()
        .map(|(k, c)| match k % 5 {
            0 => {
                translate(Vector3f::new(c[0], c[1], c[2]))
                    * rotate(c[3], Vector3f::new(c[4], c[5], c[6] + 0.5))
            }
            1 => scale(c[0] + 1.0, c[1] - 1.0, c[2].rem_euclid(3.0) + 0.5),
            2 => perspective(
                c[0].rem_euclid(120.0) + 10.0,
                c[1].abs() + 0.01,
                c[2].abs() + 100.0,
            ),
            3 => look_at(
                Point3f::new(c[0], c[1], c[2]),
                Point3f::new(c[3], c[4], c[5]),
                Vector3f::new(c[6], c[7] + 1.0, c[8]),
            )
            .unwrap_or_default(),
            _ => Transform::from(SquareMatrix::new([
                [c[0], c[1], c[2], c[3]],
                [c[4], c[5], c[6], c[7]],
                [c[8], c[9], c[10], c[11]],
                [c[12], c[13], c[14], c[15]],
            ])),
        })
        .map(|t| if finite(&t) { t } else { Transform::default() })
        .collect()
}

/// The `w` of the inverse applied to `p`, as `apply_inverse` computes it:
/// zero where the point lies on the plane that the inverse projects to
/// infinity, which `apply_inverse` rejects with a debug assertion as
/// pbrt-v4's `DCHECK` does.
fn inverse_w(t: &Transform, p: Point3f) -> f32 {
    let m = t.inverse_matrix().rows();
    (m[3][0] * p.x + m[3][1] * p.y) + (m[3][2] * p.z + m[3][3])
}

fn same_point(a: Point3f, b: Point3f) -> bool {
    same(a.x, b.x) && same(a.y, b.y) && same(a.z, b.z)
}

fn same_vector(a: Vector3f, b: Vector3f) -> bool {
    same(a.x, b.x) && same(a.y, b.y) && same(a.z, b.z)
}

fn same_normal(a: Normal3f, b: Normal3f) -> bool {
    same(a.x, b.x) && same(a.y, b.y) && same(a.z, b.z)
}

/// Whether both bounds are finite.
fn finite_interval(i: Interval) -> bool {
    i.lower_bound().is_finite() && i.upper_bound().is_finite()
}

/// The same intervals, wherever the reference's bound is finite. A bound that
/// overflowed went through `∞ − ∞ = NaN` and then a ULP step of the NaN's
/// payload, which is not fixed (Miri draws it at random, and the step can
/// land on 0 or ∞): outside the contract, as an `Interval` with a NaN bound.
fn same_bounded(a: Interval, b: Interval) -> bool {
    !finite_interval(b) || same_interval(a, b)
}

fn same_point3fi(a: Point3fi, b: Point3fi) -> bool {
    same_bounded(a.x, b.x) && same_bounded(a.y, b.y) && same_bounded(a.z, b.z)
}

fn same_vector3fi(a: Vector3fi, b: Vector3fi) -> bool {
    same_bounded(a.x, b.x) && same_bounded(a.y, b.y) && same_bounded(a.z, b.z)
}

#[test]
fn points_vectors_and_normals_match() {
    let s = finite_samples(67, COUNT * 3 / 4);
    for (t, c) in transforms(71, COUNT / 4).iter().zip(s.as_chunks::<3>().0) {
        let p = Point3f::new(c[0], c[1], c[2]);
        let v = Vector3f::new(c[0], c[1], c[2]);
        let n = Normal3f::new(c[0], c[1], c[2]);
        assert!(
            same_point(t.apply(p), reference::apply_point(t, p)),
            "apply({t:?}, {p:?})"
        );
        if inverse_w(t, p) != 0.0 {
            assert!(
                same_point(t.apply_inverse(p), reference::apply_inverse_point(t, p)),
                "apply_inverse({t:?}, {p:?})"
            );
        }
        assert!(
            same_vector(t.apply(v), reference::apply_vector(t, v)),
            "apply({t:?}, {v:?})"
        );
        assert!(
            same_vector(t.apply_inverse(v), reference::apply_inverse_vector(t, v)),
            "apply_inverse({t:?}, {v:?})"
        );
        assert!(
            same_normal(t.apply(n), reference::apply_normal(t, n)),
            "apply({t:?}, {n:?})"
        );
        assert!(
            same_normal(t.apply_inverse(n), reference::apply_inverse_normal(t, n)),
            "apply_inverse({t:?}, {n:?})"
        );
    }
}

#[test]
fn error_bounded_points_and_vectors_match() {
    let i = intervals(73, COUNT * 3 / 4);
    let s = finite_samples(79, COUNT * 3 / 4);
    let ts = transforms(83, COUNT / 4);
    for ((t, c), f) in ts
        .iter()
        .zip(i.as_chunks::<3>().0)
        .zip(s.as_chunks::<3>().0)
    {
        // An exact point, a point with error, and the same as vectors.
        let exact = Point3fi::from(Point3f::new(f[0], f[1], f[2]));
        let bounded = Point3fi::new(c[0], c[1], c[2]);
        for p in [exact, bounded] {
            let (opt, reference) = (t.apply(p), reference::apply_point3fi(t, p));
            assert!(
                same_point3fi(opt, reference),
                "apply({t:?}, {p:?}): {opt:?} vs {reference:?}"
            );
            let (opt, reference) = (t.apply_inverse(p), reference::apply_inverse_point3fi(t, p));
            assert!(
                same_point3fi(opt, reference),
                "apply_inverse({t:?}, {p:?}): {opt:?} vs {reference:?}"
            );
            let v = Vector3fi::from(p);
            let (opt, reference) = (t.apply(v), reference::apply_vector3fi(t, v));
            assert!(
                same_vector3fi(opt, reference),
                "apply({t:?}, {v:?}): {opt:?} vs {reference:?}"
            );
            let (opt, reference) = (t.apply_inverse(v), reference::apply_inverse_vector3fi(t, v));
            assert!(
                same_vector3fi(opt, reference),
                "apply_inverse({t:?}, {v:?}): {opt:?} vs {reference:?}"
            );
        }
    }
}
