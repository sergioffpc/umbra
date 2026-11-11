//! Tests of `umbra-gpu` through its public interface. They need an NVIDIA
//! GPU with a driver for OptiX 9.0, so they are `#[ignore]`d: CI has no GPU,
//! and `scripts/gpu-test` runs them on the P620 (ADR 0026).

mod optix;
