//! `coordinate`: a Render Job from its Scene to its image, through the
//! Local Workers (arc42 §5.3, §6.2).

use std::collections::{BTreeMap, VecDeque};
use std::num::NonZeroUsize;
use std::path::PathBuf;
use std::sync::Arc;
use std::thread;
use std::time::{Duration, Instant};

use quinn::Endpoint;
use rayon::{ThreadPool, ThreadPoolBuilder};
use tokio::runtime::Runtime;
use tokio::sync::mpsc::{UnboundedReceiver, UnboundedSender, unbounded_channel};
use tokio::time;
use tracing::{debug, error, info, info_span, warn};
use umbra_render::{
    Acceptance, Cancellation, Decomposition, Film, FilmHash, Image, ReproductionRecord, WorkResult,
    WorkUnit,
};
use umbra_scene::{RenderJobOptions, Scene};

use crate::children::{self, GpuWorkers};
use crate::cpu_worker;
use crate::device::{GpuOrdinal, RenderDevice};
use crate::error::Error;
use crate::host::Host;
use crate::listener::{self, Replica};
use crate::progress::Progress;
use crate::quic;
use crate::report::{Occupancy, Outcome, RenderJobReport, Rendered, Submission, WorkerFigures};
use crate::runtime;
use crate::status::{Moment, Status};
use crate::transport::{self, Event, ToCoordinator, ToWorker, WorkerId};

/// How long the Coordinator waits for its Workers to close once the Render
/// Job ends (arc42 §8.13).
const CLOSE_WAIT: Duration = Duration::from_secs(2);

/// The CPU Local Worker's identifier; the Workers over QUIC follow it.
const CPU_WORKER: WorkerId = WorkerId(0);

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

/// How the Coordinator runs a Render Job: the Local Workers it starts on
/// its own machine (ADR 0026). Remote Workers arrive with
/// [#129](https://github.com/sergioffpc/umbra/issues/129).
#[derive(Debug)]
pub struct CoordinatorOptions<'c> {
    /// The CPU Local Worker's threads, in the Coordinator's process; `None`
    /// for no CPU Local Worker.
    pub cpu: Option<Threads>,
    /// The GPU Local Workers, each in a child process; `None` for none.
    pub gpus: Option<GpuWorkers<'c>>,
}

impl Default for CoordinatorOptions<'_> {
    /// The CPU Local Worker on every logical CPU, and no GPU's.
    fn default() -> Self {
        Self {
            cpu: Some(Threads::All),
            gpus: None,
        }
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
/// [`Error`] if the Render Job cannot be cut into Work Units, the runtime,
/// the QUIC endpoint or the CPU Local Worker cannot start, or every Worker
/// goes away before the end.
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
    let cpu = options
        .cpu
        .map(|threads| CpuWorker::new(&scene, &job_options, threads))
        .transpose()?;
    let mut scheduler = Scheduler {
        film: Film::new(&scene, &job_options)?,
        progress: Progress::new(&decomposition, Instant::now()),
        show: status,
        threads: cpu.as_ref().map_or(0, CpuWorker::threads),
        queue: Queue::new(decomposition.work_units()),
        workers: BTreeMap::new(),
        starting: Vec::new(),
        events,
    };
    let runtime = runtime::runtime()?;
    let replica = || Replica::new(&job_options, scene.builder(), decomposition.clone());
    let span = info_span!("render").entered();
    let ending = thread::scope(|scope| {
        if let Some(cpu) = &cpu {
            cpu.start(scope, &runtime, &mut scheduler);
        }
        runtime.block_on(scheduler.drive(options.gpus.as_ref(), replica, &mut inbox, cancel))
    })?;
    drop(span);
    Ok(scheduler.finish(
        ending,
        Submission {
            options: job_options,
            output,
            reproduction_record: record,
        },
    ))
}

/// The CPU Local Worker, before it starts: its renderer and its threads.
struct CpuWorker<'s> {
    renderer: umbra_cpu::Renderer<'s>,
    pool: ThreadPool,
}

impl<'s> CpuWorker<'s> {
    /// The CPU Local Worker of a Render Job of `scene` with `options`, on
    /// `threads`.
    fn new(scene: &'s Scene, options: &RenderJobOptions, threads: Threads) -> Result<Self, Error> {
        let count = match threads {
            Threads::All => 0,
            Threads::Count(n) => n.get(),
        };
        let pool = ThreadPoolBuilder::new()
            .num_threads(count)
            .build()
            .map_err(Error::ThreadPool)?;
        Ok(Self {
            renderer: umbra_cpu::Renderer::new(scene, options)?,
            pool,
        })
    }

    /// Its threads.
    fn threads(&self) -> usize {
        self.pool.current_num_threads()
    }

    /// Starts its render loop on a thread of `scope`, joins it to
    /// `scheduler`, and forwards its messages to the scheduler's events on
    /// `runtime`.
    fn start<'scope>(
        &'scope self,
        scope: &'scope thread::Scope<'scope, '_>,
        runtime: &Runtime,
        scheduler: &mut Scheduler<'_, impl Iterator<Item = WorkUnit>, impl FnMut(&Status, Moment)>,
    ) {
        let (coordinator, worker) = transport::connect();
        scope.spawn(move || cpu_worker::work(&self.renderer, &self.pool, worker));
        scheduler.join(CPU_WORKER, RenderDevice::Cpu, coordinator.to_worker);
        runtime.spawn(transport::forward(
            CPU_WORKER,
            coordinator.from_worker,
            scheduler.events.clone(),
        ));
    }
}

