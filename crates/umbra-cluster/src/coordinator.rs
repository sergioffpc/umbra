//! `coordinate`: a Render Job from its Scene to its image, through the
//! Local Workers, each a child process over QUIC (arc42 §5.3, §6.2).

use std::collections::{BTreeMap, VecDeque};
use std::fmt;
use std::path::PathBuf;
use std::sync::Arc;
use std::time::{Duration, Instant};

use quinn::Endpoint;
use tokio::sync::mpsc::{UnboundedReceiver, UnboundedSender, unbounded_channel};
use tokio::time;
use tracing::{debug, error, info, info_span, warn};
use umbra_render::{
    Acceptance, Cancellation, Decomposition, Film, FilmHash, Image, ReproductionRecord, WorkResult,
    WorkUnit,
};
use umbra_scene::RenderJobOptions;

use crate::children::{self, ProcessId, WorkerCommand};
use crate::device::RenderDevice;
use crate::error::Error;
use crate::host::Host;
use crate::listener::{self, Replica};
use crate::progress::Progress;
use crate::quic;
use crate::report::{Occupancy, Outcome, RenderJobReport, Rendered, Submission, WorkerFigures};
use crate::runtime;
use crate::status::{Moment, Status};
use crate::transport::{Event, Joined, ToCoordinator, ToWorker, WorkerId};

/// How long the Coordinator waits for its Workers to close once the Render
/// Job ends (arc42 §8.13).
const CLOSE_WAIT: Duration = Duration::from_secs(2);

/// A Render Job: what is rendered, how it is cut into Work Units, where
/// its image goes, and the record of how to compute it again.
#[derive(Debug)]
pub struct RenderJob {
    /// The Scene.
    pub scene: umbra_scene::Scene,
    /// The Render Job Options.
    pub options: RenderJobOptions,
    /// Where its final image goes: the caller writes it, and the report
    /// lists it with the options.
    pub output: PathBuf,
    /// Its Reproduction Record.
    pub record: ReproductionRecord,
}

/// How the Coordinator runs a Render Job: the Local Workers it starts on
/// its own machine, each a child process that connects back to it over
/// QUIC on loopback, as a remote Worker would (ADR 0026, ADR 0035).
/// Remote Workers arrive with
/// [#129](https://github.com/sergioffpc/umbra/issues/129).
pub struct CoordinatorOptions<'c> {
    /// The Render Devices that get a Local Worker each.
    pub local_workers: Vec<RenderDevice>,
    /// The command line of each one.
    pub worker_command: &'c WorkerCommand<'c>,
}

impl fmt::Debug for CoordinatorOptions<'_> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("CoordinatorOptions")
            .field("local_workers", &self.local_workers)
            .finish_non_exhaustive()
    }
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

/// Renders `job` to its image with the Local Workers of `options`,
/// assigning Work Units as each asks for them, Pass by Pass in raster Tile
/// order (ADR 0009), and merging each Work Result into the Film as it
/// arrives (ADR 0022). A Worker that goes away before the end, its child
/// process exiting or its connection closing, gives its Work Units in
/// flight back to the front of the queue, and the Render Job finishes on
/// the Workers that remain. It blocks until the last Pass is merged or
/// `cancel` is made; the runtime it needs is its own. `status` receives
/// its [`Status`] at each tick, as [`Moment::Running`], and once at the
/// end, as [`Moment::Ended`]: it is the caller's to show.
///
/// # Errors
///
/// [`Error`] if the Render Job cannot be cut into Work Units, the runtime
/// or the QUIC endpoint cannot start, or every Worker goes away before the
/// end.
pub fn coordinate(
    job: RenderJob,
    options: &CoordinatorOptions<'_>,
    cancel: &Cancellation,
    status: impl FnMut(&Status, Moment),
) -> Result<RenderJobOutcome, Error> {
    let RenderJob {
        scene,
        options: job_options,
        output,
        record,
    } = job;
    let decomposition = Decomposition::new(scene.full_resolution(), &job_options)?;
    let (events, mut inbox) = unbounded_channel();
    let mut scheduler = Scheduler {
        film: Film::new(&scene, &job_options)?,
        progress: Progress::new(&decomposition, Instant::now()),
        show: status,
        threads: 0,
        queue: Queue::new(decomposition.work_units()),
        workers: BTreeMap::new(),
        starting: BTreeMap::new(),
        events,
    };
    let replica = Replica::new(&job_options, scene.builder(), decomposition.clone());
    let ending = info_span!("render").in_scope(|| {
        runtime::runtime()?.block_on(scheduler.drive(options, replica, &mut inbox, cancel))
    })?;
    Ok(scheduler.finish(
        ending,
        Submission {
            options: job_options,
            output,
            reproduction_record: record,
        },
    ))
}

/// The Work Units not assigned yet: those given back by Workers that went
/// away, first, then the rest in order.
struct Queue<I> {
    returned: VecDeque<WorkUnit>,
    rest: I,
}

impl<I: Iterator<Item = WorkUnit>> Queue<I> {
    fn new(rest: I) -> Self {
        Self {
            returned: VecDeque::new(),
            rest,
        }
    }

