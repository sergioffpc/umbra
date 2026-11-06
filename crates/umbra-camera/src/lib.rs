//! Cameras and `CameraKind`, Filters, sensor RGB, camera sample generation.
//!
//! A core crate (ADR 0014, ADR 0015): `#![no_std]` without `alloc`.

#![no_std]

// Dependency edges of arc42 §5.1, used once the crate has code.
#[cfg(feature = "serde")]
use serde as _;
use umbra_geometry as _;
use umbra_spectrum as _;
