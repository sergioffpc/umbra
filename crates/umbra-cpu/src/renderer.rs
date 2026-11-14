//! The CPU's Renderer: everything between a Work Unit and its Work Result
//! on the cores of one NUMA node (arc42 §5.3).

use std::fmt;
use std::ops::Range;
use std::time::{Duration, Instant};

use rayon::prelude::*;
use umbra_integrators::{
    NoRecorder, PixelSampleId, PixelSampleState, RayIntegrator, SceneView, evaluate_pixel_sample,
};
use umbra_math::Point2i;
use umbra_render::{
    Cancellation, Decomposition, Error, Meter, PixelSum, Rendered, WorkResult, WorkUnit,
};
use umbra_sampling::{
    DigitPermutations, HaltonRandomizer, HaltonSampler, RandomizeStrategy, SamplerKind,
};
use umbra_scene::{RenderJobOptions, Scene};

/// A render stopped by its [`Cancellation`].
#[derive(Debug)]
struct Cancelled;

/// A render of a Work Unit: how it ended, and how long the pool's threads
/// spent on it.
#[derive(Debug, Clone, PartialEq)]
pub struct Render {
    /// How it ended.
    pub rendered: Rendered,
    /// The time spent rendering its rows, summed over the threads that
    /// rendered them: its share of the Worker's thread occupancy (arc42
    /// §8.10). A cancelled render counts the rows it rendered.
    pub thread_time: Duration,
}

/// What a render reads, shared by every thread rendering a Work Unit.
struct Job<'a> {
    view: SceneView<'a>,
    sampler: SamplerKind<'a>,
    decomposition: &'a Decomposition,
    cancel: &'a Cancellation,
    meter: &'a Meter,
}

/// The coarse seam between the Renderer and an integrator, called once per
/// Work Unit, so it may be `dyn` (ADR 0006): the per-Pixel-Sample body
/// behind it is generic and statically dispatched.
trait Integrator: Sync + fmt::Debug {
    /// Renders `unit`, unless the job's cancellation stops it.
    fn render(&self, job: &Job<'_>, unit: WorkUnit) -> Render;
}

/// The CPU's driver of a ray integrator: the Tile's rows in parallel, each
/// pixel's Pixel Samples in ascending sample index (ADR 0022).
#[derive(Debug)]
struct PixelLoop<I>(I);

impl<I: RayIntegrator + Sync + fmt::Debug> Integrator for PixelLoop<I> {
    fn render(&self, job: &Job<'_>, unit: WorkUnit) -> Render {
        let bounds = job.decomposition.tile_bounds(unit.tile);
        let samples = job.decomposition.sample_indices(unit.pass);
        let mut pixels = vec![PixelSum::default(); bounds.area()];
        let rows = pixels
            .par_chunks_mut(bounds.width())
            .zip(bounds.min.y..bounds.max.y);
        // Every row runs: once cancelled, each returns at its first check.
        let (thread_time, outcome) = rows
            .map(|(row, y)| {
                let start = Instant::now();
                job.meter.row_started();
                let outcome = self.render_row(job, row, Point2i::new(bounds.min.x, y), &samples);
                let done = match outcome {
                    Ok(()) => (row.len() * samples.len()) as u64,
                    Err(Cancelled) => 0,
                };
                job.meter.row_finished(done);
                (start.elapsed(), outcome)
            })
            .reduce(
                || (Duration::ZERO, Ok(())),
                |(a, first), (b, second)| (a + b, first.and(second)),
            );
        let rendered = match outcome {
            Ok(()) => Rendered::Done(WorkResult::new(unit, pixels)),
            Err(Cancelled) => Rendered::Cancelled,
        };
        Render {
            rendered,
            thread_time,
        }
    }
}

impl<I: RayIntegrator> PixelLoop<I> {
    /// The sums of the pixels of `row`, the first at `start`, over the
    /// sample indices `samples`; nothing if the render is cancelled first.
    fn render_row(
        &self,
        job: &Job<'_>,
        row: &mut [PixelSum],
        start: Point2i,
        samples: &Range<u32>,
    ) -> Result<(), Cancelled> {
        if job.cancel.is_cancelled() {
            return Err(Cancelled);
        }
        let mut state = PixelSampleState {
            sampler: job.sampler.clone(),
            recorder: NoRecorder,
        };
        for (sum, x) in row.iter_mut().zip(start.x..) {
            *sum = self.render_pixel(job, &mut state, Point2i::new(x, start.y), samples);
        }
        Ok(())
    }

