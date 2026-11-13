//! `umbra render`: parse, apply the overrides, render through
//! `coordinate`, write the image and its Render Job Report (arc42 §6.2).

use std::fs;
use std::io::{self, IsTerminal, Write};
use std::path::Path;
use std::time::Instant;

use tracing::info_span;
use umbra_cluster::{
    CoordinatorOptions, Phase, RenderJob, RenderJobOutcome, RenderJobReport, StatusLine, Threads,
    coordinate,
};
use umbra_parser::{Overrides, ParseTimes, Parsed};
use umbra_render::{Cancellation, ReproductionRecord};
use umbra_scene::{RenderJobOptions, Seed};

use crate::build_identity::build_identity;
use crate::cli::RenderArgs;
use crate::error::Error;

/// Renders the Scene File of `args`, writing its warnings and progress to
/// `stderr`, then the image, and its Render Job Report to `stdout`; or,
/// once `cancel` is made, the report alone.
pub(crate) fn render(
    args: &RenderArgs,
    cancel: &Cancellation,
    stdout: &mut dyn Write,
    stderr: &mut dyn Write,
) -> Result<(), Error> {
    let _span = info_span!("render_job").entered();
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
    let status = if io::stderr().is_terminal() {
        StatusLine::Redrawn(stderr)
    } else {
        StatusLine::Logged
    };
    let outcome = coordinate(job, &CoordinatorOptions { threads }, cancel, status)?;
    finish(outcome, parsed.times, &parsed.output, stdout)
}

/// Writes the image of `outcome` to `image`, then its report to `stdout`,
/// with the times of parsing, `times`, and of writing the image; or the
/// report alone, for a cancelled Render Job.
fn finish(
    outcome: RenderJobOutcome,
    times: ParseTimes,
    image: &Path,
    stdout: &mut dyn Write,
) -> Result<(), Error> {
    let with_parse_times = |mut report: RenderJobReport| {
        report.record_phase(Phase::Parse, times.parse);
        report.record_phase(Phase::SceneBuild, times.scene_build);
        report
    };
    match outcome {
        RenderJobOutcome::Finished {
            image: pixels,
            report,
        } => {
            let mut report = with_parse_times(report);
            let start = Instant::now();
            info_span!("image_write").in_scope(|| pixels.write(image))?;
            report.record_phase(Phase::ImageWrite, start.elapsed());
            write!(stdout, "{report}")?;
            Ok(())
        }
        RenderJobOutcome::Cancelled { report } => {
            write!(stdout, "{}", with_parse_times(report))?;
            Err(Error::Cancelled)
        }
    }
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
