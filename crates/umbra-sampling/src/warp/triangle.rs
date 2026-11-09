//! Triangles, uniformly by area and uniformly by the solid angle they subtend
//! from a point.

use umbra_math::{
    PI, Point2f, Point3f, Vector3f, angle_between, clamp, cross, difference_of_products, dot,
    gram_schmidt, lerp, safe_sqrt, sin_cos, sqr, sum_of_products,
};

/// Barycentrics `(b₀, b₁, b₂)` of a uniformly distributed point of a
/// triangle, by Heitz's low-distortion map.
///
/// PBR 4e §A.5.5, pbrt-v4 `SampleUniformTriangle`.
#[must_use]
pub fn sample_uniform_triangle(u: Point2f) -> [f32; 3] {
    let (b0, b1) = if u.x < u.y {
        let b0 = u.x / 2.0;
        (b0, u.y - b0)
    } else {
        let b1 = u.y / 2.0;
        (u.x - b1, b1)
    };
    [b0, b1, 1.0 - b0 - b1]
}

/// The `u` that [`sample_uniform_triangle`] maps to the barycentrics `b`.
///
/// pbrt-v4 `InvertUniformTriangleSample`.
#[must_use]
pub fn invert_uniform_triangle_sample(b: [f32; 3]) -> Point2f {
    if b[0] > b[1] {
        Point2f::new(b[0] + b[1], 2.0 * b[1])
    } else {
        Point2f::new(2.0 * b[0], b[1] + b[0])
    }
}

/// A point of a triangle sampled uniformly by solid angle: its barycentrics
/// and the PDF with respect to solid angle.
///
/// pbrt-v4 `SampleSphericalTriangle`'s result and its `pdf` argument.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct SphericalTriangleSample {
    /// The barycentrics `(b₀, b₁, b₂)` of the sampled point.
    pub barycentrics: [f32; 3],
    /// `1 / A` for the spherical triangle's area `A`, and 0 when the
    /// triangle is degenerate as seen from the reference point; the sample
    /// is then unusable.
    pub pdf: f32,
}

/// The spherical triangle that the triangle `v` projects to on the unit
/// sphere around `p`: its vertices, the unit normals of its edge planes and
/// its interior angles.
struct SphericalTriangle {
    a: Vector3f,
    b: Vector3f,
    c: Vector3f,
    n_ab: Vector3f,
    alpha: f32,
    beta: f32,
    gamma: f32,
}

impl SphericalTriangle {
    /// The projection of `v` seen from `p`, or `None` if two of its
    /// directions are parallel.
    fn new(v: [Point3f; 3], p: Point3f) -> Option<Self> {
        let [a, b, c] = v.map(|vi| {
            let d = vi - p;
            debug_assert!(
                d.length_squared() > 0.0,
                "{p:?} is a vertex of the triangle"
            );
            d.normalize()
        });
        let edges = [cross(a, b), cross(b, c), cross(c, a)];
        if edges.iter().any(|n| n.length_squared() == 0.0) {
            return None;
        }
        let [n_ab, n_bc, n_ca] = edges.map(Vector3f::normalize);
        Some(Self {
            a,
            b,
            c,
            n_ab,
            alpha: angle_between(n_ab, -n_ca),
            beta: angle_between(n_bc, -n_ab),
            gamma: angle_between(n_ca, -n_bc),
        })
    }

    /// The cosine of the arc length from `a` to the vertex `c′` of the
    /// sub-triangle `(a, b, c′)` whose area plus π is `sampled_area_plus_pi`
    /// (Arvo).
    fn cos_b_prime(&self, sampled_area_plus_pi: f32) -> f32 {
        let (sin_alpha, cos_alpha) = sin_cos(self.alpha);
        let (sin_area, cos_area) = sin_cos(sampled_area_plus_pi);
        let sin_phi = sin_area * cos_alpha - cos_area * sin_alpha;
        let cos_phi = cos_area * cos_alpha + sin_area * sin_alpha;
        let k1 = cos_phi + cos_alpha;
        let k2 = sin_phi - sin_alpha * dot(self.a, self.b);
        let cos_b_prime = (k2 + difference_of_products(k2, cos_phi, k1, sin_phi) * cos_alpha)
            / (sum_of_products(k2, sin_phi, k1, cos_phi) * sin_alpha);
        // NaN if the triangle covers about the whole hemisphere; the caller
        // is to avoid that case (pbrt-v4).
        debug_assert!(!cos_b_prime.is_nan(), "the spherical triangle is too large");
        clamp(cos_b_prime, -1.0, 1.0)
    }
}

