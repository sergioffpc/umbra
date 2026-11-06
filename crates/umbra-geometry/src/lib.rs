//! `Ray`, `Bounds`, interactions, Shapes and `ShapeKind`, BVH node layout and traversal.
//!
//! A core crate (ADR 0014, ADR 0015): `#![no_std]` without `alloc`.

#![no_std]

// Dependency edges of arc42 §5.1, used once the crate has code.
#[cfg(feature = "serde")]
use serde as _;
use umbra_math as _;
use umbra_sampling as _;
