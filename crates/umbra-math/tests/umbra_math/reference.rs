//! The safe, book-faithful reference implementations of every operation that
//! the crate also provides as an aggressively optimised kernel (ADR 0007):
//! pbrt-v4's code, in plain scalar Rust over `libm`, with no `unsafe` and no
//! SIMD.
//!
//! The optimised version in `umbra_math` is the one the renderer uses; it
//! must compute, for every input, exactly the bits the function here
//! computes (NaN payloads excepted, as in the rest of the crate). The
//! equivalence tests in `equivalence/` check that over random bit patterns
//! and the special values, and `benches/kernels.rs`, which includes this
//! module by `#[path]`, times the two side by side.
//!
//! Each module mirrors the crate module it is the reference for, and works on
//! the public types through their public interface, so it shares no code with
//! the kernels it checks.

// Explicit paths, so the benchmark can include this file by `#[path]` too.
#[path = "reference/float.rs"]
pub(crate) mod float;
#[path = "reference/interval.rs"]
pub(crate) mod interval;
#[path = "reference/math.rs"]
pub(crate) mod math;
#[path = "reference/square_matrix.rs"]
pub(crate) mod square_matrix;
#[path = "reference/transform.rs"]
pub(crate) mod transform;
