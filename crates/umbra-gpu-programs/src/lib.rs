#![doc = include_str!("../README.md")]
// The GPU's programs on `nvptx64`, `no_std` there; on the host, the layout
// they share with `umbra-gpu` alone (ADR 0028, ADR 0033).
#![cfg_attr(target_arch = "nvptx64", no_std)]
// `ptx-kernel` entry points and inline PTX, on `nvptx64` only: the only
// unstable features of the workspace (ADR 0027).
#![cfg_attr(target_arch = "nvptx64", feature(abi_ptx, asm_experimental_arch))]
// No `f64` on the GPU (ADR 0028): `clippy.toml` lists it.
#![warn(clippy::disallowed_types)]

mod launch;
#[cfg(all(target_arch = "nvptx64", feature = "math-program"))]
mod math;
#[cfg(feature = "math-program")]
mod math_launch;
#[cfg(target_arch = "nvptx64")]
mod optix;
#[cfg(target_arch = "nvptx64")]
mod programs;
#[cfg(target_arch = "nvptx64")]
mod stages;
mod wavefront;

pub use launch::{HitKind, HitRecord, LaunchParams, RayRecord};
#[cfg(feature = "math-program")]
pub use math_launch::{MathFunction, MathParams, UnknownMathFunction};
pub use wavefront::{
    DeviceSlice, FilmSampleRecord, Lanes, PixelSampleQueue, RandomizerRecord, SamplerRecord,
    SceneRecord, WavefrontParams, WorkUnitRecord,
};
