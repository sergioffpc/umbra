#![doc = include_str!("../README.md")]
#![no_std]
// No `f64` in the core (ADR 0028): `clippy.toml` lists it.
#![warn(clippy::disallowed_types)]

mod camera;
mod filter;
mod sensor;

// Dependency edge of arc42 §5.1, used once the crate has types to serialise.
#[cfg(feature = "serde")]
use serde as _;

pub use camera::{
    Camera, CameraBaseParameters, CameraKind, CameraRay, CameraSample, CameraTransform,
    PerspectiveCamera, ThinLens, get_camera_sample,
};
pub use filter::{Filter, FilterKind, FilterSample, FilterSampler, GaussianFilter};
pub use sensor::PixelSensor;
