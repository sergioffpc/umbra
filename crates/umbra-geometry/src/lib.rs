#![doc = include_str!("../README.md")]
#![no_std]
// No `f64` in the core (ADR 0028): `clippy.toml` lists it.
#![warn(clippy::disallowed_types)]

mod bounds;
mod handle;
mod ray;
mod sphere;

// Dependency edges of arc42 §5.1, used once the crate has code that needs
// them.
#[cfg(feature = "serde")]
use serde as _;
use umbra_sampling as _;

pub use bounds::{Bounds2f, Bounds3f};
pub use handle::Handle;
pub use ray::Ray;
pub use sphere::{QuadricIntersection, Sphere};
