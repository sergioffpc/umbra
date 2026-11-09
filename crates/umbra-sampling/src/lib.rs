//! Sampling functions (warps), hashing and RNG, low-discrepancy sequences, `Sampler` and `SamplerKind`.
//!
//! A core crate (ADR 0014, ADR 0015): `#![no_std]` without `alloc`. The
//! routines are pbrt-v4's (`src/pbrt/util/hash.h`, `rng.h`, `sampling.h`),
//! ported faithfully so that a seed draws pbrt-v4's numbers (ADR 0004), in
//! `f32` (ADR 0001) and over the `libm` crate (ADR 0022). Names follow
//! pbrt-v4, in snake case.

#![no_std]

mod hash;
mod piecewise;
mod rng;
mod warp;

// Dependency edge of arc42 §5.1, used once the crate has types to serialise.
#[cfg(feature = "serde")]
use serde as _;

pub use hash::{Hashable, hash, hash_float, mix_bits, murmur_hash_64a};
pub use piecewise::{
    PiecewiseConstant1D, PiecewiseConstant1DSample, PiecewiseConstant2D, PiecewiseConstant2DSample,
};
pub use rng::{Rng, Uniform, UniformBelow};
pub use warp::*;
