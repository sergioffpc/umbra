//! Tests of `umbra-math` through its public interface: the pbrt-v4 unit tests
//! it ports, the `Vec4`/`Mask4` facade against its scalar reference, and the
//! optimised kernels against the safe references in `reference/` (ADR 0007).

mod accuracy;
mod equivalence;
mod pbrt_v4;
mod reference;
mod rng;
mod sampling;
mod simd_test;
mod transcendentals;
