#![doc = include_str!("../README.md")]
#![no_std]
// No `f64` in the core (ADR 0028): `clippy.toml` lists it.
#![warn(clippy::disallowed_types)]

mod pixel_sample;
mod recorder;
mod view;
mod volpath;

// Dependency edges of arc42 §5.1, used once the crate has code that needs
// them.
use umbra_shading as _;

pub use pixel_sample::{
    FilmSample, PixelSampleId, PixelSampleState, RayIntegrator, evaluate_pixel_sample,
};
pub use recorder::{NoRecorder, Recorder};
pub use view::SceneView;
pub use volpath::VolPathIntegrator;
