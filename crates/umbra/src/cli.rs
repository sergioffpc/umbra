//! The command line, with `clap`.

use std::net::SocketAddr;
use std::num::{NonZeroU32, NonZeroUsize};
use std::path::PathBuf;

use clap::{Args, Parser, Subcommand, ValueEnum};
use umbra_cluster::{RenderDevice, RenderDevices};

use crate::logs::{LogFormat, parse_filter};

/// Umbra: a physically based renderer of pbrt-v4 Scene Files.
#[derive(Debug, Parser)]
#[command(name = "umbra", version, about)]
pub(crate) struct Cli {
    #[command(subcommand)]
    pub(crate) command: Command,
    /// The events logged to stderr, as a `RUST_LOG`-style filter.
    #[arg(long, global = true, default_value = "info", value_parser = parse_filter)]
    pub(crate) log: String,
    /// The log format; `human` on a terminal and `json` otherwise by default.
    #[arg(long, global = true, value_enum)]
    pub(crate) log_format: Option<LogFormat>,
}

/// The subcommands.
#[derive(Debug, Subcommand)]
pub(crate) enum Command {
    /// Render a Scene File into its final image and Render Job Report.
    Render(RenderArgs),
    /// Serve one Render Job for the Coordinator at an address: how `umbra
    /// render` runs the Local Worker of each Render Device, in a child
    /// process.
    #[command(hide = true)]
    Worker(WorkerArgs),
}

/// `umbra render`: pbrt-v4's overrides keep pbrt-v4's names (ADR 0003), and
/// Umbra's own `--spp-per-pass` is named after `--spp`.
#[derive(Debug, Args)]
pub(crate) struct RenderArgs {
    /// The pbrt-v4 Scene File to render.
    pub(crate) scene_file: PathBuf,
    /// Threads to render with; one per logical CPU by default.
    #[arg(long)]
    pub(crate) nthreads: Option<NonZeroUsize>,
    /// The output image, over the Film's; its extension names the format.
    #[arg(long)]
    pub(crate) outfile: Option<PathBuf>,
    /// The samplers' seed, over the Scene File's; 0 by default.
    #[arg(long, allow_negative_numbers = true)]
    pub(crate) seed: Option<i32>,
    /// Pixel Samples per pixel, over the Scene File's; more gives less noise.
    #[arg(long)]
    pub(crate) spp: Option<NonZeroU32>,
    /// Pixel Samples per pixel in one Pass: how the work is cut, not quality.
    #[arg(long, default_value = "64")]
    pub(crate) spp_per_pass: NonZeroU32,
    /// The side of a square Tile, in pixels.
    #[arg(long, default_value = "32")]
    pub(crate) tile_size: NonZeroU32,
    /// The Render Devices of this machine to render on, comma-separated;
    /// the CPU and every GPU by default, or the CPU alone, with a warning,
    /// when no GPU is usable.
    #[arg(long, value_enum, value_delimiter = ',')]
    pub(crate) render_devices: Vec<DeviceKind>,
}

/// A kind of Render Device, for `--render-devices`.
#[derive(Debug, Clone, Copy, PartialEq, Eq, ValueEnum)]
pub(crate) enum DeviceKind {
    /// The CPU's cores.
    Cpu,
    /// Every GPU.
    Gpu,
}

impl RenderArgs {
    /// The Render Devices `--render-devices` chooses.
    pub(crate) fn render_devices(&self) -> RenderDevices {
        let chosen = |kind| self.render_devices.contains(&kind);
        match (chosen(DeviceKind::Cpu), chosen(DeviceKind::Gpu)) {
            (false, false) => RenderDevices::Usable,
            (true, false) => RenderDevices::Cpu,
            (false, true) => RenderDevices::Gpu,
            (true, true) => RenderDevices::CpuAndGpu,
        }
    }
}

/// `umbra worker`: a Worker for one Render Job of the Coordinator at
/// `--coordinator`.
#[derive(Debug, Args)]
pub(crate) struct WorkerArgs {
    /// The Render Device to render on: `cpu`, or `gpu:N` for GPU `N`.
    #[arg(long, default_value = "cpu")]
    pub(crate) render_device: RenderDevice,
    /// The Coordinator's address.
    #[arg(long)]
    pub(crate) coordinator: SocketAddr,
    /// Threads to render with on the CPU; one per logical CPU by default.
    #[arg(long)]
    pub(crate) nthreads: Option<NonZeroUsize>,
}
