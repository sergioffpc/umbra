//! `umbra render`: parse, apply the overrides, render through
//! `coordinate`, write the image and its Render Job Report (arc42 §6.2).

use std::ffi::OsString;
use std::fmt::{self, Display};
use std::fs;
use std::io::{self, Write};
use std::path::{Path, PathBuf};

use thiserror::Error;
use umbra_cluster::{ClusterError, CoordinatorOptions, RenderJob, Threads, coordinate};
use umbra_parser::{Overrides, ParseError, Parsed, ParserError};
use umbra_render::{ExrWriter, ImageWriter, ReproductionRecord, write_atomically};
use umbra_scene::{RenderJobOptions, Seed};

use crate::build_identity::build_identity;
use crate::cli::RenderArgs;

/// Why `umbra render` did not write an image.
#[derive(Debug, Error)]
pub(crate) enum RenderFailure {
    /// The Scene File is invalid: every `ParserError`.
    #[error("{}", lines(.0))]
    Refused(Vec<ParserError>),
    /// The Scene File could not be read.
    #[error("error: cannot read the Scene File: {0}")]
    Unreadable(#[source] io::Error),
    /// A file could not be resolved or written.
    #[error("error: {0}")]
    Io(#[from] io::Error),
    /// The Render Job did not finish.
    #[error("error: {0}")]
    Cluster(#[from] ClusterError),
}

/// `diagnostics`, one per line.
fn lines(diagnostics: &[ParserError]) -> impl Display {
    fmt::from_fn(move |f| {
        diagnostics
            .iter()
            .enumerate()
            .try_for_each(|(i, d)| match i {
                0 => write!(f, "{d}"),
                _ => write!(f, "\n{d}"),
            })
    })
}

/// Renders the Scene File of `args`, writing its warnings and progress to
/// `stderr`, then the image and its Render Job Report.
pub(crate) fn render(args: &RenderArgs, stderr: &mut dyn Write) -> Result<(), RenderFailure> {
    let parsed = parse(args)?;
    for warning in &parsed.warnings {
        // Warnings are informative: a closed stderr does not fail the render.
        let _ = writeln!(stderr, "{warning}");
    }
    let options = RenderJobOptions {
        tile_size: args.tile_size,
        pixel_samples_per_pass: args.pixel_samples_per_pass,
        ..parsed.options
    };
    let record = ReproductionRecord::new(
        fs::canonicalize(&args.scene_file)?,
        parsed.content_hash,
        &options,
        build_identity(),
    );
    let job = RenderJob {
        scene: parsed.scene,
        options,
        record,
    };
    let threads = args.nthreads.map_or(Threads::All, Threads::Count);
    let outcome = coordinate(job, &CoordinatorOptions { threads }, stderr)?;
    ExrWriter.write(&outcome.image, &parsed.output)?;
    let json = outcome.report.to_json();
    write_atomically(&report_path(&parsed.output), |file| {
        writeln!(file, "{json}")
    })?;
    Ok(())
}

/// The Scene File of `args`, parsed with its overrides.
fn parse(args: &RenderArgs) -> Result<Parsed, RenderFailure> {
    let overrides = Overrides {
        spp: args.spp,
        seed: args.seed.map(Seed),
        outfile: args.outfile.clone(),
    };
    umbra_parser::parse(&args.scene_file, &overrides).map_err(|error| match error {
        ParseError::Invalid(diagnostics) => RenderFailure::Refused(diagnostics),
        ParseError::Io(error) => RenderFailure::Unreadable(error),
    })
}

/// Where the Render Job Report of the image at `image` goes:
/// `<image>.report.json` (arc42 §8.10).
fn report_path(image: &Path) -> PathBuf {
    let mut path = OsString::from(image);
    path.push(".report.json");
    PathBuf::from(path)
}