/// The GPU Local Workers once started: the endpoint they connect to, open
/// while the Render Job runs, and the GPUs whose process started.
#[derive(Default)]
struct GpuChildren {
    _endpoint: Option<Endpoint>,
    starting: Vec<GpuOrdinal>,
}

/// Starts the Local Workers of `gpus`, each sent `replica` once it joins,
/// on an endpoint of their own whose events go to `events`.
fn start_gpus(
    gpus: Option<&GpuWorkers<'_>>,
    replica: impl FnOnce() -> Replica,
    events: &UnboundedSender<Event>,
) -> Result<GpuChildren, Error> {
    let Some(GpuWorkers { gpus, command }) = gpus.filter(|workers| !workers.gpus.is_empty()) else {
        return Ok(GpuChildren::default());
    };
    let endpoint = quic::coordinator_endpoint()?;
    let address = endpoint.local_addr().map_err(Error::Listen)?;
    tokio::spawn(listener::listen(
        endpoint.clone(),
        Arc::new(replica()),
        CPU_WORKER.next(),
        events.clone(),
    ));
    Ok(GpuChildren {
        starting: children::launch(gpus, *command, address, events),
        _endpoint: Some(endpoint),
    })
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
    /// The CPU Local Worker's threads; 0 without one.
    threads: usize,
    queue: Queue<Q>,
    workers: BTreeMap<WorkerId, Worker>,
    /// The GPUs whose Local Worker's process runs and has not joined yet.
    starting: Vec<GpuOrdinal>,
    /// Where the Workers' events go.
    events: UnboundedSender<Event>,
}

impl<Q: Iterator<Item = WorkUnit>, S: FnMut(&Status, Moment)> Scheduler<'_, Q, S> {
    /// Starts the GPU Local Workers of `gpus`, runs the Render Job, and
    /// closes every Worker once it ends; the Workers' events arrive in
    /// `inbox`.
    async fn drive(
        &mut self,
        gpus: Option<&GpuWorkers<'_>>,
        replica: impl FnOnce() -> Replica,
        inbox: &mut UnboundedReceiver<Event>,
        cancel: &Cancellation,
    ) -> Result<Ending, Error> {
        let (ending, _children) = match start_gpus(gpus, replica, &self.events) {
            Ok(children) => {
                self.starting.clone_from(&children.starting);
                (self.run(inbox, cancel).await, children)
            }
            Err(error) => (Err(error), GpuChildren::default()),
        };
        self.close(inbox, &ending).await;
        ending
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
            Event::Joined {
                worker,
                device,
                to_worker,
            } => self.join(worker, device, to_worker),
            Event::Message { worker, message } => self.message(worker, message),
            Event::Left { worker } => self.retire(worker),
            Event::Exited { gpu } => self.exited(gpu),
        }
    }

    /// The Local Worker of `gpu` exited: it is no longer starting, and if
    /// it joined, it goes. A Worker is told apart by its Render Device, so
    /// another Worker on the same device would be taken for it.
    fn exited(&mut self, gpu: GpuOrdinal) {
        self.started(gpu);
        let device = RenderDevice::Gpu(gpu);
        let joined = self
            .workers
            .iter()
            .find_map(|(&id, worker)| (worker.device == device).then_some(id));
        if let Some(worker) = joined {
            self.retire(worker);
        }
    }

    /// Lets `worker`, on `device`, ask for Work Units through `to_worker`.
    fn join(
        &mut self,
        worker: WorkerId,
        device: RenderDevice,
        to_worker: UnboundedSender<ToWorker>,
    ) {
        debug!("Worker {worker} joined on {device}");
        if let RenderDevice::Gpu(gpu) = device {
            self.started(gpu);
        }
        self.workers.insert(
            worker,
            Worker {
                id: worker,
                device,
                sender: to_worker,
                readiness: Readiness::Loading,
                demand: 0,
                in_flight: Vec::new(),
            },
        );
    }

    /// The Local Worker of `gpu` is no longer starting: it joined, or its
    /// process exited.
    fn started(&mut self, gpu: GpuOrdinal) {
        self.starting.retain(|&starting| starting != gpu);
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
                self.thread_time_of(id, thread_time);
                self.merge(&result, render_time);
            }
            ToCoordinator::Fatal(reason) => {
                error!("Worker {id} on {} cannot continue: {reason}", worker.device);
            }
        }
    }

    /// The threads of `worker` spent `thread_time` on a Work Unit: counted
    /// if it is the CPU Local Worker, whose occupancy the report gives.
    fn thread_time_of(&mut self, worker: WorkerId, thread_time: Duration) {
        if worker == CPU_WORKER {
            self.progress.rendered(thread_time);
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
                    Event::Joined { to_worker, .. } => {
                        let _ = to_worker.send(message());
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
