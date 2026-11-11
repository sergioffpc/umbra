//! `Sphere::intersect` on worked examples: each expected distance is
//! computed by hand from the ray and the sphere.

use umbra_geometry::{Ray, Sphere};
use umbra_math::{Point3f, Transform, Vector3f, rotate_z, translate};

/// A ray from `(0, 0, −5)` along `+z` meets the unit sphere at the origin
/// first at `z = −1`, four units away.
#[test]
fn ray_towards_the_centre_hits_the_near_side() {
    let sphere = Sphere::new(Transform::default(), 1.0);
    let ray = Ray::new(Point3f::new(0.0, 0.0, -5.0), Vector3f::new(0.0, 0.0, 1.0));

    let hit = sphere.intersect(&ray, f32::INFINITY).expect("the ray hits");

    assert!((hit.t_hit - 4.0).abs() < 1e-6, "t_hit = {}", hit.t_hit);
    assert_eq!(hit.p_obj, Point3f::new(0.0, 0.0, -1.0));
}

/// A ray parallel to the `z` axis two units off it passes the unit sphere.
#[test]
fn ray_off_the_sphere_misses() {
    let sphere = Sphere::new(Transform::default(), 1.0);
    let ray = Ray::new(Point3f::new(0.0, 2.0, -5.0), Vector3f::new(0.0, 0.0, 1.0));

    assert_eq!(sphere.intersect(&ray, f32::INFINITY), None);
}

/// From the centre, the only hit ahead is the far side, one radius away.
#[test]
fn ray_from_inside_hits_the_far_side() {
    let sphere = Sphere::new(Transform::default(), 1.0);
    let ray = Ray::new(Point3f::new(0.0, 0.0, 0.0), Vector3f::new(1.0, 0.0, 0.0));

    let hit = sphere.intersect(&ray, f32::INFINITY).expect("the ray hits");

    assert!((hit.t_hit - 1.0).abs() < 1e-6, "t_hit = {}", hit.t_hit);
    assert_eq!(hit.p_obj, Point3f::new(1.0, 0.0, 0.0));
}

/// The near side is four units away: a `t_max` of three stops short of it.
#[test]
fn hit_beyond_t_max_is_not_reported() {
    let sphere = Sphere::new(Transform::default(), 1.0);
    let ray = Ray::new(Point3f::new(0.0, 0.0, -5.0), Vector3f::new(0.0, 0.0, 1.0));

    assert_eq!(sphere.intersect(&ray, 3.0), None);
}

/// A sphere of radius 2 moved to `x = 10`: a ray from the origin along `+x`
/// meets it at `x = 8`; in object space that is `(−2, 0, 0)`.
#[test]
fn moved_sphere_is_hit_in_render_space() {
    let sphere = Sphere::new(translate(Vector3f::new(10.0, 0.0, 0.0)), 2.0);
    let ray = Ray::new(Point3f::new(0.0, 0.0, 0.0), Vector3f::new(1.0, 0.0, 0.0));

    let hit = sphere.intersect(&ray, f32::INFINITY).expect("the ray hits");

    assert!((hit.t_hit - 8.0).abs() < 1e-5, "t_hit = {}", hit.t_hit);
    assert_eq!(hit.p_obj, Point3f::new(-2.0, 0.0, 0.0));
}

/// A sphere of radius 2 moved to `x = 10` is bounded by the box from
/// `(8, −2, −2)` to `(12, 2, 2)`.
#[test]
fn moved_sphere_bounds_surround_it_in_render_space() {
    let sphere = Sphere::new(translate(Vector3f::new(10.0, 0.0, 0.0)), 2.0);

    let bounds = sphere.bounds();

    assert_eq!(bounds.p_min, Point3f::new(8.0, -2.0, -2.0));
    assert_eq!(bounds.p_max, Point3f::new(12.0, 2.0, 2.0));
}

/// A rotation by 45° about `z` turns the unit cube around the unit sphere
/// so that its corners `(±1, ±1)` land on the axes at `±√2`: the bounds
/// grow to `[−√2, √2]` in `x` and `y`, conservatively.
#[test]
fn rotated_sphere_bounds_contain_the_rotated_object_box() {
    let sphere = Sphere::new(rotate_z(45.0), 1.0);

    let bounds = sphere.bounds();

    let sqrt_2 = core::f32::consts::SQRT_2;
    for (bound, expected) in [
        (bounds.p_min.x, -sqrt_2),
        (bounds.p_min.y, -sqrt_2),
        (bounds.p_max.x, sqrt_2),
        (bounds.p_max.y, sqrt_2),
    ] {
        assert!((bound - expected).abs() < 1e-6, "{bound} vs {expected}");
    }
    assert_eq!((bounds.p_min.z, bounds.p_max.z), (-1.0, 1.0));
}
