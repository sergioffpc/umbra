#![doc = include_str!("../README.md")]

mod error;
mod optix;
mod records;
mod renderer;
mod tracer;

pub use error::Error;
pub use renderer::Renderer;
pub use tracer::Tracer;
// The math program's functions, with the `math-program` feature alone (#146).
#[cfg(feature = "math-program")]
pub use umbra_gpu_programs::{MathFunction, UnknownMathFunction};

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
