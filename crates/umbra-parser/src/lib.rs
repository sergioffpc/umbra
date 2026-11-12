#![doc = include_str!("../README.md")]

mod diagnostic;
mod error;
mod files;
mod params;
mod scene_target;
mod syntax;
mod tokenizer;

use std::num::NonZeroU32;
use std::path::{Path, PathBuf};

pub use diagnostic::{Diagnostic, Location, Severity};
pub use error::Error;
use umbra_scene::{ContentHash, RenderJobOptions, Scene, Seed};

use crate::diagnostic::Diagnostics;
use crate::files::{SceneFiles, Unread};
use crate::scene_target::SceneTarget;
use crate::syntax::Parser;

/// pbrt-v4's command-line overrides, which take precedence over the Scene
/// File (ADR 0003). `None` leaves the Scene File's setting, or the default.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct Overrides {
    /// `--spp`: the Pixel Samples per pixel.
    pub spp: Option<NonZeroU32>,
    /// `--seed`: the seed; 0 if neither it nor the sampler sets one.
    pub seed: Option<Seed>,
    /// `--outfile`: the output path, relative to the working directory.
    pub outfile: Option<PathBuf>,
}

/// A parsed Scene File: its Scene, its Render Job Options, where its image
/// goes, the content hash of the files it was read from, and its warnings.
#[derive(Debug)]
pub struct Parsed {
    /// The Scene.
    pub scene: Scene,
    /// The Render Job Options it sets, with the overrides applied; the
    /// Tile size and Pixel Samples per Pass are the defaults, as a Scene
    /// File never sets them.
    pub options: RenderJobOptions,
    /// The output path: `--outfile`, else the Film's `filename`, else
    /// `pbrt.exr`.
    pub output: PathBuf,
    /// The hash of the Scene File, its includes and its assets.
    pub content_hash: ContentHash,
    /// The warnings: parameters given but unused.
    pub warnings: Vec<Diagnostic>,
}

/// Reads the Scene File at `scene_file` with its includes, validates it
/// strictly against the supported subset, applies `overrides`, and builds
/// its Scene (ADR 0003).
///
/// # Errors
///
/// [`Error::Io`] if the Scene File cannot be read, and
/// [`Error::Invalid`] with every `Diagnostic` if anything in it is
/// unsupported or wrong; nothing is built then.
pub fn parse(scene_file: &Path, overrides: &Overrides) -> Result<Parsed, Error> {
    let mut files = SceneFiles::new(scene_file);
    let text = files.open(scene_file).map_err(|error| match error {
        Unread::Io(error) => Error::Io(error),
        Unread::Cycle => unreachable!("the first file opened is in no cycle"),
    })?;
    let mut target = SceneTarget::new();
    let mut diagnostics = Diagnostics::default();
    Parser {
        files: &mut files,
        target: &mut target,
        diagnostics: &mut diagnostics,
    }
    .parse(scene_file, &text);
    let built = target.finish(overrides, |name| files.resolve(name), &mut diagnostics);
    match built {
        Some(built) => Ok(Parsed {
            scene: built.scene,
            options: built.options,
            output: built.output,
            content_hash: files.content_hash(),
            warnings: diagnostics.into_vec(),
        }),
        None => Err(Error::Invalid(diagnostics.into_vec())),
    }
}