    /// Puts `units` at the front of the queue, in their order.
    fn give_back(&mut self, units: Vec<WorkUnit>) {
        for unit in units.into_iter().rev() {
            self.returned.push_front(unit);
        }
    }
}

impl<I: Iterator<Item = WorkUnit>> Iterator for Queue<I> {
    type Item = WorkUnit;

    fn next(&mut self) -> Option<WorkUnit> {
        self.returned.pop_front().or_else(|| self.rest.next())
    }
}

/// A Worker that joined: how to reach it, what it asked for and was not
/// given yet, and what it was given and has not answered.
struct Worker {
    id: WorkerId,
    device: RenderDevice,
    /// Its process, when it is a Local Worker the Coordinator started.
    process: ProcessId,
    sender: UnboundedSender<ToWorker>,
    readiness: Readiness,
    demand: u32,
    in_flight: Vec<WorkUnit>,
}

/// Whether a Worker has asked for its first Work Units, which it does once
/// it has loaded the Scene and can render (arc42 §8.13).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Readiness {
    Loading,
    Ready,
}

impl Worker {
    /// Adds `count` Work Units to its demand, logging its first request: it
    /// is ready.
    fn request(&mut self, count: u32) {
        if self.readiness == Readiness::Loading {
            info!("Worker {} on {} is ready", self.id, self.device);
            self.readiness = Readiness::Ready;
        }
        self.demand = self.demand.saturating_add(count);
    }

    /// The share of `thread_time` that counts for the CPU's occupancy: all
    /// of it on the CPU, none on a GPU.
    fn cpu_thread_time(&self, thread_time: Duration) -> Duration {
        match self.device {
            RenderDevice::Cpu => thread_time,
            RenderDevice::Gpu(_) => Duration::ZERO,
        }
    }

    /// Assigns it Work Units from `queue` while it asks for more and any is
    /// left. A Worker gone away meanwhile gives them back when it leaves.
    fn fill(&mut self, queue: &mut impl Iterator<Item = WorkUnit>, progress: &mut Progress<'_>) {
        while self.demand > 0
            && let Some(unit) = queue.next()
        {
            let _ = self.sender.send(ToWorker::Assign(unit));
            self.in_flight.push(unit);
            self.demand -= 1;
            progress.assigned(Instant::now());
        }
    }
}

/// The Coordinator's side of a running Render Job: the Film it merges
/// into, the Work Units not assigned yet, its Workers, its progress, and
/// where its status goes.
struct Scheduler<'d, Q, S> {
    film: Film,
    progress: Progress<'d>,
    /// Receives the status.
    show: S,
    /// The threads of the CPU Workers that joined, summed over them.
    threads: usize,
    queue: Queue<Q>,
    workers: BTreeMap<WorkerId, Worker>,
    /// The Local Workers whose process runs and has not joined yet.
    starting: BTreeMap<ProcessId, RenderDevice>,
    /// Where the Workers' events go.
    events: UnboundedSender<Event>,
}

