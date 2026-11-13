//! `coordinate`: a Render Job from its Scene to its image, through the
//! Local Worker (arc42 §5.3, §6.2).

use std::num::NonZeroUsize;
use std::path::PathBuf;
use std::thread;
use std::time::{Duration, Instant};

use rayon::{ThreadPool, ThreadPoolBuilder};
use tokio::runtime::Builder;
use tokio::sync::mpsc::UnboundedSender;
use tokio::time::{self, MissedTickBehavior};
use tracing::{info, info_span};
use umbra_render::{
    Acceptance, Cancellation, Decomposition, Film, FilmHash, Image, ReproductionRecord, WorkResult,
    WorkUnit,
};
use umbra_scene::{RenderJobOptions, Scene};

use crate::error::Error;
use crate::host::Host;
use crate::local_worker;
use crate::progress::Progress;
use crate::renderer::{RenderDevice, Renderer};
use crate::report::{Outcome, RenderJobReport, Rendered, Submission, WorkerFigures};
use crate::status::{Status, StatusLine};
use crate::transport::{CoordinatorEnd, ToCoordinator, ToWorker, connect};

/// How often the Coordinator checks for a cancellation and shows its
/// status.
const TICK: Duration = Duration::from_millis(250);

/// A Render Job: what is rendered, how it is cut into Work Units, where
/// its image goes, and the record of how to compute it again.
#[derive(Debug)]
pub struct RenderJob {
    /// The Scene.
    pub scene: Scene,
    /// The Render Job Options.
    pub options: RenderJobOptions,
    /// Where its final image goes: the caller writes it, and the report
    /// lists it with the options.
    pub output: PathBuf,
    /// Its Reproduction Record.
    pub record: ReproductionRecord,
}

/// How many threads a CPU Worker renders with.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum Threads {
    /// One per logical CPU.
    #[default]
    All,
    /// Exactly this many.
    Count(NonZeroUsize),
}

/// How the Coordinator runs a Render Job. So far it has only its Local
/// Worker on the CPU; remote Workers arrive with
/// [#129](https://github.com/sergioffpc/umbra/issues/129).
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct CoordinatorOptions {
    /// The Local Worker's threads.
    pub threads: Threads,
}

/// How a Render Job ended: finished, with its image and report, which the
/// caller writes, or cancelled, with its report alone (ADR 0004).
#[derive(Debug)]
pub enum RenderJobOutcome {
    /// Every Pass was merged.
    Finished {
        /// The final image.
        image: Image,
        /// The Render Job Report.
        report: RenderJobReport,
    },
    /// `cancel` stopped it before the end.
    Cancelled {
        /// The Render Job Report, with what was merged.
        report: RenderJobReport,
    },
}

/// Why the scheduling loop ended.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Ending {
    Finished,
    Cancelled,
}

/// Renders `job` to its image with the Local Worker, assigning Work Units
/// as it asks for them, Pass by Pass in raster Tile order (ADR 0009), and
/// merging each Work Result into the Film as it arrives (ADR 0022). It
/// blocks until the last Pass is merged or `cancel` is made; the runtime it
/// needs is its own. `status` receives the progress.
///
/// # Errors
///
/// [`Error`] if the Render Job cannot be cut into Work Units, the
/// runtime or the Local Worker cannot start, or the Local Worker goes away.
pub fn coordinate(
    job: RenderJob,
    options: &CoordinatorOptions,
    cancel: &Cancellation,
    status: StatusLine<'_>,
) -> Result<RenderJobOutcome, Error> {
    let RenderJob {
        scene,
        options: job_options,
        output,
        record,
    } = job;
    let decomposition = Decomposition::new(scene.full_resolution(), &job_options)?;
    // The Local Worker renders on the CPU.
    let renderer = Renderer::new(RenderDevice::Cpu, &scene, &job_options)?;
    let pool = thread_pool(options.threads)?;
    let threads = pool.current_num_threads();
    let mut scheduler = Scheduler {
        film: Film::new(&scene, &job_options)?,
        progress: Progress::new(&decomposition, Instant::now()),
        status: Status::new(status, threads),
        queue: decomposition.work_units(),
        demand: 0,
    };
    let ending = {
        let _span = info_span!("render").entered();
        let runtime = Builder::new_current_thread()
            .enable_time()
            .build()
            .map_err(Error::Runtime)?;
        let (coordinator, worker) = connect();
        thread::scope(|s| {
            s.spawn(|| local_worker::work(&renderer, &pool, worker));
            runtime.block_on(scheduler.run(coordinator, cancel))
        })?
    };
    Ok(scheduler.finish(
        ending,
        Submission {
            options: job_options,
            output,
            reproduction_record: record,
        },
    ))
}

