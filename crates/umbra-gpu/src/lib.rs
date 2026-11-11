#![doc = include_str!("../README.md")]

mod error;
// The layout shared with the device: one file, compiled by both crates.
#[path = "../../umbra-kernels/src/launch.rs"]
mod launch;
mod optix;
mod records;
mod tracer;

pub use error::Error;
pub use tracer::Tracer;

/// The geometry a [`Tracer`] traces rays against.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Geometry {
    /// The triangle, by OptiX's built-in intersection.
    Triangle,
    /// The sphere, by the custom intersection program, which calls the
    /// core's `Sphere::intersect`.
    Sphere,
}

/// Where a ray first hit a [`Geometry`].
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Hit {
    /// The ray parameter of the hit.
    pub t: f32,
    /// What it hit.
    pub geometry: Geometry,
}
