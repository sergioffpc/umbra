#![doc = include_str!("../README.md")]

mod arena;
mod error;
mod options;
mod scene;

pub use arena::Arena;
pub use error::SceneError;
pub use options::{RenderJobOptions, Seed};
pub use scene::{
    FilmParameters, GaussianFilterParameters, HaltonSamplerParameters, PerspectiveCameraParameters,
    Scene, SceneBuilder,
};
