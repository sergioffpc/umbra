//! `umbra render`: parse, apply the overrides, render through
//! `coordinate`, write the image and its Render Job Report (arc42 §6.2).

use std::ffi::OsString;
use std::fs;
use std::io::Write;
use std::path::{Path, PathBuf};

use umbra_cluster::{CoordinatorOptions, RenderJob, Threads, coordinate};
use umbra_parser::{Overrides, Parsed};
use umbra_render::ReproductionRecord;
use umbra_scene::{RenderJobOptions, Seed};

use crate::build_identity::build_identity;
use crate::cli::RenderArgs;
use crate::error::Error;

/// Renders the Scene File of `args`, writing its warnings and progress to
/// `stderr`, then the image and its Render Job Report.
pub(crate) fn render(args: &RenderArgs, stderr: &mut dyn Write) -> Result<(), Error> {
    let parsed = parse(args)?;
    for warning in &parsed.warnings {
        // Warnings are informative: a closed stderr does not fail the render.
        let _ = writeln!(stderr, "{warning}");
    }
    let options = RenderJobOptions {
        tile_size: args.tile_size,
        spp_per_pass: args.spp_per_pass,
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
    outcome.image.write(&parsed.output)?;
    outcome.report.write(&report_path(&parsed.output))?;
    Ok(())
}

/// The Scene File of `args`, parsed with its overrides.
fn parse(args: &RenderArgs) -> Result<Parsed, Error> {
    let overrides = Overrides {
        spp: args.spp,
        seed: args.seed.map(Seed),
        outfile: args.outfile.clone(),
    };
    umbra_parser::parse(&args.scene_file, &overrides).map_err(|error| match error {
        umbra_parser::Error::Invalid(diagnostics) => Error::Refused(diagnostics),
        umbra_parser::Error::Io(error) => Error::Unreadable(error),
    })
}

/// Where the Render Job Report of the image at `image` goes:
/// `<image>.report.json` (arc42 §8.10).
fn report_path(image: &Path) -> PathBuf {
    let mut path = OsString::from(image);
    path.push(".report.json");
    PathBuf::from(path)
}
