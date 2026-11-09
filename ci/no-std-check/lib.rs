//! Links the seven core crates into a `no_std` `staticlib` (ADR 0014,
//! ADR 0023). See `Cargo.toml` for what makes the build fail.

#![no_std]

use umbra_camera as _;
use umbra_geometry as _;
use umbra_integrators as _;
use umbra_math as _;
use umbra_sampling as _;
use umbra_shading as _;
use umbra_spectrum as _;

/// Never called: the check is that this crate links.
#[panic_handler]
fn panic(_: &core::panic::PanicInfo<'_>) -> ! {
    loop {}
}
