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

/// `umbra render`: pbrt-v4's overrides keep pbrt-v4's names (ADR 0003), and
/// Umbra's own `--spp-per-pass` is named after `--spp`.
#[derive(Debug, Args)]
pub(crate) struct RenderArgs {
    /// The pbrt-v4 Scene File.
    pub(crate) scene_file: PathBuf,
    /// How many Pixel Samples every pixel gets (samples per pixel), over the
    /// Scene File's `pixelsamples`.
    ///
    /// This sets the image's quality: each Pixel Sample is one estimate of
    /// the pixel's light, and the pixel is their weighted average, so more
    /// Pixel Samples give less noise and a longer render. pbrt-v4's option,
    /// with pbrt-v4's meaning.
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
    /// How many of each pixel's `--spp` Pixel Samples one Pass holds (samples
    /// per pixel per Pass): how the work is cut, never how many Pixel Samples
    /// are taken.
    ///
    /// With N Pixel Samples per Pass, Pass p takes the sample indices from
    /// p × N to (p + 1) × N − 1 in every pixel, and the last Pass stops at
    /// `--spp`. One Tile for one Pass is a Work Unit. A
    /// smaller value gives more, smaller Work Units: a failed Worker loses
    /// less, and the end of the render waits less on one Work Unit. The
    /// image's noise stays the same, but its last bits can change, because
    /// the Film adds the Pixel Samples in other groups (ADR 0009, ADR 0022).
    /// A value of `--spp` or more renders in one Pass.
    /// Umbra's own option, never in a Scene File.
    #[arg(long, default_value = "64")]
    pub(crate) spp_per_pass: NonZeroU32,
    /// The Local Worker's threads; one per logical CPU by default.
    #[arg(long)]
    pub(crate) nthreads: Option<NonZeroUsize>,
}
