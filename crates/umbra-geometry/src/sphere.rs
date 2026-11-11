//! Spheres (pbrt-v4 `Sphere`, `src/pbrt/shapes.h`).

use umbra_math::{Interval, Point3f, Point3fi, Transform, Vector3f, Vector3fi, distance};

use crate::{Bounds3f, Ray};

/// A full sphere of radius `radius` centred on its object space's origin.
///
/// An arena type: `#[repr(C)]`, with the layout assertions below, so the
/// host copies it to the GPU byte for byte (ADR 0028). pbrt-v4's partial
/// spheres (`zmin`, `zmax`, `phimax`) and its surface interaction come with
/// the Shapes of issue #79.
///
/// PBR 4e §6.2, pbrt-v4 `Sphere`.
#[derive(Debug, Clone, Copy, PartialEq)]
#[repr(C)]
pub struct Sphere {
    render_from_object: Transform,
    radius: f32,
}

// The host copies arena slices of these into GPU buffers byte for byte
// (ADR 0028, arc42 §8.12): their layout is the same on `x86_64` and `nvptx64`.
const _: () = {
    use core::mem::offset_of;

    assert!(
        size_of::<Sphere>() == 272,
        "`Sphere` is 272 bytes: 256 of `Transform`, 4 of radius, 12 of tail padding"
    );
    assert!(align_of::<Sphere>() == 16, "`Sphere` is 16-byte aligned");
    assert!(
        offset_of!(Sphere, render_from_object) == 0,
        "`Sphere::render_from_object` is at byte 0"
    );
    assert!(
        offset_of!(Sphere, radius) == 256,
        "`Sphere::radius` is at byte 256"
    );
};

/// Where a ray first meets a quadric, before the surface interaction is
/// built from it.
///
/// pbrt-v4 `QuadricIntersection`, without `phi`, which only partial spheres
/// and the surface interaction read.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct QuadricIntersection {
    /// The ray parameter of the hit.
    pub t_hit: f32,
    /// The hit point in object space, refined onto the surface.
    pub p_obj: Point3f,
}

impl Sphere {
    /// The sphere of radius `radius` in the object space that
    /// `render_from_object` maps to render space.
    #[inline]
    #[must_use]
    pub const fn new(render_from_object: Transform, radius: f32) -> Self {
        Self {
            render_from_object,
            radius,
        }
    }

    /// The box around the sphere in render space.
    ///
    /// pbrt-v4 `Sphere::Bounds`.
    #[must_use]
    pub fn bounds(&self) -> Bounds3f {
        let r = self.radius;
        Bounds3f::new(Point3f::new(-r, -r, -r), Point3f::new(r, r, r))
            .transform(&self.render_from_object)
    }

    /// The nearest hit of `ray` with `t` in `(0, t_max]`, with conservative
    /// floating-point bounds on `t` so that no hit behind the origin is
    /// reported.
    ///
    /// PBR 4e §6.2.2, pbrt-v4 `Sphere::BasicIntersect`.
    #[must_use]
    pub fn intersect(&self, ray: &Ray, t_max: f32) -> Option<QuadricIntersection> {
        let oi = self.render_from_object.apply_inverse(Point3fi::from(ray.o));
        let di = self
            .render_from_object
            .apply_inverse(Vector3fi::from(ray.d));
        let (t0, t1) = self.quadratic_roots(oi, di)?;
        if t0.upper_bound() > t_max || t1.lower_bound() <= 0.0 {
            return None;
        }
        let t_shape_hit = if t0.lower_bound() > 0.0 { t0 } else { t1 };
        if t_shape_hit.upper_bound() > t_max {
            return None;
        }
        let t_hit = f32::from(t_shape_hit);
        let p_hit = Point3f::from(oi) + Vector3f::from(di) * t_hit;
        // Refine the hit point onto the surface (PBR 4e §6.8.5).
        let p_obj = p_hit * (self.radius / distance(p_hit, Point3f::default()));
        Some(QuadricIntersection { t_hit, p_obj })
    }

    /// The two roots of the ray–sphere quadratic in object space, lesser
    /// first, or `None` when the discriminant may be negative. The
    /// discriminant is pbrt-v4's form, `4a(r + |v|)(r − |v|)`, which loses
    /// less precision than `b² − 4ac`.
    fn quadratic_roots(&self, oi: Point3fi, di: Vector3fi) -> Option<(Interval, Interval)> {
        let a = di.x.sqr() + di.y.sqr() + di.z.sqr();
        let b = 2.0 * (di.x * oi.x + di.y * oi.y + di.z * oi.z);
        let radius = Interval::from(self.radius);
        let c = oi.x.sqr() + oi.y.sqr() + oi.z.sqr() - radius.sqr();
        let v = Vector3fi::from(oi - b / (2.0 * a) * di);
        let length = v.length();
        let discrim = 4.0 * a * (radius + length) * (radius - length);
        if discrim.lower_bound() < 0.0 {
            return None;
        }
        let root_discrim = discrim.sqrt();
        let q = if f32::from(b) < 0.0 {
            -0.5 * (b - root_discrim)
        } else {
            -0.5 * (b + root_discrim)
        };
        let (t0, t1) = (q / a, c / q);
        Some(if t0.lower_bound() > t1.lower_bound() {
            (t1, t0)
        } else {
            (t0, t1)
        })
    }
}
