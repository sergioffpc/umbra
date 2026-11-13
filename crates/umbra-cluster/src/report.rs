//! The Render Job Report (arc42 §8.10): Umbra's own JSON, versioned
//! `major.minor`, with a JSON Schema generated from these types and
//! committed as `render-job-report.schema.json`. The Scene and BVH
//! statistics arrive with the BVH, and the Anomalous Pixel Sample count
//! with [#85](https://github.com/sergioffpc/umbra/issues/85).

use std::io::{self, Write};
use std::path::Path;
use std::time::Duration;

use schemars::JsonSchema;
use serde::{Serialize, Serializer};
use umbra_render::{FilmHash, ReproductionRecord, write_atomically};
use umbra_scene::RenderJobOptions;

/// How a Render Job ended.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, JsonSchema)]
#[serde(rename_all = "lowercase")]
pub enum Outcome {
    /// Every Pass was rendered and merged; the image exists.
    Finished,
    /// It was cancelled before the end; there is no image (ADR 0004).
    Cancelled,
}

/// The version of the Render Job Report's schema, written as
/// `{"major": 0, "minor": 2}`. Reports of one major version can be
/// compared; see [`RenderJobReport::SCHEMA_VERSION`].
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, JsonSchema)]
pub struct SchemaVersion {
    /// Changes with the meaning of a comparable field.
    pub major: u32,
    /// Changes with any other field.
    pub minor: u32,
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

/// How long each phase took, in seconds; `null` for a phase that did not
/// run: one after a cancellation, or the BVH build of a Scene without one.
#[derive(Debug, Clone, Copy, PartialEq, Default, Serialize, JsonSchema)]
struct PhaseTimes {
    #[serde(serialize_with = "seconds")]
    #[schemars(with = "Option<f64>")]
    parse: Option<Duration>,
    #[serde(serialize_with = "seconds")]
    #[schemars(with = "Option<f64>")]
    scene_build: Option<Duration>,
    #[serde(serialize_with = "seconds")]
    #[schemars(with = "Option<f64>")]
    bvh_build: Option<Duration>,
    #[serde(serialize_with = "seconds")]
    #[schemars(with = "Option<f64>")]
    render: Option<Duration>,
    #[serde(serialize_with = "seconds")]
    #[schemars(with = "Option<f64>")]
    merge: Option<Duration>,
    #[serde(serialize_with = "seconds")]
    #[schemars(with = "Option<f64>")]
    image_write: Option<Duration>,
}

impl PhaseTimes {
    /// The time of `phase`, as stored.
    fn slot(&mut self, phase: Phase) -> &mut Option<Duration> {
        match phase {
            Phase::Parse => &mut self.parse,
            Phase::SceneBuild => &mut self.scene_build,
            Phase::BvhBuild => &mut self.bvh_build,
            Phase::Render => &mut self.render,
            Phase::Merge => &mut self.merge,
            Phase::ImageWrite => &mut self.image_write,
        }
    }
}

/// The Render Job Options and the Coordinator options it ran with.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, JsonSchema)]
pub(crate) struct ReportOptions {
    seed: i32,
    spp: u32,
    spp_per_pass: u32,
    tile_size: u32,
    /// The Local Worker's threads.
    threads: usize,
}

impl ReportOptions {
    /// `options`, with the Local Worker on `threads`.
    pub(crate) fn new(options: &RenderJobOptions, threads: usize) -> Self {
        Self {
            seed: options.seed.0,
            spp: options.spp.get(),
            spp_per_pass: options.spp_per_pass.get(),
            tile_size: options.tile_size.get(),
            threads,
        }
    }
}

/// What the Local Worker did.
#[derive(Debug, Clone, Copy, PartialEq, Serialize, JsonSchema)]
pub(crate) struct WorkerFigures {
    /// Its threads.
    pub(crate) threads: usize,
    /// The Work Units whose Work Results were merged.
    pub(crate) work_units: u64,
    /// The Pixel Samples of those.
    pub(crate) pixel_samples: u64,
    /// The time it had a Work Unit in flight, in seconds.
    #[serde(serialize_with = "seconds")]
    #[schemars(with = "f64")]
    pub(crate) busy_time: Duration,
    /// The rest of the render phase, in seconds.
    #[serde(serialize_with = "seconds")]
    #[schemars(with = "f64")]
    pub(crate) idle_time: Duration,
}

/// What the render phase of a Render Job produced, for its report.
#[derive(Debug, Clone, PartialEq)]
pub(crate) struct Rendered {
    pub(crate) outcome: Outcome,
    pub(crate) film_hash: Option<FilmHash>,
    pub(crate) options: ReportOptions,
    pub(crate) render_time: Duration,
    pub(crate) merge_time: Duration,
    pub(crate) local_worker: WorkerFigures,
    pub(crate) tile_costs: Vec<f64>,
}

/// What a Render Job leaves behind besides its image, written next to it
/// as `<image>.report.json`, finished or cancelled.
#[derive(Debug, Clone, PartialEq, Serialize, JsonSchema)]
pub struct RenderJobReport {
    schema_version: SchemaVersion,
    /// How the Render Job ended.
    outcome: Outcome,
    /// The Film hash, when it finished.
    #[serde(serialize_with = "hex")]
    #[schemars(with = "Option<String>")]
    film_hash: Option<FilmHash>,
    /// The options it ran with.
    options: ReportOptions,
    /// How long each phase took.
    phase_times: PhaseTimes,
    /// The Pixel Samples merged into the Film.
    pixel_samples: u64,
    /// The Pixel Samples merged per second of the render phase: comparable.
    pixel_samples_per_second: f64,
    /// What the Local Worker did.
    local_worker: WorkerFigures,
    /// Each Tile's render time over the Worker's mean time per Pixel
    /// Sample, summed over its Passes, in raster order: its cost in Pixel
    /// Samples at the Worker's mean speed.
    tile_costs: Vec<f64>,
    /// How to compute any of its Pixel Samples again (arc42 §8.9).
    reproduction_record: ReproductionRecord,
}

impl RenderJobReport {
    /// The schema version: the major part changes with the meaning of a
    /// comparable field, the minor part with any other change.
    pub const SCHEMA_VERSION: SchemaVersion = SchemaVersion { major: 0, minor: 2 };

