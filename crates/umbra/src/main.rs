//! umbra: a physically based ray tracer inspired by pbrt-v4.

// Dependency edges of arc42 §5.1, used once the binary has subcommands.
use umbra_cluster as _;
use umbra_parser as _;
use umbra_render as _;

#[expect(
    clippy::print_stdout,
    reason = "placeholder until the CLI writes through `tracing` and the JSON report"
)]
fn main() {
    println!("Hello, world!");
}
