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
    /// Render a Scene File into an EXR final image and its Render Job
    /// Report.
    Render(RenderArgs),
}

/// `umbra render`: pbrt-v4's overrides keep pbrt-v4's names (ADR 0003).
#[derive(Debug, Args)]
pub(crate) struct RenderArgs {
    /// The pbrt-v4 Scene File.
    pub(crate) scene_file: PathBuf,
    /// Pixel Samples per pixel, over the Scene File's.
    #[arg(long)]
    pub(crate) spp: Option<NonZeroU32>,
    /// The seed of the samplers, over the Scene File's; 0 by default.
    #[arg(long, allow_negative_numbers = true)]
    pub(crate) seed: Option<i32>,
    /// The output image, over the Film's `filename`; an `.exr` file.
    #[arg(long)]
    pub(crate) outfile: Option<PathBuf>,
    /// The side of a square Tile, in pixels.
    #[arg(long, default_value = "32")]
    pub(crate) tile_size: NonZeroU32,
    /// The Pixel Samples of a pixel in one Pass.
    #[arg(long, default_value = "64")]
    pub(crate) pixel_samples_per_pass: NonZeroU32,
    /// The Local Worker's threads; one per logical CPU by default.
    #[arg(long)]
    pub(crate) nthreads: Option<NonZeroUsize>,
}
