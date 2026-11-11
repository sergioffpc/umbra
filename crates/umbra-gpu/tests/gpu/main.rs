//! The GPU tests: `umbra-gpu` through its public interface, on an NVIDIA GPU
//! with a driver for OptiX 9.0 (ADR 0026).
//!
//! The test binary's name, `gpu`, is their label: nextest's `default`
//! profile, and the `ci` and `miri` profiles that inherit from it, leave out
//! every test binary named `gpu`, since CI has no GPU; the `gpu` profile
//! runs only those, on the P620: `cargo nextest run --profile gpu`
//! (`.config/nextest.toml`).

mod optix;