impl<Q: Iterator<Item = WorkUnit>, S: FnMut(&Status, Moment)> Scheduler<'_, Q, S> {
    /// Starts the Local Workers of `options`, runs the Render Job, and
    /// closes every Worker once it ends; each joins with `replica`, and the
    /// Workers' events arrive in `inbox`.
    async fn drive(
        &mut self,
        options: &CoordinatorOptions<'_>,
        replica: Replica,
        inbox: &mut UnboundedReceiver<Event>,
        cancel: &Cancellation,
    ) -> Result<Ending, Error> {
        let (ending, _endpoint) = match self.start(options, replica) {
            Ok(endpoint) => (self.run(inbox, cancel).await, Some(endpoint)),
            Err(error) => (Err(error), None),
        };
        self.close(inbox, &ending).await;
        ending
    }

    /// Listens for the Local Workers of `options` on an endpoint of their
    /// own, open while the Render Job runs, and starts them.
    fn start(
        &mut self,
        options: &CoordinatorOptions<'_>,
        replica: Replica,
    ) -> Result<Endpoint, Error> {
        let endpoint = quic::coordinator_endpoint()?;
        let address = endpoint.local_addr().map_err(Error::Listen)?;
        tokio::spawn(listener::listen(
            endpoint.clone(),
            Arc::new(replica),
            self.events.clone(),
        ));
        self.starting = children::launch(
            &options.local_workers,
            options.worker_command,
            address,
            &self.events,
        );
        Ok(endpoint)
    }

    /// Assigns every Work Unit as the Workers ask, and merges each Work
    /// Result, until every one is merged or `cancel` is made.
    async fn run(
        &mut self,
        inbox: &mut UnboundedReceiver<Event>,
        cancel: &Cancellation,
    ) -> Result<Ending, Error> {
        let mut ticker = runtime::ticker();
        while !self.film.is_complete() {
            self.check_workers()?;
            tokio::select! {
                Some(event) = inbox.recv() => {
                    self.receive(event);
                    self.assign();
                }
                _ = ticker.tick() => {
                    if cancel.is_cancelled() {
                        return Ok(Ending::Cancelled);
                    }
                    let now = Instant::now();
                    self.progress.tick(now);
                    (self.show)(&Status::of(&self.progress, self.threads, now), Moment::Running);
                }
            }
        }
        Ok(Ending::Finished)
    }

    /// Fails once no Worker is left, joined or starting.
    fn check_workers(&self) -> Result<(), Error> {
        if self.workers.is_empty() && self.starting.is_empty() {
            Err(Error::NoWorkers)
        } else {
            Ok(())
        }
    }

    /// Takes in what happened to a Worker.
    fn receive(&mut self, event: Event) {
        match event {
            Event::Joined(joined) => self.join(joined),
            Event::Message { worker, message } => self.message(worker, message),
            Event::Left { worker } => self.retire(worker),
            Event::Exited { process } => self.exited(process),
        }
    }

    /// The Local Worker of `process` exited: it is no longer starting, and
    /// if it joined, it goes.
    fn exited(&mut self, process: ProcessId) {
        self.starting.remove(&process);
        let joined = self
            .workers
            .values()
            .find_map(|worker| (worker.process == process).then_some(worker.id));
        if let Some(worker) = joined {
            self.retire(worker);
        }
    }

    /// Lets the Worker that `joined` describes ask for Work Units.
    fn join(&mut self, joined: Joined) {
        let Joined {
            worker,
            device,
            threads,
            process,
            to_worker,
        } = joined;
        debug!("Worker {worker} joined on {device}, {threads} threads");
        self.starting.remove(&process);
        if device == RenderDevice::Cpu {
            self.threads += usize::try_from(threads).unwrap_or(usize::MAX);
        }
        self.workers.insert(
            worker,
            Worker {
                id: worker,
                device,
                process,
                sender: to_worker,
                readiness: Readiness::Loading,
                demand: 0,
                in_flight: Vec::new(),
            },
        );
    }

    /// Takes in a message from `worker`, unless it has gone away already.
    fn message(&mut self, id: WorkerId, message: ToCoordinator) {
        let Some(worker) = self.workers.get_mut(&id) else {
            return;
        };
        match message {
            ToCoordinator::RequestWork(count) => worker.request(count),
            ToCoordinator::WorkResult {
                result,
                render_time,
                thread_time,
            } => {
                worker.in_flight.retain(|&unit| unit != result.unit());
                self.progress.rendered(worker.cpu_thread_time(thread_time));
                self.merge(&result, render_time);
            }
            ToCoordinator::Fatal(reason) => {
                error!("Worker {id} on {} cannot continue: {reason}", worker.device);
            }
        }
    }

    /// Lets `worker` go: its Work Units in flight go back to the front of
    /// the queue (arc42 §8.8).
    fn retire(&mut self, id: WorkerId) {
        let Some(worker) = self.workers.remove(&id) else {
            return;
        };
        let lost = worker.in_flight.len();
        if lost > 0 {
            warn!(
                "Worker {id} on {} went away: its {lost} Work Units in flight go back to the queue",
                worker.device
            );
            let now = Instant::now();
            (0..lost).for_each(|_| self.progress.returned(now));
            self.queue.give_back(worker.in_flight);
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

    /// Assigns each Worker as many Work Units as it asked for, while any is
    /// left.
    fn assign(&mut self) {
        for worker in self.workers.values_mut() {
            worker.fill(&mut self.queue, &mut self.progress);
        }
    }

    /// Tells every Worker how the Render Job ended, `JobFinished` or a
    /// cancellation, and waits for them to close, for at most
    /// [`CLOSE_WAIT`].
    async fn close(
        &mut self,
        inbox: &mut UnboundedReceiver<Event>,
        ending: &Result<Ending, Error>,
    ) {
        let message = || match ending {
            Ok(Ending::Finished) => ToWorker::JobFinished,
            Ok(Ending::Cancelled) | Err(_) => ToWorker::Cancel,
        };
        for worker in self.workers.values() {
            let _ = worker.sender.send(message());
        }
        let deadline = time::sleep(CLOSE_WAIT);
        tokio::pin!(deadline);
        while !self.workers.is_empty() {
            tokio::select! {
                Some(event) = inbox.recv() => match event {
                    Event::Left { worker } => {
                        self.workers.remove(&worker);
                    }
                    Event::Joined(joined) => {
                        let _ = joined.to_worker.send(message());
                    }
                    Event::Exited { .. } | Event::Message { .. } => {}
                },
                () = &mut deadline => return,
            }
        }
    }

    /// Sends the last status and makes the outcome of the Render Job
    /// submitted as `submission` says.
    fn finish(mut self, ending: Ending, submission: Submission) -> RenderJobOutcome {
        let now = Instant::now();
        self.progress.tick(now);
        (self.show)(
            &Status::of(&self.progress, self.threads, now),
            Moment::Ended,
        );
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
                threads: self.threads,
                work_units: self.progress.merged_units(),
                pixel_samples: self.progress.merged_samples(),
                busy_time,
                idle_time: render_time.saturating_sub(busy_time),
                occupancy: Occupancy::new(self.progress.thread_time(), self.threads, render_time),
            },
            tile_costs: self.progress.tile_costs(),
        }
    }
}
