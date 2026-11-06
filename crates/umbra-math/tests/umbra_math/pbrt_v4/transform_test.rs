//! pbrt-v4 `src/pbrt/util/transform_test.cpp`, and the transforms its tests
//! leave out.

use crate::rng::Rng;
use crate::sampling::sample_uniform_sphere;
use umbra_math::{
    Normal3f, Point2f, Point3f, Point3fi, SquareMatrix, Transform, Vector3f, dot, look_at,
    perspective, rotate, rotate_from_to, rotate_x, scale, translate,
};

/// `TEST(RotateFromTo, Simple)`.
#[test]
fn rotate_from_to_simple() {
    for to in [
        Vector3f::new(0.0, 0.0, 1.0),
        Vector3f::new(1.0, 0.0, 0.0),
        Vector3f::new(0.0, 1.0, 0.0),
    ] {
        let from = Vector3f::new(0.0, 0.0, 1.0);
        assert_eq!(to, rotate_from_to(from, to).apply(from));
    }
}

/// `TEST(RotateFromTo, Randoms)`.
#[test]
fn rotate_from_to_randoms() {
    let mut rng = Rng::default();
    for _ in 0..100 {
        let from = sample_uniform_sphere(Point2f::new(rng.uniform_f32(), rng.uniform_f32()));
        let to = sample_uniform_sphere(Point2f::new(rng.uniform_f32(), rng.uniform_f32()));
        let to_new = rotate_from_to(from, to).apply(from);
        assert!((to_new.length() - 1.0).abs() < 1e-3, "{to_new:?}");
        assert!(dot(to, to_new) > 0.999, "{to:?} vs {to_new:?}");
    }
}

fn assert_near(a: Point3f, b: Point3f) {
    assert!((a - b).length() < 1e-5, "{a:?} vs {b:?}");
}

/// Not in pbrt-v4's tests: `LookAt` maps the eye to the origin and the look
/// point onto `+z`, and keeps `up` in the `yz` plane.
#[test]
fn look_at_camera_space() {
    let (pos, look) = (Point3f::new(1.0, 2.0, 3.0), Point3f::new(1.0, 2.0, -7.0));
    let camera_from_world = look_at(pos, look, Vector3f::new(0.0, 1.0, 0.0)).unwrap();
    assert_near(Point3f::new(0.0, 0.0, 0.0), camera_from_world.apply(pos));
    assert_near(Point3f::new(0.0, 0.0, 10.0), camera_from_world.apply(look));
    let up = camera_from_world.apply(Vector3f::new(0.0, 1.0, 0.0));
    assert!(
        (up - Vector3f::new(0.0, 1.0, 0.0)).length() < 1e-6,
        "{up:?}"
    );
    assert!(
        !camera_from_world.swaps_handedness(),
        "a rotation and a translation"
    );
    // `up` along the viewing direction has no frame.
    assert!(look_at(pos, look, Vector3f::new(0.0, 0.0, 1.0)).is_none());
}

/// Not in pbrt-v4's tests: `Perspective` maps the near and far planes to
/// `z = 0` and `z = 1`, and the edge of the field of view to `x = ±1`.
#[test]
fn perspective_projection() {
    let (near, far) = (1.0, 100.0);
    let p = perspective(90.0, near, far);
    assert_near(
        Point3f::new(0.0, 0.0, 0.0),
        p.apply(Point3f::new(0.0, 0.0, near)),
    );
    assert_near(
        Point3f::new(0.0, 0.0, 1.0),
        p.apply(Point3f::new(0.0, 0.0, far)),
    );
    // tan(45°) = 1, so x = z is the edge of the field of view.
    assert_near(
        Point3f::new(1.0, -1.0, 1.0),
        p.apply(Point3f::new(far, -far, far)),
    );
    let back = p.apply_inverse(p.apply(Point3f::new(0.5, 0.25, 10.0)));
    assert!(
        (back - Point3f::new(0.5, 0.25, 10.0)).length() < 1e-4,
        "{back:?}"
    );
}

/// Not in pbrt-v4's tests: composition, inverses, and the transform rules of
/// points, vectors and normals.
#[test]
fn transform_composition() {
    let t = translate(Vector3f::new(1.0, -2.0, 3.0)) * scale(2.0, 2.0, 2.0) * rotate_x(90.0);
    let p = Point3f::new(1.0, 1.0, 1.0);
    assert_near(Point3f::new(3.0, -4.0, 5.0), t.apply(p));
    assert_near(p, t.apply_inverse(t.apply(p)));
    assert_near(p, t.inverse().apply(t.apply(p)));
    // Vectors ignore the translation.
    let v = t.apply(Vector3f::new(0.0, 1.0, 0.0));
    assert!((v - Vector3f::new(0.0, 0.0, 2.0)).length() < 1e-6, "{v:?}");
    // Normals stay perpendicular to transformed tangents under a skew scale.
    let s = scale(1.0, 4.0, 1.0) * rotate(30.0, Vector3f::new(1.0, 1.0, 0.0));
    let (tangent, n) = (Vector3f::new(1.0, -1.0, 0.0), Normal3f::new(1.0, 1.0, 0.0));
    assert!(dot(s.apply(n), s.apply(tangent)).abs() < 1e-5);
    assert!(Transform::default().is_identity());
    assert!(t.has_scale(1e-3) && !rotate_x(30.0).has_scale(1e-3));
    assert!(!t.swaps_handedness() && scale(-1.0, 1.0, 1.0).swaps_handedness());
}

/// Not in pbrt-v4's tests: a transformed `Point3fi` bounds the exact result
/// of transforming its centre, for exact and inexact inputs, both ways.
#[test]
fn transform_point3fi_error_bounds() {
    let mut rng = Rng::default();
    for _ in 0..10_000 {
        let mut c = || -100.0 + 200.0 * rng.uniform_f32();
        let t = translate(Vector3f::new(c(), c(), c())) * rotate(c(), Vector3f::new(c(), c(), c()));
        let p = Point3f::new(c(), c(), c());
        let inexact = Point3fi::from_value_and_error(p, Vector3f::new(1e-3, 1e-3, 1e-3).into());
        for pi in [Point3fi::from(p), inexact] {
            check_bound(t.matrix(), p, t.apply(pi));
            check_bound(t.inverse_matrix(), p, t.apply_inverse(pi));
        }
    }
}

/// `m · p` in `f64` lies inside `pi` for each coordinate.
fn check_bound(m: &SquareMatrix<4>, p: Point3f, pi: Point3fi) {
    let pd = [p.x, p.y, p.z, 1.0].map(f64::from);
    let exact = |row: usize| (0..4).map(|j| f64::from(m[row][j]) * pd[j]).sum::<f64>();
    for (i, c) in [pi.x, pi.y, pi.z].into_iter().enumerate() {
        let e = exact(i);
        assert!(
            f64::from(c.lower_bound()) <= e && e <= f64::from(c.upper_bound()),
            "{e} outside {c:?}"
        );
    }
}
