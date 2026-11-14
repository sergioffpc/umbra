//! `coordinate`: a Render Job from its Scene to its image, through the
//! Local Workers, each a child process over QUIC, and the remote Workers
//! of its Cluster, which find it by DNS-SD (arc42 §5.3, §6.2).

use std::collections::{BTreeMap, VecDeque};
use std::fmt::{self, Display};
use std::net::{IpAddr, Ipv4Addr, SocketAddr};
use std::path::PathBuf;
use std::sync::Arc;
use std::time::{Duration, Instant};

use quinn::Endpoint;
use tokio::sync::mpsc::{UnboundedReceiver, UnboundedSender, unbounded_channel};
use tokio::sync::oneshot;
use tokio::time;
use tracing::{error, info, info_span, warn};
use umbra_render::{
    Acceptance, BuildIdentity, Cancellation, Decomposition, Film, FilmHash, Image,
    ReproductionRecord, Tree, WorkResult, WorkUnit,
};
use umbra_scene::RenderJobOptions;

use crate::children::{self, ProcessId, WorkerCommand};
use crate::cluster::ClusterName;
use crate::device::RenderDevice;
use crate::discovery::{Announcement, Interface};
use crate::error::Error;
use crate::host::Host;
use crate::listener::{self, Replica, Serving};
use crate::progress::Progress;
use crate::quic;
use crate::report::{Cluster, Outcome, RenderJobReport, Rendered, Scheduling, Submission};
use crate::roster::{self, Liveness, Merged, Readiness, Worker, WorkerKind};
use crate::runtime;
use crate::slot::Slots;
use crate::status::{Moment, Staffing, Status};
use crate::transport::{Departure, Event, ToCoordinator, ToWorker, Verdict, WorkerId};
use crate::wire::{Heartbeat, Hello, Refusal};

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
    /// Its Reproduction Record, whose Build Identity is the Coordinator's.
    pub record: ReproductionRecord,
}

/// Whether the Coordinator admits Workers built from a dirty tree
/// (`--allow-dirty`, ADR 0012). Its own Local Workers are its own
/// executable, and always admitted.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum DirtyBuilds {
    /// A dirty Worker is refused with `DirtyBuild`.
    #[default]
    Refused,
    /// A dirty Worker on the Coordinator's commit is admitted.
    Allowed,
}

impl Display for DirtyBuilds {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(match self {
            Self::Refused => "refused",
            Self::Allowed => "allowed",
        })
    }
}

/// How the Coordinator runs a Render Job: the Local Workers it starts on
/// its own machine, each a child process that connects back to it over
/// QUIC, as a remote Worker would (ADR 0026, ADR 0035), and how it meets
/// the remote Workers of its Cluster (ADR 0021, ADR 0029).
pub struct CoordinatorOptions<'c> {
    /// The Render Devices that get a Local Worker each.
    pub local_workers: Vec<RenderDevice>,
    /// The command line of each one.
    pub worker_command: &'c WorkerCommand<'c>,
    /// The Cluster whose Workers it admits.
    pub cluster: ClusterName,
    /// The UDP port it listens on; 0 for one the OS picks.
    pub port: u16,
    /// The interfaces it listens and announces on.
    pub interface: Interface,
    /// Whether it admits Workers built from a dirty tree.
    pub dirty_builds: DirtyBuilds,
    /// How it judges its Workers' heartbeats.
    pub liveness: Liveness,
}

impl fmt::Debug for CoordinatorOptions<'_> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("CoordinatorOptions")
            .field("local_workers", &self.local_workers)
            .field("cluster", &self.cluster)
            .field("port", &self.port)
            .field("interface", &self.interface)
            .field("dirty_builds", &self.dirty_builds)
            .field("liveness", &self.liveness)
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