/// The barycentrics, in the triangle `v`, of the point along `w` from `p`,
/// clamped into the triangle (pbrt-v4's ray–triangle solve).
fn barycentrics_along(v: [Point3f; 3], p: Point3f, w: Vector3f) -> [f32; 3] {
    let (e1, e2) = (v[1] - v[0], v[2] - v[0]);
    let s1 = cross(w, e2);
    let divisor = dot(s1, e1);
    if divisor == 0.0 {
        // The triangle covers (nearly) the whole hemisphere.
        return [1.0 / 3.0; 3];
    }
    let inv_divisor = 1.0 / divisor;
    let s = p - v[0];
    let mut b1 = clamp(dot(s, s1) * inv_divisor, 0.0, 1.0);
    let mut b2 = clamp(dot(w, cross(s, e1)) * inv_divisor, 0.0, 1.0);
    if b1 + b2 > 1.0 {
        // pbrt-v4 divides `b2` by the already rescaled `b1` plus `b2`.
        b1 /= b1 + b2;
        b2 /= b1 + b2;
    }
    [1.0 - b1 - b2, b1, b2]
}

/// A point of the triangle `v` sampled uniformly by the solid angle it
/// subtends from `p` (Arvo's method).
///
/// PBR 4e §6.5.4, pbrt-v4 `SampleSphericalTriangle`.
#[must_use]
pub fn sample_spherical_triangle(
    v: [Point3f; 3],
    p: Point3f,
    u: Point2f,
) -> SphericalTriangleSample {
    let Some(t) = SphericalTriangle::new(v, p) else {
        return SphericalTriangleSample {
            barycentrics: [0.0; 3],
            pdf: 0.0,
        };
    };

    // Uniformly sample the triangle area A to compute A′.
    let area_plus_pi = t.alpha + t.beta + t.gamma;
    let sampled_area_plus_pi = lerp(u.x, PI, area_plus_pi);
    let area = area_plus_pi - PI;
    let pdf = if area <= 0.0 { 0.0 } else { 1.0 / area };

    // Sample c′ along the arc between a and c.
    let cos_b_prime = t.cos_b_prime(sampled_area_plus_pi);
    let sin_b_prime = safe_sqrt(1.0 - sqr(cos_b_prime));
    let cp = cos_b_prime * t.a + sin_b_prime * gram_schmidt(t.c, t.a).normalize();

    // Sample the direction along the arc between b and c′.
    let cos_theta = 1.0 - u.y * (1.0 - dot(cp, t.b));
    let sin_theta = safe_sqrt(1.0 - sqr(cos_theta));
    let w = cos_theta * t.b + sin_theta * gram_schmidt(cp, t.b).normalize();

    SphericalTriangleSample {
        barycentrics: barycentrics_along(v, p, w),
        pdf,
    }
}

/// The `u` that [`sample_spherical_triangle`] maps to the direction `w` from
/// `p` (after Arvo's `SphTri.C`). Where pbrt-v4 gives up it returns its
/// values: `(0, 0)` if the triangle is degenerate as seen from `p`, and
/// `(0.5, 0.5)` if `w` is along an edge.
///
/// pbrt-v4 `InvertSphericalTriangleSample`.
#[must_use]
pub fn invert_spherical_triangle_sample(v: [Point3f; 3], p: Point3f, w: Vector3f) -> Point2f {
    let Some(t) = SphericalTriangle::new(v, p) else {
        return Point2f::new(0.0, 0.0);
    };

    // Find the vertex c′ along the arc from a to c for w.
    let mut cp = cross(cross(t.b, w), cross(t.c, t.a)).normalize();
    if dot(cp, t.a + t.c) < 0.0 {
        cp = -cp;
    }

    // Invert the uniform area sampling to find u₀.
    let u0 = if dot(t.a, cp) > 0.999_998_45 {
        // c′ is within 0.1° of a.
        0.0
    } else {
        let (n_cpb, n_acp) = (cross(cp, t.b), cross(t.a, cp));
        if n_cpb.length_squared() == 0.0 || n_acp.length_squared() == 0.0 {
            return Point2f::new(0.5, 0.5);
        }
        let (n_cpb, n_acp) = (n_cpb.normalize(), n_acp.normalize());
        let ap = t.alpha + angle_between(t.n_ab, n_cpb) + angle_between(n_acp, -n_cpb) - PI;
        let area = t.alpha + t.beta + t.gamma - PI;
        ap / area
    };

    // Invert the arc sampling to find u₁.
    let u1 = (1.0 - dot(w, t.b)) / (1.0 - dot(cp, t.b));
    Point2f::new(clamp(u0, 0.0, 1.0), clamp(u1, 0.0, 1.0))
}
