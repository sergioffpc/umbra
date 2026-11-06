//! pbrt-v4 `src/pbrt/util/vecmath_test.cpp`, for the types `umbra-math` owns
//! (the `Bounds` and `DirectionCone` tests belong to `umbra-geometry`).

use crate::rng::Rng;
use crate::sampling::{hammersley_2d, sample_uniform_sphere};
use umbra_math::{
    Frame, Interval, Normal3f, OctahedralVector, PI, Point2f, Point3f, Point3fi, Vector2f,
    Vector2i, Vector3, Vector3f, Vector3fi, Vector3i, abs_dot, angle_between, coordinate_system,
    cross, distance, distance_squared, dot, safe_acos, sqr,
};
use umbra_math::{rotate, spherical_triangle_area};

/// `TEST(Vector2, Basics)`, conversion and arithmetic.
#[test]
fn vector2_basics() {
    let vf = Vector2f::new(-1.0, 10.0);
    assert_eq!(vf, Vector2f::from(Vector2i::new(-1, 10)));
    assert_ne!(vf, Vector2f::new(-1.0, 100.0));
    assert_eq!(Vector2f::new(-2.0, 20.0), vf + vf);
    assert_eq!(Vector2f::new(0.0, 0.0), vf - vf);
    assert_eq!(Vector2f::new(-2.0, 20.0), vf * 2.0);
    assert_eq!(Vector2f::new(-2.0, 20.0), 2.0 * vf);
    assert_eq!(Vector2f::new(-0.5, 5.0), vf / 2.0);
}

/// `TEST(Vector2, Basics)`, the component-wise functions.
#[test]
fn vector2_basics_component_wise() {
    let vf = Vector2f::new(-1.0, 10.0);
    assert_eq!(Vector2f::new(1.0, 10.0), vf.abs());
    assert_eq!(vf, Vector2f::new(-1.5, 9.9).ceil());
    assert_eq!(vf, Vector2f::new(-0.5, 10.01).floor());
    assert_eq!(
        Vector2f::new(-20.0, 10.0),
        vf.min(Vector2f::new(-20.0, 20.0))
    );
    assert_eq!(
        Vector2f::new(-1.0, 20.0),
        vf.max(Vector2f::new(-20.0, 20.0))
    );
}

/// `TEST(Vector2, Basics)`, the component queries.
#[test]
fn vector2_basics_components() {
    let vf = Vector2f::new(-1.0, 10.0);
    assert_eq!(-1.0, vf.min_component_value());
    assert_eq!(-10.0, (-vf).min_component_value());
    assert_eq!(10.0, vf.max_component_value());
    assert_eq!(1.0, (-vf).max_component_value());
    assert_eq!(1, vf.max_component_index());
    assert_eq!(0, (-vf).max_component_index());
    assert_eq!(0, vf.min_component_index());
    assert_eq!(1, (-vf).min_component_index());
}

/// `TEST(Vector2, Basics)`, `Permute`.
#[test]
fn vector2_basics_permute() {
    let vf = Vector2f::new(-1.0, 10.0);
    assert_eq!(vf, vf.permute([0, 1]));
    assert_eq!(Vector2f::new(10.0, -1.0), vf.permute([1, 0]));
    assert_eq!(Vector2f::new(10.0, 10.0), vf.permute([1, 1]));
}

/// `TEST(Vector3, Basics)`, conversion and arithmetic.
#[test]
fn vector3_basics() {
    let vf = Vector3f::new(-1.0, 10.0, 2.0);
    assert_eq!(vf, Vector3f::from(Vector3i::new(-1, 10, 2)));
    assert_ne!(vf, Vector3f::new(-1.0, 100.0, 2.0));
    assert_eq!(Vector3f::new(-2.0, 20.0, 4.0), vf + vf);
    assert_eq!(Vector3f::new(0.0, 0.0, 0.0), vf - vf);
    assert_eq!(Vector3f::new(-2.0, 20.0, 4.0), vf * 2.0);
    assert_eq!(Vector3f::new(-2.0, 20.0, 4.0), 2.0 * vf);
    assert_eq!(Vector3f::new(-0.5, 5.0, 1.0), vf / 2.0);
}

