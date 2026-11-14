//! The Render Job Report (arc42 §8.10): plain text, one `Name: value` line
//! per figure, which `umbra` writes to stdout. The Scene and BVH
//! statistics arrive with the BVH
//! ([#79](https://github.com/sergioffpc/umbra/issues/79)), and the
//! Anomalous Pixel Sample count with
//! [#85](https://github.com/sergioffpc/umbra/issues/85).

use std::fmt::{self, Display};
use std::path::PathBuf;
use std::time::Duration;

use umbra_render::{FilmHash, ReproductionRecord};
use umbra_scene::RenderJobOptions;

use crate::host::Host;
use crate::status::Si;

/// How a Render Job ended.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Outcome {
    /// Every Pass was rendered and merged; the image exists.
    Finished,
    /// It was cancelled before the end; there is no image (ADR 0004).
    Cancelled,
}

impl Display for Outcome {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(match self {
            Self::Finished => "finished",
            Self::Cancelled => "cancelled",
        })
    }
}

/// A phase of a Render Job, timed on its own (arc42 §8.4).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Phase {
    /// Reading and checking the Scene File.
    Parse,
    /// Building the Scene.
    SceneBuild,
    /// Building the BVH.
    BvhBuild,
    /// Rendering every Work Unit, from the first assigned to the last
    /// merged.
    Render,
    /// Merging Work Results into the Film, summed over them.
    Merge,
    /// Writing the final image.
    ImageWrite,
}

impl Phase {
    /// Every phase, in the order they run.
    const ALL: [Self; 6] = [
        Self::Parse,
        Self::SceneBuild,
        Self::BvhBuild,
        Self::Render,
        Self::Merge,
        Self::ImageWrite,
    ];

    /// The phase's name in the report.
    fn name(self) -> &'static str {
        match self {
            Self::Parse => "parse",
            Self::SceneBuild => "Scene build",
            Self::BvhBuild => "BVH build",
            Self::Render => "render",
            Self::Merge => "merge",
            Self::ImageWrite => "image write",
        }
    }

    /// The phase's index in [`Phase::ALL`].
    fn index(self) -> usize {
        self as usize
    }
}

/// What the Local Worker did, and on what machine.
#[derive(Debug, Clone, PartialEq)]
pub(crate) struct WorkerFigures {
    /// The machine it ran on.
    pub(crate) host: Host,
    /// Its threads.
    pub(crate) threads: usize,
    /// The Work Units whose Work Results were merged.
    pub(crate) work_units: u64,
    /// The Pixel Samples of those.
    pub(crate) pixel_samples: u64,
    /// The time it had a Work Unit in flight.
    pub(crate) busy_time: Duration,
    /// The rest of the render phase.
    pub(crate) idle_time: Duration,
    /// How much of the render phase its threads spent rendering.
    pub(crate) occupancy: Occupancy,
}

/// The mean thread occupancy of a Worker: the share of the render phase
/// each of its threads spent rendering rows, averaged over them, so the
/// time they spent rendering over its threads times the render phase. The
/// time of a Work Unit in flight when the Render Job is cancelled is left
/// out.
#[derive(Debug, Clone, Copy, PartialEq)]
pub(crate) struct Occupancy(f64);

impl Occupancy {
    /// The occupancy of `threads` threads that spent `thread_time`
    /// rendering, summed over them, in a render phase of `render_time`.
    pub(crate) fn new(thread_time: Duration, threads: usize, render_time: Duration) -> Self {
        let capacity = render_time.as_secs_f64() * to_f64(threads as u64);
        Self(if capacity > 0.0 {
            thread_time.as_secs_f64() / capacity
        } else {
            0.0
        })
    }
}

impl Display for Occupancy {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{:.1} %", self.0 * 100.0)
    }
}

/// What the render phase of a Render Job produced, for its report.
#[derive(Debug, Clone, PartialEq)]
pub(crate) struct Rendered {
    pub(crate) outcome: Outcome,
    pub(crate) film_hash: Option<FilmHash>,
    pub(crate) render_time: Duration,
    pub(crate) merge_time: Duration,
    pub(crate) local_worker: WorkerFigures,
    pub(crate) tile_costs: Vec<f64>,
}

/// What a Render Job was submitted with, for its report.
#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct Submission {
    pub(crate) options: RenderJobOptions,
    /// Where its final image goes.
    pub(crate) output: PathBuf,
    pub(crate) reproduction_record: ReproductionRecord,
}

/// What a Render Job leaves behind besides its image, finished or
/// cancelled. Its [`Display`] is the text `umbra` writes to stdout:
///
/// ```text
/// Render Job: finished
/// Options: seed 0, spp 4, spp per pass 2, tile size 8, output black.exr
/// Film hash: a5bab1a6c36d4cf5
/// Pixel Samples: 3840 merged, 2.00 M per second
/// Phase times: parse 0.000 s, Scene build 0.000 s, render 0.002 s, merge 0.000 s, image write 0.003 s
/// Local Worker: host p620, CPU AMD Ryzen Threadripper PRO 3975WX 32-Cores, 12 threads, 30 Work Units, 3840 Pixel Samples, busy 0.002 s, idle 0.000 s, occupancy 83.4 %
/// Tile costs: min 68.9, mean 247.4, max 1603.3 (Tile 1)
/// Reproduction Record: version 1, scene file /scenes/a.pbrt, content hash 9505…, seed 0, …
/// ```
///
/// A phase that did not run (one after a cancellation, or the BVH build
/// of a Scene without one) is left out, and so is the Film hash of a
/// cancelled Render Job.
#[derive(Debug, Clone, PartialEq)]
pub struct RenderJobReport {
    outcome: Outcome,
    options: RenderJobOptions,
    output: PathBuf,
    film_hash: Option<FilmHash>,
    phase_times: [Option<Duration>; Phase::ALL.len()],
    render_time: Duration,
    local_worker: WorkerFigures,
    /// Each Tile's render time over the Worker's mean time per Pixel
    /// Sample, summed over its Passes, in raster order: its cost in Pixel
    /// Samples at the Worker's mean speed.
    tile_costs: Vec<f64>,
    reproduction_record: ReproductionRecord,
}

