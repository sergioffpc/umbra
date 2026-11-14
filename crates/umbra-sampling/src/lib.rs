#![doc = include_str!("../README.md")]
#![no_std]
// No `f64` in the core (ADR 0028): `clippy.toml` lists it.
#![warn(clippy::disallowed_types)]

mod hash;
mod lowdiscrepancy;
mod piecewise;
mod rng;
mod sampler;
mod warp;

pub use hash::{Hashable, hash, hash_float, mix_bits, murmur_hash_64a};
pub use lowdiscrepancy::{
    DigitPermutation, DigitPermutations, PRIME_TABLE_SIZE, PRIMES, inverse_radical_inverse,
    owen_scrambled_radical_inverse, permutation_element, radical_inverse,
    scrambled_radical_inverse,
};
pub use piecewise::{
    PiecewiseConstant1D, PiecewiseConstant1DSample, PiecewiseConstant2D, PiecewiseConstant2DSample,
};
pub use rng::{Rng, Uniform, UniformBelow};
pub use sampler::{HaltonRandomizer, HaltonSampler, RandomizeStrategy, Sampler, SamplerKind};
pub use warp::*;
