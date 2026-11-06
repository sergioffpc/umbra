//! Vectors, points, normals, frames and spherical geometry (pbrt-v4
//! `src/pbrt/util/vecmath.h`). `Bounds2`/`Bounds3` and `DirectionCone` live
//! in `umbra-geometry`.

mod frame;
mod normal;
mod octahedral;
mod point;
mod spherical;
mod tuple;
mod vector;

pub use frame::{Frame, InFrame};
pub use normal::{Normal3, Normal3f};
pub use octahedral::OctahedralVector;
pub use point::{
    Distance, Point2, Point2f, Point2i, Point3, Point3f, Point3fi, Point3i, distance,
    distance_squared,
};
pub use spherical::{
    abs_cos_theta, cos_d_phi, cos_phi, cos_theta, cos2_theta, same_hemisphere, sin_phi, sin_theta,
    sin2_theta, spherical_direction, spherical_phi, spherical_quad_area, spherical_theta,
    spherical_triangle_area, tan_theta, tan2_theta,
};
pub use vector::{
    AngleBetween, Cross, Dot, FaceForward, Vector2, Vector2f, Vector2i, Vector3, Vector3f,
    Vector3fi, Vector3i, abs_dot, angle_between, coordinate_system, cross, dot, face_forward,
    gram_schmidt,
};