    /// The report of what `rendered` says, made as `reproduction_record`
    /// says.
    pub(crate) fn new(rendered: Rendered, reproduction_record: ReproductionRecord) -> Self {
        let pixel_samples = rendered.local_worker.pixel_samples;
        let render_time = rendered.render_time.as_secs_f64();
        let mut report = Self {
            schema_version: Self::SCHEMA_VERSION,
            outcome: rendered.outcome,
            film_hash: rendered.film_hash,
            options: rendered.options,
            phase_times: PhaseTimes::default(),
            pixel_samples,
            pixel_samples_per_second: rate(pixel_samples, render_time),
            local_worker: rendered.local_worker,
            tile_costs: rendered.tile_costs,
            reproduction_record,
        };
        report.record_phase(Phase::Render, rendered.render_time);
        report.record_phase(Phase::Merge, rendered.merge_time);
        report
    }

    /// The JSON Schema of the report.
    #[must_use]
    pub fn json_schema() -> schemars::Schema {
        schemars::schema_for!(Self)
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
        *self.phase_times.slot(phase) = Some(time);
    }

    /// Writes the report to `path` as indented JSON, its fields in a fixed
    /// order, replacing any file there.
    ///
    /// # Errors
    ///
    /// Any I/O error; `path` is then left as it was.
    pub fn write(&self, path: &Path) -> io::Result<()> {
        write_atomically(path, |file| {
            serde_json::to_writer_pretty(&mut *file, self)?;
            writeln!(file)
        })
    }
}

/// `samples` over `seconds`, or 0 over no time.
#[expect(clippy::cast_precision_loss, reason = "a rate, not an exact count")]
fn rate(samples: u64, seconds: f64) -> f64 {
    if seconds > 0.0 {
        samples as f64 / seconds
    } else {
        0.0
    }
}

/// Serialises a Film hash as its hexadecimal digits, or `null`.
#[expect(
    clippy::ref_option,
    reason = "`serialize_with` passes a reference to the field"
)]
fn hex<S: Serializer>(hash: &Option<FilmHash>, serializer: S) -> Result<S::Ok, S::Error> {
    match hash {
        Some(hash) => serializer.collect_str(hash),
        None => serializer.serialize_none(),
    }
}

/// Serialises a duration as its seconds, or `null`.
fn seconds<S: Serializer, T: Seconds>(time: &T, serializer: S) -> Result<S::Ok, S::Error> {
    match time.seconds() {
        Some(seconds) => serializer.serialize_f64(seconds),
        None => serializer.serialize_none(),
    }
}

/// A duration, or a duration that may be missing, as seconds.
trait Seconds {
    /// The seconds, if any.
    fn seconds(&self) -> Option<f64>;
}

impl Seconds for Duration {
    fn seconds(&self) -> Option<f64> {
        Some(self.as_secs_f64())
    }
}

impl Seconds for Option<Duration> {
    fn seconds(&self) -> Option<f64> {
        self.map(|time| time.as_secs_f64())
    }
}
