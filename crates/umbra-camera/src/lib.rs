#![doc = include_str!("../README.md")]
#![no_std]

// Dependency edges of arc42 §5.1, used once the crate has code.
#[cfg(feature = "serde")]
use serde as _;
use umbra_geometry as _;
use umbra_spectrum as _;