/// Renders `job` to its image with the Local Workers of `options` and the
/// remote Workers of its Cluster: it listens, announces the Render Job,
/// starts the Local Workers, admits each Worker that says `Hello`, assigns
/// Work Units as each asks for them, Pass by Pass in raster Tile order,
/// then copies of the oldest in flight once none is left (ADR 0009), and
/// merges each Work Result into the Film as it arrives (ADR 0022). A
/// Worker that goes away gives its Work Units in flight back to the front
/// of the queue; with no Worker left, the Render Job waits for one. It
/// blocks until the last Pass is merged or `cancel` is made; the runtime
/// it needs is its own. `status` receives its [`Status`] at each tick, as
/// [`Moment::Running`], and once at the end, as [`Moment::Ended`]: it is
/// the caller's to show.
///
/// # Errors
///
/// [`Error`] if the Render Job cannot be cut into Work Units, the runtime
/// cannot start, or the Coordinator cannot listen where `options` say:
/// all before anything is announced.
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
        decomposition: &decomposition,
        progress: Progress::new(&decomposition, Instant::now()),
        show: status,
        queue: Queue::new(decomposition.work_units()),
        tail: Tail::default(),
        workers: BTreeMap::new(),
        slots: Slots::default(),
        scheduling: Scheduling::default(),
        starting: BTreeMap::new(),
        events,
        identity: record.build_identity().clone(),
        options,
        last: None,
    };
    let replica = Replica::new(&job_options, scene.builder(), decomposition.clone());
    let ending = info_span!("render")
        .in_scope(|| runtime::runtime()?.block_on(scheduler.drive(replica, &mut inbox, cancel)))?;
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

/// The speculative tail (ADR 0009): when each Work Unit was first
/// assigned, and the Worker given the one copy of each that has one.
#[derive(Debug, Default)]
struct Tail {
    /// Each Work Unit in flight, by the order it was assigned.
    assigned: BTreeMap<WorkUnit, u64>,
    next: u64,
    /// The Work Units copied, and the Worker holding the copy.
    copies: BTreeMap<WorkUnit, WorkerId>,
}

/// The Coordinator's side of a running Render Job: the Film it merges
/// into, the Work Units not assigned yet, its Workers, its progress, and
/// where its status goes.
struct Scheduler<'d, 'o, Q, S> {
    film: Film,
    decomposition: &'d Decomposition,
    progress: Progress<'d>,
    /// Receives the status.
    show: S,
    queue: Queue<Q>,
    tail: Tail,
    /// Every Worker admitted, present or gone.
    workers: BTreeMap<WorkerId, Worker>,
    slots: Slots,
    scheduling: Scheduling,
    /// The Local Workers whose process runs and has not joined yet.
    starting: BTreeMap<ProcessId, RenderDevice>,
    /// Where the Workers' events go.
    events: UnboundedSender<Event>,
    /// The Coordinator's Build Identity, which a Worker must share.
    identity: BuildIdentity,
    options: &'o CoordinatorOptions<'o>,
    /// The status as the Render Job ended.
    last: Option<Status>,
}

impl<Q: Iterator<Item = WorkUnit>, S: FnMut(&Status, Moment)> Scheduler<'_, '_, Q, S> {
    /// Listens and announces the Render Job, starts the Local Workers,
    /// runs the Render Job, withdraws the announcement and closes every
    /// Worker once it ends; each joins with `replica`, and the Workers'
    /// events arrive in `inbox`.
    async fn drive(
        &mut self,
        replica: Replica,
        inbox: &mut UnboundedReceiver<Event>,
        cancel: &Cancellation,
    ) -> Result<Ending, Error> {
        let (endpoint, announcement) = self.start(replica)?;
        let ending = self.run(inbox, cancel).await;
        // The last status is the Render Job's as it ends, before its
        // Workers close.
        let now = Instant::now();
        self.progress.tick(now);
        self.last = Some(self.status(now));
        if let Some(announcement) = announcement {
            announcement.withdraw();
        }
        self.close(inbox, ending).await;
        drop(endpoint);
        Ok(ending)
    }

