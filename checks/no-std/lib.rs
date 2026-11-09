//! The `no_std` check of the rendering core (ADR 0014, ADR 0023).
//!
//! A `#![no_std]` library with its own panic handler that uses the seven core
//! crates. If any of them, or any of their dependencies, links `std`, this
//! crate stops compiling with a duplicate `panic_impl` lang item (E0152), so
//! every `cargo check`, `cargo clippy`, `cargo build` and `cargo test` of the
//! workspace runs the check. `alloc` is caught by `tests/no_alloc.rs`.

#![no_std]

use umbra_camera as _;
use umbra_geometry as _;
use umbra_integrators as _;
use umbra_math as _;
use umbra_sampling as _;
use umbra_shading as _;
use umbra_spectrum as _;

/// Never called: the check is that this crate compiles.
#[panic_handler]
fn panic(_: &core::panic::PanicInfo<'_>) -> ! {
    loop {}
}