/// The Local Worker's thread pool.
fn thread_pool(threads: Threads) -> Result<ThreadPool, Error> {
    let count = match threads {
        Threads::All => 0,
        Threads::Count(n) => n.get(),
    };
    ThreadPoolBuilder::new()
        .num_threads(count)
        .build()
        .map_err(Error::ThreadPool)
}

/// The Coordinator's side of a running Render Job: the Film it merges
/// into, the Work Units not assigned yet, and the progress it shows.
struct Scheduler<'d, 'w, Q> {
    film: Film,
    progress: Progress<'d>,
    status: Status<'w>,
    queue: Q,
    /// The Work Units the Local Worker asked for and was not given yet.
    demand: u32,
}

impl<Q: Iterator<Item = WorkUnit>> Scheduler<'_, '_, Q> {
    /// Assigns every Work Unit as the Worker asks, and merges each Work
    /// Result, until every one is merged or `cancel` is made.
    async fn run(
        &mut self,
        connection: CoordinatorEnd,
        cancel: &Cancellation,
    ) -> Result<Ending, Error> {
        let CoordinatorEnd {
            to_worker,
            mut from_worker,
        } = connection;
        let mut ticker = time::interval(TICK);
        ticker.set_missed_tick_behavior(MissedTickBehavior::Delay);
        while !self.film.is_complete() {
            tokio::select! {
                message = from_worker.recv() => {
                    self.receive(message.ok_or(Error::NoWorkers)?);
                    self.assign(&to_worker)?;
                }
                _ = ticker.tick() => {
                    if cancel.is_cancelled() {
                        // The Worker may already be gone.
                        let _ = to_worker.send(ToWorker::Cancel);
                        return Ok(Ending::Cancelled);
                    }
                    let now = Instant::now();
                    self.progress.tick(now);
                    self.status.show(&self.progress, now);
                }
            }
        }
        // The Worker may already be gone: it has nothing left to do.
        let _ = to_worker.send(ToWorker::JobFinished);
        Ok(Ending::Finished)
    }

    /// Takes in a message from the Local Worker.
    fn receive(&mut self, message: ToCoordinator) {
        match message {
            ToCoordinator::RequestWork(count) => self.demand = self.demand.saturating_add(count),
            ToCoordinator::WorkResult {
                result,
                render_time,
            } => self.merge(&result, render_time),
        }
    }

    /// Merges `result`, rendered in `render_time`, into the Film, unless
    /// its Work Unit is merged already.
    fn merge(&mut self, result: &WorkResult, render_time: Duration) {
        let start = Instant::now();
        let acceptance = info_span!("merge").in_scope(|| self.film.accept(result));
        let now = Instant::now();
        if acceptance == Acceptance::Merged {
            self.progress
                .merged(result.unit(), render_time, now - start, now);
        }
    }

    /// Assigns as many Work Units as the Local Worker asked for, while any
    /// is left.
    fn assign(&mut self, to_worker: &UnboundedSender<ToWorker>) -> Result<(), Error> {
        while self.demand > 0
            && let Some(unit) = self.queue.next()
        {
            to_worker
                .send(ToWorker::Assign(unit))
                .map_err(|_| Error::NoWorkers)?;
            self.progress.assigned(Instant::now());
            self.demand -= 1;
        }
        Ok(())
    }

    /// Shows the last status and makes the outcome of the Render Job
    /// submitted as `submission` says.
    fn finish(mut self, ending: Ending, submission: Submission) -> RenderJobOutcome {
        let now = Instant::now();
        self.progress.tick(now);
        self.status.finish(&self.progress, now);
        match ending {
            Ending::Finished => {
                let hash = self.film.hash();
                info!("Render Job finished, Film hash {hash}");
                let rendered = self.rendered(Outcome::Finished, Some(hash), now);
                RenderJobOutcome::Finished {
                    report: RenderJobReport::new(rendered, submission),
                    image: self.film.finish(),
                }
            }
            Ending::Cancelled => {
                let merged = self.progress.merged_samples();
                info!("Render Job cancelled, {merged} Pixel Samples merged");
                let rendered = self.rendered(Outcome::Cancelled, None, now);
                RenderJobOutcome::Cancelled {
                    report: RenderJobReport::new(rendered, submission),
                }
            }
        }
    }

    /// What the render phase produced, up to `now`.
    fn rendered(&self, outcome: Outcome, film_hash: Option<FilmHash>, now: Instant) -> Rendered {
        let render_time = self.progress.elapsed(now);
        let busy_time = self.progress.busy_time(now);
        Rendered {
            outcome,
            film_hash,
            render_time,
            merge_time: self.progress.merge_time(),
            local_worker: WorkerFigures {
                host: Host::this(),
                threads: self.status.threads(),
                work_units: self.progress.merged_units(),
                pixel_samples: self.progress.merged_samples(),
                busy_time,
                idle_time: render_time.saturating_sub(busy_time),
            },
            tile_costs: self.progress.tile_costs(),
        }
    }
}
