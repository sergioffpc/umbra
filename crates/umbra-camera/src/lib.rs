#![doc = include_str!("../README.md")]
#![no_std]
// No `f64` in the core (ADR 0028): `clippy.toml` lists it.
#![warn(clippy::disallowed_types)]

mod camera;
mod filter;
mod sensor;

pub use camera::{
    Camera, CameraBaseParameters, CameraKind, CameraRay, CameraSample, CameraTransform,
    PerspectiveCamera, ThinLens, get_camera_sample,
};
pub use filter::{Filter, FilterKind, FilterSample, FilterSampler, GaussianFilter};
pub use sensor::PixelSensor;
