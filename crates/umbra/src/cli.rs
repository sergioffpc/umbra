//! The command line, with `clap`.

use std::num::{NonZeroU32, NonZeroUsize};
use std::path::PathBuf;

use clap::{Args, Parser, Subcommand};

/// Umbra: a physically based renderer of pbrt-v4 Scene Files.
#[derive(Debug, Parser)]
#[command(name = "umbra", version, about)]
pub(crate) struct Cli {
    #[command(subcommand)]
    pub(crate) command: Command,
}

/// The subcommands.
#[derive(Debug, Subcommand)]
pub(crate) enum Command {
    /// Render a Scene File into its final image and Render Job Report.
    Render(RenderArgs),
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
}