/// `TEST(Vector3, Basics)`, the component-wise functions.
#[test]
fn vector3_basics_component_wise() {
    let vf = Vector3f::new(-1.0, 10.0, 2.0);
    assert_eq!(Vector3f::new(1.0, 10.0, 2.0), vf.abs());
    assert_eq!(vf, Vector3f::new(-1.5, 9.9, 1.01).ceil());
    assert_eq!(vf, Vector3f::new(-0.5, 10.01, 2.99).floor());
    let other = Vector3f::new(-20.0, 20.0, 1.5);
    assert_eq!(Vector3f::new(-20.0, 10.0, 1.5), vf.min(other));
    let other = Vector3f::new(-20.0, 20.0, 0.0);
    assert_eq!(Vector3f::new(-1.0, 20.0, 2.0), vf.max(other));
}

/// `TEST(Vector3, Basics)`, the component queries.
#[test]
fn vector3_basics_components() {
    let vf = Vector3f::new(-1.0, 10.0, 2.0);
    assert_eq!(-1.0, vf.min_component_value());
    assert_eq!(-10.0, (-vf).min_component_value());
    assert_eq!(10.0, vf.max_component_value());
    assert_eq!(1.0, (-vf).max_component_value());
    assert_eq!(1, vf.max_component_index());
    assert_eq!(0, (-vf).max_component_index());
    assert_eq!(0, vf.min_component_index());
    assert_eq!(1, (-vf).min_component_index());
}

/// `TEST(Vector3, Basics)`, `Permute`.
#[test]
fn vector3_basics_permute() {
    let vf = Vector3f::new(-1.0, 10.0, 2.0);
    assert_eq!(vf, vf.permute([0, 1, 2]));
    assert_eq!(Vector3f::new(10.0, -1.0, 2.0), vf.permute([1, 0, 2]));
    assert_eq!(Vector3f::new(2.0, -1.0, 10.0), vf.permute([2, 0, 1]));
    assert_eq!(Vector3f::new(10.0, 10.0, -1.0), vf.permute([1, 1, 0]));
}

/// `acos` of the dot product of `a` and `b`, normalised in `f64`: the
/// reference `AngleBetween` is measured against.
fn precise_angle(a: Vector3f, b: Vector3f) -> f64 {
    let ad = [a.x, a.y, a.z].map(f64::from);
    let bd = [b.x, b.y, b.z].map(f64::from);
    let len = |v: [f64; 3]| libm::sqrt(v.iter().map(|c| c * c).sum());
    let d: f64 = ad.iter().zip(&bd).map(|(x, y)| x * y).sum::<f64>() / (len(ad) * len(bd));
    libm::acos(d.clamp(-1.0, 1.0))
}

fn random_direction(rng: &mut Rng) -> Vector3f {
    let mut c = || -1.0 + 2.0 * rng.uniform_f32();
    Vector3f::new(c(), c(), c()).normalize()
}

/// The relative error of `angle_between(a, b)` against `precise_angle`.
fn angle_error(a: Vector3f, b: Vector3f) -> f64 {
    let precise = precise_angle(a, b);
    #[expect(clippy::cast_possible_truncation, reason = "pbrt-v4 compares in Float")]
    let precise = f64::from(precise as f32);
    (precise - f64::from(angle_between(a, b))).abs() / precise
}

/// `TEST(Vector, AngleBetween)`, the special cases.
#[test]
fn vector_angle_between() {
    assert_eq!(
        0.0,
        angle_between(Vector3f::new(1.0, 0.0, 0.0), Vector3f::new(1.0, 0.0, 0.0))
    );
    let opposite = angle_between(Vector3f::new(0.0, 0.0, 1.0), Vector3f::new(0.0, 0.0, -1.0));
    assert!(opposite.abs() - PI < 1e-7);
    let right = angle_between(Vector3f::new(1.0, 0.0, 0.0), Vector3f::new(0.0, 1.0, 0.0));
    assert!(right.abs() - PI / 2.0 < 1e-7);

    let x = Vector3f::new(1.0, -3.0, 10.0).normalize();
    assert_eq!(0.0, angle_between(x, x));
    assert!((angle_between(x, -x) - PI).abs() < 3e-7);

    // Nearly opposite: more accurate than the naive `acos` of the dot product.
    let a = Vector3f::new(1.0, 1.0, 1.0).normalize();
    let b = Vector3f::new(-1.0, -1.0001, -1.0).normalize();
    // pbrt-v4 normalises in `long double`; `f64` is close enough here.
    #[expect(clippy::cast_possible_truncation, reason = "pbrt-v4 compares in Float")]
    let precise = libm::acos(-3.0001 / libm::sqrt(3.0 * (2.0 + 1.0001 * 1.0001))) as f32;
    assert_eq!(
        angle_between(a, b),
        precise,
        "naive {}",
        safe_acos(dot(a, b))
    );
}