impl RenderJobReport {
    /// The report of what `rendered` says, of a Render Job submitted as
    /// `submission` says.
    pub(crate) fn new(rendered: Rendered, submission: Submission) -> Self {
        let mut report = Self {
            outcome: rendered.outcome,
            options: submission.options,
            output: submission.output,
            film_hash: rendered.film_hash,
            phase_times: [None; Phase::ALL.len()],
            render_time: rendered.render_time,
            local_worker: rendered.local_worker,
            tile_costs: rendered.tile_costs,
            reproduction_record: submission.reproduction_record,
        };
        report.record_phase(Phase::Render, rendered.render_time);
        report.record_phase(Phase::Merge, rendered.merge_time);
        report
    }

    /// How the Render Job ended.
    #[must_use]
    pub fn outcome(&self) -> Outcome {
        self.outcome
    }

    /// The Film hash, when the Render Job finished.
    #[must_use]
    pub fn film_hash(&self) -> Option<FilmHash> {
        self.film_hash
    }

    /// Records that `phase` took `time`: the phases run outside
    /// `coordinate` are recorded by its caller.
    pub fn record_phase(&mut self, phase: Phase, time: Duration) {
        self.phase_times[phase.index()] = Some(time);
    }

    /// The Pixel Samples merged per second of the render phase.
    fn pixel_samples_per_second(&self) -> f64 {
        let seconds = self.render_time.as_secs_f64();
        if seconds > 0.0 {
            to_f64(self.local_worker.pixel_samples) / seconds
        } else {
            0.0
        }
    }
}

impl Display for RenderJobReport {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        writeln!(f, "Render Job: {}", self.outcome)?;
        let RenderJobOptions {
            seed,
            tile_size,
            spp_per_pass,
            spp,
        } = self.options;
        writeln!(
            f,
            "Options: seed {}, spp {spp}, spp per pass {spp_per_pass}, tile size {tile_size}, output {}",
            seed.0,
            self.output.display(),
        )?;
        if let Some(hash) = self.film_hash {
            writeln!(f, "Film hash: {hash}")?;
        }
        let rate = Si(self.pixel_samples_per_second());
        let samples = self.local_worker.pixel_samples;
        writeln!(f, "Pixel Samples: {samples} merged, {rate} per second")?;
        writeln!(f, "Phase times: {}", PhaseTimes(&self.phase_times))?;
        writeln!(f, "Local Worker: {}", self.local_worker)?;
        writeln!(f, "Tile costs: {}", TileCosts(&self.tile_costs))?;
        writeln!(f, "Reproduction Record: {}", self.reproduction_record)
    }
}

impl Display for WorkerFigures {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "{}, {} threads, {} Work Units, {} Pixel Samples, busy {}, idle {}, occupancy {}",
            self.host,
            self.threads,
            self.work_units,
            self.pixel_samples,
            Seconds(self.busy_time),
            Seconds(self.idle_time),
            self.occupancy,
        )
    }
}

/// The phases that ran, each with its time.
struct PhaseTimes<'a>(&'a [Option<Duration>; Phase::ALL.len()]);

impl Display for PhaseTimes<'_> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let ran = Phase::ALL
            .into_iter()
            .filter_map(|phase| self.0[phase.index()].map(|time| (phase, time)));
        let separators = std::iter::once("").chain(std::iter::repeat(", "));
        separators
            .zip(ran)
            .try_for_each(|(separator, (phase, time))| {
                write!(f, "{separator}{} {}", phase.name(), Seconds(time))
            })
    }
}

/// The least, mean and greatest Tile cost, and the costliest Tile.
struct TileCosts<'a>(&'a [f64]);

impl Display for TileCosts<'_> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let costliest = self
            .0
            .iter()
            .enumerate()
            .max_by(|(_, a), (_, b)| a.total_cmp(b));
        let Some((tile, &max)) = costliest else {
            return f.write_str("none merged");
        };
        let min = self.0.iter().copied().fold(f64::INFINITY, f64::min);
        let mean = self.0.iter().sum::<f64>() / to_f64(self.0.len() as u64);
        write!(
            f,
            "min {min:.1}, mean {mean:.1}, max {max:.1} (Tile {tile})"
        )
    }
}

/// A duration in seconds, to the millisecond.
struct Seconds(Duration);

impl Display for Seconds {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{:.3} s", self.0.as_secs_f64())
    }
}

/// `n` as an `f64`, for rates and means.
#[expect(
    clippy::cast_precision_loss,
    reason = "rates and means, not exact counts"
)]
fn to_f64(n: u64) -> f64 {
    n as f64
}