    /// Listens where the options say, announces the Render Job there, and
    /// starts the Local Workers; the endpoint stays open, and the Render
    /// Job announced, while the Render Job runs.
    fn start(&mut self, replica: Replica) -> Result<(Endpoint, Option<Announcement>), Error> {
        let options = self.options;
        let ip = options.interface.address()?;
        let address = SocketAddr::new(ip, options.port);
        let endpoint = quic::coordinator_endpoint(address, options.liveness.death_time)?;
        let port = endpoint.local_addr().map_err(Error::Listen)?.port();
        let serving = Serving {
            replica,
            death_time: options.liveness.death_time,
        };
        tokio::spawn(listener::listen(
            endpoint.clone(),
            Arc::new(serving),
            self.events.clone(),
        ));
        let announcement = match Announcement::new(&options.cluster, &options.interface, ip, port) {
            Ok(announcement) => {
                info!(
                    "Render Job {} announced to Cluster {} on UDP port {port}",
                    announcement.id(),
                    options.cluster
                );
                Some(announcement)
            }
            Err(error) => {
                warn!("cannot announce the Render Job, only the Local Workers render it: {error}");
                None
            }
        };
        // The Local Workers reach the Coordinator on loopback, unless it
        // listens on one interface alone, whose address loopback is not.
        let local = match ip {
            IpAddr::V4(v4) if v4.is_unspecified() => IpAddr::V4(Ipv4Addr::LOCALHOST),
            IpAddr::V4(_) | IpAddr::V6(_) => ip,
        };
        self.starting = children::launch(
            &options.local_workers,
            options.worker_command,
            SocketAddr::new(local, port),
            &self.events,
        );
        Ok((endpoint, announcement))
    }

    /// Assigns every Work Unit as the Workers ask, and merges each Work
    /// Result, until every one is merged or `cancel` is made.
    async fn run(&mut self, inbox: &mut UnboundedReceiver<Event>, cancel: &Cancellation) -> Ending {
        let mut ticker = runtime::ticker();
        while !self.film.is_complete() {
            tokio::select! {
                Some(event) = inbox.recv() => {
                    self.receive(event);
                    self.assign();
                }
                _ = ticker.tick() => {
                    if cancel.is_cancelled() {
                        return Ending::Cancelled;
                    }
                    self.tick(Instant::now());
                }
            }
        }
        Ending::Finished
    }

    /// Takes in the progress made up to `now`, and shows the status.
    fn tick(&mut self, now: Instant) {
        self.progress.tick(now);
        let liveness = self.options.liveness;
        for worker in self.workers.values_mut() {
            worker.tick(now, liveness);
        }
        let status = self.status(now);
        (self.show)(&status, Moment::Running);
    }

    /// The status at `now`: the aggregate line, then one per Worker present,
    /// and one per Worker gone whose Worker Slot has had no Worker since.
    fn status(&self, now: Instant) -> Status {
        let liveness = self.options.liveness;
        let present = self.workers.values().any(Worker::is_present);
        let staffing = if present || !self.starting.is_empty() {
            Staffing::Workers
        } else {
            Staffing::NoWorkers
        };
        let in_flight = self.workers.values().map(Worker::in_flight_samples).sum();
        let latest: BTreeMap<_, _> = self
            .workers
            .values()
            .map(|worker| (worker.slot(), worker.id))
            .collect();
        let lines = self
            .workers
            .values()
            .filter(|worker| worker.is_present() || latest.get(&worker.slot()) == Some(&worker.id))
            .map(|worker| worker.status_line(now, liveness, self.slots.failures(&worker.slot())))
            .collect();
        Status::of(&self.progress, in_flight, staffing, lines, now)
    }