    /// The sum of the Pixel Samples `samples` of `pixel`, in ascending
    /// sample index (ADR 0022).
    fn render_pixel(
        &self,
        job: &Job<'_>,
        state: &mut PixelSampleState<'_, NoRecorder>,
        pixel: Point2i,
        samples: &Range<u32>,
    ) -> PixelSum {
        samples
            .clone()
            .fold(PixelSum::default(), |mut sum, sample_index| {
                let id = PixelSampleId {
                    pixel,
                    sample_index,
                };
                sum += evaluate_pixel_sample(&self.0, &job.view, id, state);
                sum
            })
    }
}

/// What the sampler randomises with, owned for the Render Job: the digit
/// permutations of its seed, when it permutes digits.
#[derive(Debug)]
enum Randomization {
    None,
    PermuteDigits(DigitPermutations<Vec<u16>>),
    Owen,
}

impl Randomization {
    /// The randomization `strategy` names, for `seed`.
    fn new(strategy: RandomizeStrategy, seed: u32) -> Self {
        match strategy {
            RandomizeStrategy::None => Self::None,
            RandomizeStrategy::PermuteDigits => {
                Self::PermuteDigits(DigitPermutations::new_in(seed, |n| vec![0; n]))
            }
            RandomizeStrategy::Owen => Self::Owen,
        }
    }

    /// The same, borrowed, as the sampler holds it.
    fn randomizer(&self) -> HaltonRandomizer<'_> {
        match self {
            Self::None => HaltonRandomizer::None,
            Self::PermuteDigits(p) => HaltonRandomizer::PermuteDigits(p.view()),
            Self::Owen => HaltonRandomizer::Owen,
        }
    }
}

/// Renders the Work Units of a Render Job of one Scene into Work Results on
/// the CPU, in this process (arc42 §5.3). `render` may run for several Work Units at
/// once, from any threads; each render splits its rows over rayon's pool.
#[derive(Debug)]
pub struct Renderer<'s> {
    scene: &'s Scene,
    decomposition: Decomposition,
    samples_per_pixel: u32,
    randomization: Randomization,
    integrator: Box<dyn Integrator + 's>,
}

impl<'s> Renderer<'s> {
    /// The Renderer of a Render Job of `scene` with `options`. Computes the
    /// sampler's digit permutations for the seed, if it permutes digits.
    ///
    /// # Errors
    ///
    /// [`Error::TooManyTiles`] if the Tiles cannot be counted in a
    /// `u32`.
    pub fn new(scene: &'s Scene, options: &RenderJobOptions) -> Result<Self, Error> {
        Ok(Self {
            scene,
            decomposition: Decomposition::new(scene.full_resolution(), options)?,
            samples_per_pixel: options.spp.get(),
            randomization: Randomization::new(scene.randomize_strategy(), options.seed.into()),
            integrator: Box::new(PixelLoop(scene.integrator())),
        })
    }

    /// How the Render Job is cut into Work Units.
    #[must_use]
    pub fn decomposition(&self) -> &Decomposition {
        &self.decomposition
    }

    /// Renders `unit` into its Work Result, unless `cancel` stops it first,
    /// counting its progress on `meter` as it goes, and measures the
    /// threads' time on it.
    ///
    /// # Panics
    ///
    /// If `unit` is not one of this Render Job's.
    pub fn render(&self, unit: WorkUnit, cancel: &Cancellation, meter: &Meter) -> Render {
        assert!(
            self.decomposition.contains(unit),
            "{unit:?} is not in this Render Job"
        );
        let job = Job {
            view: self.scene.view(),
            sampler: self.sampler(),
            decomposition: &self.decomposition,
            cancel,
            meter,
        };
        self.integrator.render(&job, unit)
    }

    /// The Render Job's sampler, before any Pixel Sample starts.
    fn sampler(&self) -> SamplerKind<'_> {
        let resolution = self.scene.full_resolution();
        let randomizer = self.randomization.randomizer();
        HaltonSampler::new(self.samples_per_pixel, resolution, randomizer).into()
    }
}
