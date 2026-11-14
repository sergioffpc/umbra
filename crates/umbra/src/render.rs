//! `umbra render`: parse, apply the overrides, render through
//! `coordinate`, write the image and its Render Job Report (arc42 §6.2).

use std::env;
use std::fs;
use std::io::Write;
use std::net::SocketAddr;
use std::path::Path;
use std::process::Command;
use std::time::Instant;

use tracing::info_span;
use umbra_cluster::{
    CoordinatorOptions, Interface, Liveness, Moment, Phase, RenderDevice, RenderJob,
    RenderJobOutcome, RenderJobReport, Status, coordinate,
};
use umbra_parser::{Overrides, ParseTimes, Parsed};
use umbra_render::{Cancellation, ReproductionRecord};
use umbra_scene::{RenderJobOptions, Seed};

use crate::build_identity::build_identity;
use crate::cli::RenderArgs;
use crate::error::Error;
use crate::footer::Footer;
use crate::status::StatusLine;

/// Renders the Scene File of `args`, writing its warnings to `stderr` and
/// its progress to the footer of `stderr` on a terminal, then the image,
/// and its Render Job Report to `stdout`; or, once `cancel` is made, the
/// report alone. Each Local Worker is this executable, run as `umbra
/// worker`, logging with the log filter `log_filter`.
pub(crate) fn render(
    args: &RenderArgs,
    log_filter: &str,
    cancel: &Cancellation,
    stdout: &mut dyn Write,
    mut stderr: &Footer,
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
        output: parsed.output.clone(),
        record,
    };
    let mut status_line = StatusLine::on(stderr);
    let outcome = coordinate_locally(job, args, log_filter, cancel, |status, moment| {
        status_line.show(status, moment);
    })?;
    // The last status stays on the terminal, above the errors and logs
    // that follow it.
    drop(status_line);
    finish(outcome, parsed.times, &parsed.output, stdout)
}

/// Renders `job` on the Local Workers of the Render Devices `args` chooses,
/// each in a child process that logs with the log filter `log_filter`, and
/// on the remote Workers of the Cluster `args` names.
fn coordinate_locally(
    job: RenderJob,
    args: &RenderArgs,
    log_filter: &str,
    cancel: &Cancellation,
    status: impl FnMut(&Status, Moment),
) -> Result<RenderJobOutcome, Error> {
    let devices = args.render_devices();
    let cpu = devices.includes_cpu().then_some(RenderDevice::Cpu);
    let gpus = devices.gpus()?.into_iter().map(RenderDevice::Gpu);
    let executable = env::current_exe()?;
    let command = |device: RenderDevice, coordinator: SocketAddr| {
        local_worker(&executable, log_filter, device, args, coordinator)
    };
    let options = CoordinatorOptions {
        local_workers: cpu.into_iter().chain(gpus).collect(),
        worker_command: &command,
        cluster: args.cluster.clone(),
        port: args.port,
        interface: args
            .interface
            .clone()
            .map_or(Interface::All, Interface::Named),
        dirty_builds: args.dirty_builds,
        liveness: Liveness::default(),
    };
    Ok(coordinate(job, &options, cancel, status)?)
}

/// The command line of the Local Worker of `device`: `executable`, run as
/// `umbra worker`, logging JSON lines with the log filter `log_filter`, with the
/// threads of `args` on the CPU, connecting to `coordinator`.
fn local_worker(
    executable: &Path,
    log_filter: &str,
    device: RenderDevice,
    args: &RenderArgs,
    coordinator: SocketAddr,
) -> Command {
    let mut command = Command::new(executable);
    command
        .args(["--log-filter", log_filter, "--log-format", "json", "worker"])
        .args(["--render-device", &device.to_string()])
        .args(["--coordinator", &coordinator.to_string()]);
    if let (RenderDevice::Cpu, Some(threads)) = (device, args.nthreads) {
        command.args(["--nthreads", &threads.to_string()]);
    }
    command
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