    /// Takes in what happened to a Worker.
    fn receive(&mut self, event: Event) {
        match event {
            Event::Arrived {
                worker,
                hello,
                to_worker,
                verdict,
            } => self.arrive(worker, hello, to_worker, verdict),
            Event::Message { worker, message } => self.message(worker, message),
            Event::Heartbeat { worker, heartbeat } => self.heard(worker, heartbeat),
            Event::Left { worker, departure } => self.leave(worker, departure),
            Event::Exited { process } => self.exited(process),
        }
    }

    /// Takes in `heartbeat` from the Worker `id`.
    fn heard(&mut self, id: WorkerId, heartbeat: Heartbeat) {
        if let Some(worker) = self.workers.get_mut(&id) {
            worker.heard(heartbeat, Instant::now());
        }
    }

    /// Admits or refuses the Worker `hello` describes, answering through
    /// `verdict`.
    fn arrive(
        &mut self,
        id: WorkerId,
        hello: Box<Hello>,
        to_worker: UnboundedSender<ToWorker>,
        verdict: oneshot::Sender<Verdict>,
    ) {
        let kind = self.kind_of(&hello);
        let slot = hello.slot();
        match self.admission(kind, &hello) {
            Verdict::Welcome => {
                info!(
                    "Worker {id} joined: {} on host {} node {}, {} threads",
                    hello.render_device,
                    hello.host.name(),
                    hello.numa_node,
                    hello.threads
                );
                self.slots.joined(&slot);
                let worker = Worker::new(id, kind, hello, to_worker, Instant::now());
                self.workers.insert(id, worker);
                let _ = verdict.send(Verdict::Welcome);
            }
            Verdict::Reject(reason) => {
                warn!(
                    "Worker {id} refused: {} on host {} node {}, build {}: {reason}",
                    hello.render_device,
                    hello.host.name(),
                    hello.numa_node,
                    hello.build_identity.commit
                );
                self.slots.refused(&slot);
                let _ = verdict.send(Verdict::Reject(reason));
            }
        }
    }

    /// Whether `hello` comes from one of the Coordinator's own Local
    /// Workers, which is no longer starting then.
    fn kind_of(&mut self, hello: &Hello) -> WorkerKind {
        let is_child =
            hello.host == Host::this() && self.starting.remove(&hello.process_id).is_some();
        if is_child {
            WorkerKind::Local
        } else {
            WorkerKind::Remote
        }
    }

    /// Whether the Worker `hello` describes, of `kind`, is admitted
    /// (arc42 §8.13). A Local Worker is the Coordinator's own executable.
    fn admission(&self, kind: WorkerKind, hello: &Hello) -> Verdict {
        match kind {
            WorkerKind::Local => Verdict::Welcome,
            WorkerKind::Remote => self
                .refusal(hello)
                .map_or(Verdict::Welcome, Verdict::Reject),
        }
    }

    /// Why the remote Worker `hello` describes is refused, if it is: of
    /// another Cluster, built from another commit or, unless the options
    /// allow it, a dirty tree, or of a Worker Slot that failed too often.
    fn refusal(&self, hello: &Hello) -> Option<Refusal> {
        let options = self.options;
        let identity = &hello.build_identity;
        if hello.cluster != options.cluster {
            Some(Refusal::ClusterMismatch)
        } else if identity.commit != self.identity.commit {
            Some(Refusal::CommitMismatch {
                coordinator_commit: self.identity.commit.clone(),
            })
        } else if identity.tree == Tree::Dirty && options.dirty_builds == DirtyBuilds::Refused {
            Some(Refusal::DirtyBuild)
        } else {
            self.slots
                .is_barred(&hello.slot())
                .then_some(Refusal::TooManyFailures)
        }
    }

    /// The Local Worker of `process` exited: it is no longer starting, and
    /// if it joined, it goes.
    fn exited(&mut self, process: ProcessId) {
        self.starting.remove(&process);
        let joined = self.workers.values().find_map(|worker| {
            (worker.kind == WorkerKind::Local
                && worker.is_present()
                && worker.hello.process_id == process)
                .then_some(worker.id)
        });
        if let Some(worker) = joined {
            self.retire(worker);
        }
    }