/// `TEST(Vector, AngleBetween)`, the random loops.
#[test]
fn vector_angle_between_randoms() {
    for i in 0..100_000 {
        let mut rng = Rng::new(i);
        let (a, b) = (random_direction(&mut rng), random_direction(&mut rng));
        assert!(angle_error(a, b) < 5e-6, "{a:?} {b:?}");
    }
    for i in 0..100_000 {
        let mut rng = Rng::new(i + 10_000_000);
        let (a, b) = (random_direction(&mut rng), random_direction(&mut rng));
        // Facing in opposite-ish directions.
        let a = Vector3f::new(a.x.copysign(-b.x), a.y.copysign(-b.y), a.z.copysign(-b.z));
        assert!(angle_error(a, b) < 5e-6, "{a:?} {b:?}");
    }
}

/// Duff et al. 2017, footnote 1: how far `a, b, c` are from orthonormal.
fn frame_error(a: Vector3f, b: Vector3f, c: Vector3f) -> f32 {
    (sqr(a.length() - 1.0)
        + sqr(b.length() - 1.0)
        + sqr(c.length() - 1.0)
        + sqr(dot(a, b))
        + sqr(dot(b, c))
        + sqr(dot(c, a)))
        / 6.0
}

/// `TEST(Vector, CoordinateSystem)`.
#[test]
fn vector_coordinate_system() {
    for v in [
        Vector3f::new(1.0, 0.0, 0.0),
        Vector3f::new(-1.0, 0.0, 0.0),
        Vector3f::new(0.0, 1.0, 0.0),
        Vector3f::new(0.0, -1.0, 0.0),
        Vector3f::new(0.0, 0.0, 1.0),
        Vector3f::new(0.0, 0.0, -1.0),
    ] {
        let (a, b) = coordinate_system(v);
        let axis = (0..3).find(|&c| v[c] != 0.0).unwrap();
        assert_eq!((0.0, 0.0), (a[axis], b[axis]), "{v:?}");
    }

    // Bad vectors from Duff et al.
    for v in [
        Vector3f::new(0.000_385_273_16, 0.000_384_600_16, -0.999_999_9),
        Vector3f::new(-0.000_198_135_81, -0.000_089_468_39, -0.999_999_9),
    ] {
        let (a, b) = coordinate_system(v);
        assert!(frame_error(v, a, b) < 1e-10, "{v:?}");
    }

    let mut rng = Rng::default();
    for _ in 0..1000 {
        let v = sample_uniform_sphere(Point2f::new(rng.uniform_f32(), rng.uniform_f32()));
        let (a, b) = coordinate_system(v);
        assert!(frame_error(v, a, b) < 1e-10, "{v:?}");
    }
}

/// `TEST(PointVector, Interval)`: the interval variants compose.
#[test]
fn point_vector_interval() {
    let mut p = Point3fi::from(Point3f::new(1.0, 2.0, 3.0));
    let q = Point3fi::from(Point3f::new(6.0, 9.0, 1.0));
    let v = Vector3fi::from(Vector3f::new(4.0, 5.0, 6.0));

    p += v;
    assert_eq!(Point3f::new(1.0, 2.0, 3.0), Point3f::from(p - v));
    p -= v;
    let scaled: Vector3<Interval> = Interval::from(4.0) * v;
    p += scaled;
    assert_eq!(Point3f::new(17.0, 22.0, 27.0), Point3f::from(p));

    let d = dot(v, v);
    assert!(d.contains(77.0), "{d:?}");
    let d = distance_squared(p, q);
    assert!(d.contains(121.0 + 169.0 + 676.0), "{d:?}");
    let d = distance(p, q);
    assert!(d.contains(libm::sqrtf(966.0)), "{d:?}");
    let vv = cross(v, v);
    assert!(
        vv.x.contains(0.0) && vv.y.contains(0.0) && vv.z.contains(0.0),
        "{vv:?}"
    );
}

