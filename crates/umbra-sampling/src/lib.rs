//! Sampling functions (warps), hashing and RNG, low-discrepancy sequences, `Sampler` and `SamplerKind`.
//!
//! A core crate (ADR 0014, ADR 0015): `#![no_std]` without `alloc`.

#![no_std]

// Dependency edges of arc42 §5.1, used once the crate has code.
#[cfg(feature = "serde")]
use serde as _;
use umbra_math as _;
