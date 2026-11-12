//! `coordinate`: a Render Job from its Scene to its image, through the
//! Local Worker (arc42 §5.3, §6.2).

use std::io;
use std::num::NonZeroUsize;
use std::thread;

use rayon::{ThreadPool, ThreadPoolBuilder};
use tokio::runtime::Builder;
use umbra_render::{Decomposition, Film, Image, Renderer, ReproductionRecord};
use umbra_scene::{RenderJobOptions, Scene};

use crate::error::Error;
use crate::local_worker;
use crate::report::RenderJobReport;
use crate::transport::{CoordinatorEnd, ToCoordinator, ToWorker, connect};

/// A Render Job: what is rendered, how it is cut into Work Units, and the
/// record of how to compute it again.
#[derive(Debug)]
pub struct RenderJob {
    /// The Scene.
    pub scene: Scene,
    /// The Render Job Options.
    pub options: RenderJobOptions,
    /// The Reproduction Record the image carries.
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

/// What a finished Render Job produces: the image, and its report, which
/// the caller writes.
#[derive(Debug)]
pub struct RenderJobOutcome {
    /// The final image, every Pass merged.
    pub image: Image,
    /// The Render Job Report.
    pub report: RenderJobReport,
}

/// Renders `job` to its image with the Local Worker, assigning Work Units
/// as it asks for them, Pass by Pass in raster Tile order (ADR 0009), and
/// merging each Work Result into the Film as it arrives (ADR 0022). It
/// blocks until the last Pass is merged; the runtime it needs is its own.
/// `status` receives the progress.
///
/// # Errors
///
/// [`Error`] if the Render Job cannot be cut into Work Units, the
/// runtime or the Local Worker cannot start, or the Local Worker goes away.
pub fn coordinate(
    job: RenderJob,
    options: &CoordinatorOptions,
    status: &mut dyn io::Write,
) -> Result<RenderJobOutcome, Error> {
    let RenderJob {
        scene,
        options: job_options,
        record,
    } = job;
    let renderer = Renderer::new(&scene, &job_options)?;
    let mut film = Film::new(&scene, &job_options)?;
    let pool = thread_pool(options.threads)?;
    let runtime = Builder::new_current_thread()
        .build()
        .map_err(Error::Runtime)?;
    let (coordinator, worker) = connect();
    thread::scope(|s| {
        s.spawn(|| local_worker::work(&renderer, &pool, worker));
        runtime.block_on(schedule(renderer.decomposition(), &mut film, coordinator))
    })?;
    let film_hash = film.hash();
    // The status line is informative: a closed stderr does not fail the
    // Render Job.
    let _ = writeln!(
        status,
        "rendered {} Work Units, Film hash {film_hash}",
        renderer.decomposition().work_units().count()
    );
    Ok(RenderJobOutcome {
        image: film.finish(record),
        report: RenderJobReport::finished(film_hash),
    })
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

/// Assigns every Work Unit of `decomposition` as the Worker asks, and
/// merges each Work Result into `film`, until every one is merged.
async fn schedule(
    decomposition: &Decomposition,
    film: &mut Film,
    connection: CoordinatorEnd,
) -> Result<(), Error> {
    let CoordinatorEnd {
        to_worker,
        mut from_worker,
    } = connection;
    let mut queue = decomposition.work_units();
    let mut demand = 0_u32;
    while !film.is_complete() {
        match from_worker.recv().await.ok_or(Error::NoWorkers)? {
            ToCoordinator::RequestWork(count) => demand = demand.saturating_add(count),
            ToCoordinator::WorkResult(result) => {
                film.accept(&result);
            }
        }
        while demand > 0
            && let Some(unit) = queue.next()
        {
            to_worker
                .send(ToWorker::Assign(unit))
                .map_err(|_| Error::NoWorkers)?;
            demand -= 1;
        }
    }
    // The Worker may already be gone: it has nothing left to do.
    let _ = to_worker.send(ToWorker::JobFinished);
    Ok(())
}
