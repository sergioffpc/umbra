//! Rays traced through OptiX hit the triangle and the sphere where the core
//! says they do (#126).
//!
//! The triangle's expected hits are worked by hand; the sphere's come from
//! the core's own `Sphere::intersect` on the CPU, the code its intersection
//! program runs on the GPU.

use umbra_geometry::{Ray, Sphere};
use umbra_gpu::{Geometry, Hit, Tracer};
use umbra_math::{Point3f, Vector3f, translate};

/// A triangle in the plane `z = 5`, spanning `x ∈ [−0.5, 0.5]` at `y = 0`.
const TRIANGLE: [Point3f; 3] = [
    Point3f::new(-1.0, -1.0, 5.0),
    Point3f::new(1.0, -1.0, 5.0),
    Point3f::new(0.0, 1.0, 5.0),
];

/// A sphere of radius 2 centred on `(0, 0, 10)`.
fn sphere() -> Sphere {
    Sphere::new(translate(Vector3f::new(0.0, 0.0, 10.0)), 2.0)
}

/// The tracer on the first GPU, with [`TRIANGLE`] and [`sphere`].
fn tracer() -> Tracer {
    Tracer::new(0, TRIANGLE, &sphere()).unwrap_or_else(|error| panic!("no GPU tracer: {error}"))
}

fn ray(o: [f32; 3], d: [f32; 3]) -> Ray {
    Ray::new(
        Point3f::new(o[0], o[1], o[2]),
        Vector3f::new(d[0], d[1], d[2]),
    )
}

/// Whether `a` and `b` agree to a few ULP of the larger, at least of 1.
fn close(a: f32, b: f32) -> bool {
    (a - b).abs() <= 4.0 * f32::EPSILON * a.abs().max(b.abs()).max(1.0)
}

#[test]
#[ignore = "needs an NVIDIA GPU with OptiX 9.0: run by scripts/gpu-test"]
fn rays_hit_the_triangle_where_its_plane_is() {
    let tracer = tracer();
    // Every direction has `d.z = ±1`, so a hit on the plane `z = 5` is at
    // `t = 5` from `z = 0` or `z = 10`.
    let cases = [
        (ray([0.0, 0.0, 0.0], [0.0, 0.0, 1.0]), Some(5.0)),
        (ray([0.0, 0.0, 0.0], [0.0, -0.1, 1.0]), Some(5.0)),
        (ray([0.0, 0.0, 10.0], [0.0, 0.0, -1.0]), Some(5.0)),
        (ray([0.0, 0.0, 0.0], [0.2, 0.0, 1.0]), None),
        (ray([0.0, 0.0, 0.0], [0.0, 0.0, -1.0]), None),
    ];
    let rays: Vec<Ray> = cases.iter().map(|&(ray, _)| ray).collect();

    let hits = tracer
        .trace(Geometry::Triangle, &rays)
        .expect("the launch runs");

    for ((ray, expected), hit) in cases.iter().zip(&hits) {
        match (expected, hit) {
            (None, None) => {}
            (
                Some(t),
                Some(Hit {
                    t: t_gpu,
                    geometry: Geometry::Triangle,
                }),
            ) if close(*t, *t_gpu) => {}
            _ => panic!("{ray:?}: expected {expected:?}, the GPU found {hit:?}"),
        }
    }
}

#[test]
#[ignore = "needs an NVIDIA GPU with OptiX 9.0: run by scripts/gpu-test"]
fn rays_hit_the_sphere_where_the_core_says() {
    let tracer = tracer();
    let sphere = sphere();
    let mut rays = vec![
        // Through the centre: the near side at `t = 8`.
        ray([0.0, 0.0, 0.0], [0.0, 0.0, 1.0]),
        // Off centre: at `z = 10 − √3`.
        ray([0.0, 1.0, 0.0], [0.0, 0.0, 1.0]),
        // Past it.
        ray([0.0, 3.0, 0.0], [0.0, 0.0, 1.0]),
        // From the centre: the far side, one radius away.
        ray([0.0, 0.0, 10.0], [1.0, 0.0, 0.0]),
        // Away from it.
        ray([0.0, 0.0, 20.0], [0.0, 0.0, 1.0]),
    ];
    // A fan from the origin across the sphere and past its rim.
    rays.extend((0..32_u8).flat_map(|i| {
        (0..32_u8).map(move |j| {
            let spread = |k: u8| (f32::from(k) - 15.5) / 64.0;
            ray([0.0, 0.0, 0.0], [spread(i), spread(j), 1.0])
        })
    }));

    let hits = tracer
        .trace(Geometry::Sphere, &rays)
        .expect("the launch runs");

    assert_eq!(hits.len(), rays.len(), "one result per ray");
    for (ray, hit) in rays.iter().zip(&hits) {
        let expected = sphere.intersect(ray, f32::INFINITY).map(|hit| hit.t_hit);
        match (expected, hit) {
            (None, None) => {}
            (
                Some(t),
                Some(Hit {
                    t: t_gpu,
                    geometry: Geometry::Sphere,
                }),
            ) if close(t, *t_gpu) => {}
            _ => panic!("{ray:?}: the core says {expected:?}, the GPU found {hit:?}"),
        }
    }
}