    /// Takes in a message from `worker`, unless it has gone away already.
    fn message(&mut self, id: WorkerId, message: ToCoordinator) {
        let Some(worker) = self.workers.get_mut(&id).filter(|w| w.is_present()) else {
            return;
        };
        match message {
            ToCoordinator::RequestWork(count) => worker.request(count),
            ToCoordinator::WorkResult {
                result,
                render_time,
                thread_time,
            } => self.result(id, &result, render_time, thread_time),
            ToCoordinator::Fatal(reason) => {
                error!("{} cannot continue: {reason}", worker.name());
            }
        }
    }

    /// Takes in `result` from the Worker `id`: merged into the Film if it
    /// is the first for its Work Unit, whose other holder is then told to
    /// drop it, or discarded.
    fn result(
        &mut self,
        id: WorkerId,
        result: &WorkResult,
        render_time: Duration,
        thread_time: Duration,
    ) {
        let unit = result.unit();
        let now = Instant::now();
        let start = Instant::now();
        let acceptance = info_span!("merge").in_scope(|| self.film.accept(result));
        let merge_time = start.elapsed();
        let merged = match acceptance {
            Acceptance::Merged => {
                self.progress.merged(unit, render_time, merge_time);
                self.settle(unit, id, now);
                Merged::Yes
            }
            Acceptance::Duplicate => {
                self.scheduling.duplicates += 1;
                Merged::No
            }
        };
        let samples = roster::pixel_samples(self.decomposition, unit);
        if let Some(worker) = self.workers.get_mut(&id) {
            worker.release(unit, now);
            worker.rendered(samples, thread_time, merged);
            self.slots.delivered(&worker.slot());
        }
    }

    /// `unit` was merged from the Worker `winner`: every other Worker
    /// holding it drops it, and its copy, if it had one, won or lost.
    fn settle(&mut self, unit: WorkUnit, winner: WorkerId, now: Instant) {
        self.tail.assigned.remove(&unit);
        match self.tail.copies.remove(&unit) {
            Some(copy) if copy == winner => self.scheduling.copies_won += 1,
            Some(_) => self.scheduling.copies_lost += 1,
            None => {}
        }
        let holders = self.workers.values_mut().filter(|worker| {
            worker.id != winner && worker.is_present() && worker.in_flight.contains(&unit)
        });
        for worker in holders {
            worker.cancel(unit, now);
            self.scheduling.cancelled += 1;
        }
    }

    /// `worker`'s connection closed as `departure` says: it goes, and its
    /// Worker Slot counts a failure.
    fn leave(&mut self, id: WorkerId, departure: Departure) {
        self.retire(id);
        let Some(worker) = self.workers.get(&id) else {
            return;
        };
        match departure {
            Departure::Failed => {
                warn!("{} failed", worker.name());
                self.slots.failed(&worker.slot());
            }
            Departure::Ended | Departure::Stopped | Departure::ProtocolError => {}
        }
    }

    /// Lets `worker` go: each of its Work Units in flight that no other
    /// Worker holds goes back to the front of the queue (arc42 §8.8).
    fn retire(&mut self, id: WorkerId) {
        let Some(worker) = self.workers.get_mut(&id).filter(|w| w.is_present()) else {
            return;
        };
        let name = worker.name().to_string();
        let held = worker.leave(Instant::now());
        let lost: Vec<WorkUnit> = held
            .into_iter()
            .filter(|unit| {
                !self
                    .workers
                    .values()
                    .any(|other| other.is_present() && other.in_flight.contains(unit))
            })
            .collect();
        if !lost.is_empty() {
            warn!(
                "{name} went away: its {} Work Units in flight go back to the queue",
                lost.len()
            );
            self.tail.assigned.retain(|unit, _| !lost.contains(unit));
            self.scheduling.reassigned += lost.len() as u64;
            self.queue.give_back(lost);
        }
    }

