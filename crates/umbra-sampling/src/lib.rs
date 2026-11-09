#![doc = include_str!("../README.md")]
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