/// `TEST(OctahedralVector, EncodeDecode)`.
#[test]
fn octahedral_vector_encode_decode() {
    for u in hammersley_2d(65535) {
        let v = sample_uniform_sphere(u);
        let v2 = Vector3f::from(OctahedralVector::from(v));
        assert!(
            v2.length() > 0.999 && v2.length() < 1.001,
            "{v:?} -> {v2:?}"
        );
        assert!((1.0 - dot(v2, v)).abs() < 0.001, "{v:?} -> {v2:?}");
    }
}

/// Not in pbrt-v4's tests: `Frame` round trips vectors and normals, and its
/// axes are what its constructors promise.
#[test]
fn frame_round_trip() {
    let mut rng = Rng::default();
    for _ in 0..1000 {
        let z = sample_uniform_sphere(Point2f::new(rng.uniform_f32(), rng.uniform_f32()));
        let w = sample_uniform_sphere(Point2f::new(rng.uniform_f32(), rng.uniform_f32()));
        let frame = Frame::from_z(z);
        assert_eq!(z, frame.z);
        assert!(frame_error(frame.x, frame.y, frame.z) < 1e-10, "{frame:?}");
        let local = frame.to_local(w);
        assert!((local.z - dot(w, z)).abs() < 1e-6, "{local:?}");
        assert!((frame.from_local(local) - w).length() < 1e-5, "{w:?}");
        let n = Normal3f::from(w);
        assert!(
            (frame.from_local(frame.to_local(n)) - n).length() < 1e-5,
            "{n:?}"
        );
    }
    let frame = Frame::from_xz(Vector3f::new(1.0, 0.0, 0.0), Vector3f::new(0.0, 0.0, 1.0));
    assert_eq!(Vector3f::new(0.0, 1.0, 0.0), frame.y);
    assert_eq!(1.0, abs_dot(Normal3f::new(0.0, 0.0, -1.0), frame.z));
}

/// `true` if `a` is within pbrt-v4's tolerance of `expected`.
fn area_near(a: f32, expected: f32, high: f32) -> bool {
    a >= 0.9999 * expected && a <= high * expected
}

/// `TEST(SphericalTriangleArea, Basics)`, the fixed octants.
#[test]
fn spherical_triangle_area_basics() {
    let (x, y, z) = (
        Vector3f::new(1.0, 0.0, 0.0),
        Vector3f::new(0.0, 1.0, 0.0),
        Vector3f::new(0.0, 0.0, 1.0),
    );
    let a = spherical_triangle_area(x, y, z);
    assert!(area_near(a, PI / 2.0, 1.00001), "{a}");
    let a = spherical_triangle_area(x, Vector3f::new(1.0, 1.0, 0.0).normalize(), z);
    assert!(area_near(a, PI / 4.0, 1.00001), "{a}");
}

/// `TEST(SphericalTriangleArea, Basics)`, the random rotations. pbrt-v4 passes
/// `2π·u` as degrees to `Rotate`; the angle is just random, so that is kept.
#[test]
fn spherical_triangle_area_basics_rotations() {
    let mut rng = Rng::default();
    let corners = [
        (Vector3f::new(0.0, 1.0, 0.0), PI / 2.0),
        (Vector3f::new(1.0, 1.0, 0.0).normalize(), PI / 4.0),
    ];
    for (b, expected) in corners {
        for _ in 0..100 {
            let axis = sample_uniform_sphere(Point2f::new(rng.uniform_f32(), rng.uniform_f32()));
            let t = rotate(2.0 * PI * rng.uniform_f32(), axis);
            let [va, vb, vc] = [
                Vector3f::new(1.0, 0.0, 0.0),
                b,
                Vector3f::new(0.0, 0.0, 1.0),
            ]
            .map(|v| t.apply(v));
            let a = spherical_triangle_area(va, vb, vc);
            assert!(area_near(a, expected, 1.0001), "{a}");
        }
    }
}