    /// Assigns each Worker as many Work Units as it asked for: from the
    /// queue while any is left, then copies of the oldest in flight that
    /// have none (ADR 0009).
    fn assign(&mut self) {
        let now = Instant::now();
        let ids: Vec<WorkerId> = self
            .workers
            .values()
            .filter(|w| w.is_present() && w.readiness == Readiness::Ready && w.demand > 0)
            .map(|w| w.id)
            .collect();
        for id in ids {
            self.fill(id, now);
        }
    }

    /// Assigns the Worker `id` Work Units at `now` while it asks for more
    /// and any is left.
    fn fill(&mut self, id: WorkerId, now: Instant) {
        while self.workers[&id].demand > 0
            && let Some(unit) = self.next_for(id)
        {
            self.workers
                .get_mut(&id)
                .into_iter()
                .for_each(|worker| worker.assign(unit, now));
        }
    }

    /// The next Work Unit for the Worker `id`: the queue's, or a copy of
    /// the oldest Work Unit in flight that another Worker holds and that
    /// has no copy yet.
    fn next_for(&mut self, id: WorkerId) -> Option<WorkUnit> {
        if let Some(unit) = self.queue.next() {
            self.tail.assigned.insert(unit, self.tail.next);
            self.tail.next += 1;
            return Some(unit);
        }
        let held = &self.workers[&id].in_flight;
        let (&unit, _) = self
            .tail
            .assigned
            .iter()
            .filter(|(unit, _)| !held.contains(unit) && !self.tail.copies.contains_key(unit))
            .min_by_key(|&(_, &order)| order)?;
        self.tail.copies.insert(unit, id);
        Some(unit)
    }

    /// Tells every Worker how the Render Job ended, `JobFinished` or a
    /// cancellation, and waits for them to close, for at most
    /// [`CLOSE_WAIT`].
    async fn close(&mut self, inbox: &mut UnboundedReceiver<Event>, ending: Ending) {
        let message = match ending {
            Ending::Finished => ToWorker::JobFinished,
            Ending::Cancelled => ToWorker::Stop,
        };
        for worker in self.workers.values().filter(|w| w.is_present()) {
            let _ = worker.sender.send(message);
        }
        let deadline = time::sleep(CLOSE_WAIT);
        tokio::pin!(deadline);
        let now = Instant::now();
        while self.workers.values().any(Worker::is_present) {
            tokio::select! {
                Some(event) = inbox.recv() => match event {
                    Event::Left { worker, .. } => {
                        if let Some(worker) = self.workers.get_mut(&worker) {
                            worker.leave(now);
                        }
                    }
                    // Dropping its verdict drops a Worker that arrives now.
                    Event::Arrived { .. }
                    | Event::Exited { .. }
                    | Event::Message { .. }
                    | Event::Heartbeat { .. } => {}
                },
                () = &mut deadline => return,
            }
        }
    }

    /// Sends the last status and makes the outcome of the Render Job
    /// submitted as `submission` says.
    fn finish(mut self, ending: Ending, submission: Submission) -> RenderJobOutcome {
        let now = Instant::now();
        let status = self.last.take().unwrap_or_else(|| self.status(now));
        (self.show)(&status, Moment::Ended);
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
        Rendered {
            outcome,
            film_hash,
            render_time,
            merge_time: self.progress.merge_time(),
            merged_samples: self.progress.merged_samples(),
            cluster: Cluster {
                name: self.options.cluster.to_string(),
                dirty_builds: self.options.dirty_builds,
                workers: self
                    .workers
                    .values()
                    .map(|worker| (worker.id, worker.report_row(now)))
                    .collect(),
                refused: self.slots.refusals(),
                slots: self.slots.summary(),
                scheduling: self.scheduling,
            },
            tile_costs: self.progress.tile_costs(),
        }
    }
}
