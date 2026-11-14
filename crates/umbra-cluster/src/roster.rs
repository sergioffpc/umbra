//! What the Coordinator knows of each Worker that joined its Render Job:
//! how to reach it, what it was given, and what it did, from its
//! heartbeats and Work Results (arc42 §8.10). The status line shows one
//! line per Worker, and the Render Job Report one row.

use std::fmt::{self, Display};
use std::time::{Duration, Instant};

use tokio::sync::mpsc::UnboundedSender;
use tracing::{info, warn};
use umbra_render::{BuildIdentity, Decomposition, Tree, WorkUnit};

use crate::device::RenderDevice;
use crate::progress::Rate;
use crate::report::{Occupancy, Seconds};
use crate::slot::WorkerSlot;
use crate::status::Si;
use crate::transport::{ToWorker, WorkerId};
use crate::wire::{Heartbeat, Hello, Loaded, Loading};

/// How long a heartbeat may be late before its Worker shows as `stale`:
/// two of the heartbeats sent every second.
const STALE_AFTER: Duration = Duration::from_secs(2);

/// How the Coordinator judges a Worker's heartbeats (arc42 §8.8, §8.10).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Liveness {
    /// No heartbeat for this long, and a Worker is dead: 5 s by default.
    pub death_time: Duration,
    /// No progress for this long, and a Worker is shown as `stalled`:
    /// 30 s by default. Nothing is reassigned for it.
    pub stall_time: Duration,
}

impl Default for Liveness {
    fn default() -> Self {
        Self {
            death_time: Duration::from_secs(5),
            stall_time: Duration::from_secs(30),
        }
    }
}

/// Whether the Coordinator started a Worker itself.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum WorkerKind {
    /// A child process of the Coordinator.
    Local,
    /// A Worker that found the Render Job by itself.
    Remote,
}

impl Display for WorkerKind {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(match self {
            Self::Local => "Local Worker",
            Self::Remote => "Worker",
        })
    }
}

/// Whether a Worker has asked for its first Work Units, which it does once
/// it has loaded the Scene and can render (arc42 §8.13).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum Readiness {
    Loading,
    Ready,
}

/// Whether a Worker is still in the Render Job.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum Presence {
    Present,
    /// Its connection closed at this time.
    Gone(Instant),
}

/// A Worker that joined: how to reach it, what it asked for and was not
/// given yet, what it was given and has not answered, and what it did.
#[derive(Debug)]
pub(crate) struct Worker {
    pub(crate) id: WorkerId,
    pub(crate) kind: WorkerKind,
    pub(crate) hello: Box<Hello>,
    pub(crate) sender: UnboundedSender<ToWorker>,
    pub(crate) readiness: Readiness,
    pub(crate) presence: Presence,
    pub(crate) demand: u32,
    /// Its Work Units in flight, in the order it was given them.
    pub(crate) in_flight: Vec<WorkUnit>,
    stall: Stall,
    tally: Tally,
}

/// Whether a Worker's stall has been logged.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Stall {
    /// It is not stalled, or not since it last made progress.
    Unlogged,
    /// It is stalled, and a warning said so.
    Logged,
}

/// What a Worker did, for the status line and the report.
#[derive(Debug)]
struct Tally {
    joined: Instant,
    /// Its Work Results merged, and their Pixel Samples.
    merged_units: u64,
    merged_samples: u64,
    /// The Pixel Samples of every Work Result it sent, merged or not.
    rendered_samples: u64,
    /// The time its threads spent on those, summed over them.
    thread_time: Duration,
    busy_since: Option<Instant>,
    busy: Duration,
    /// When it last sent a heartbeat, or joined.
    heard: Instant,
    heartbeat: Option<Heartbeat>,
    /// Its progress at the last tick, and when it last changed or the
    /// Worker last had nothing to do.
    progress: u64,
    grew: Instant,
    rate: Rate,
}

/// What a Worker is doing, as the status line shows it (arc42 §8.10).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum State {
    Loading(Option<Loading>),
    Alive,
    Stale,
    Stalled,
    Dead,
}

impl Worker {
    /// The Worker `hello` describes, admitted as `id` at `now`, reached
    /// through `sender`.
    pub(crate) fn new(
        id: WorkerId,
        kind: WorkerKind,
        hello: Box<Hello>,
        sender: UnboundedSender<ToWorker>,
        now: Instant,
    ) -> Self {
        Self {
            id,
            kind,
            hello,
            sender,
            readiness: Readiness::Loading,
            presence: Presence::Present,
            demand: 0,
            in_flight: Vec::new(),
            stall: Stall::Unlogged,
            tally: Tally {
                joined: now,
                merged_units: 0,
                merged_samples: 0,
                rendered_samples: 0,
                thread_time: Duration::ZERO,
                busy_since: None,
                busy: Duration::ZERO,
                heard: now,
                heartbeat: None,
                progress: 0,
                grew: now,
                rate: Rate::new(now),
            },
        }
    }

    /// Its Render Device.
    pub(crate) fn device(&self) -> RenderDevice {
        self.hello.render_device
    }

    /// Its Worker Slot.
    pub(crate) fn slot(&self) -> WorkerSlot {
        self.hello.slot()
    }

    /// Whether it is still in the Render Job.
    pub(crate) fn is_present(&self) -> bool {
        self.presence == Presence::Present
    }

    /// Whether it is in the Render Job and holds `unit`.
    pub(crate) fn holds(&self, unit: WorkUnit) -> bool {
        self.is_present() && self.in_flight.contains(&unit)
    }

    /// Adds `count` Work Units to its demand, logging its first request: it
    /// is ready.
    pub(crate) fn request(&mut self, count: u32) {
        if self.readiness == Readiness::Loading {
            info!("{} on {} is ready", self.name(), self.device());
            self.readiness = Readiness::Ready;
        }
        self.demand = self.demand.saturating_add(count);
    }

    /// `Local Worker 0` or `Worker 3`.
    pub(crate) fn name(&self) -> impl Display {
        let (kind, id) = (self.kind, self.id);
        fmt::from_fn(move |f| write!(f, "{kind} {id}"))
    }

    /// Its threads rendering a row, as its last heartbeat counts them.
    fn busy_threads(&self) -> u32 {
        match &self.tally.heartbeat {
            Some(Heartbeat::Rendering { busy_threads, .. }) => *busy_threads,
            Some(Heartbeat::Loading(_)) | None => 0,
        }
    }

    /// Gives it `unit` at `now`.
    pub(crate) fn assign(&mut self, unit: WorkUnit, now: Instant) {
        // A Worker gone away meanwhile gives it back when it leaves.
        let _ = self.sender.send(ToWorker::Assign(unit));
        if self.in_flight.is_empty() {
            self.tally.busy_since = Some(now);
        }
        self.in_flight.push(unit);
        self.demand = self.demand.saturating_sub(1);
    }

    /// Takes `unit` off its Work Units in flight at `now`; whether it held
    /// it.
    pub(crate) fn release(&mut self, unit: WorkUnit, now: Instant) -> bool {
        let Some(at) = self.in_flight.iter().position(|&held| held == unit) else {
            return false;
        };
        self.in_flight.remove(at);
        if self.in_flight.is_empty()
            && let Some(since) = self.tally.busy_since.take()
        {
            self.tally.busy += now.duration_since(since);
        }
        true
    }

    /// Tells it to drop `unit`, and takes it off its Work Units in flight.
    pub(crate) fn cancel(&mut self, unit: WorkUnit, now: Instant) {
        if self.release(unit, now) {
            let _ = self.sender.send(ToWorker::Cancel(unit));
        }
    }

    /// It sent a Work Result of `samples` Pixel Samples, its threads having
    /// spent `thread_time` on it, `merged` into the Film or not.
    pub(crate) fn rendered(&mut self, samples: u64, thread_time: Duration, merged: Merged) {
        self.tally.rendered_samples += samples;
        self.tally.thread_time += cpu_time(self.device(), thread_time);
        if merged == Merged::Yes {
            self.tally.merged_units += 1;
            self.tally.merged_samples += samples;
        }
    }

    /// It sent `heartbeat` at `now`.
    pub(crate) fn heard(&mut self, heartbeat: Heartbeat, now: Instant) {
        self.tally.heard = now;
        self.tally.heartbeat = Some(heartbeat);
    }

    /// Its connection closed at `now`: its Work Units in flight, to give
    /// back.
    pub(crate) fn leave(&mut self, now: Instant) -> Vec<WorkUnit> {
        self.presence = Presence::Gone(now);
        if let Some(since) = self.tally.busy_since.take() {
            self.tally.busy += now.duration_since(since);
        }
        self.demand = 0;
        std::mem::take(&mut self.in_flight)
    }

    /// The Pixel Samples completed of its Work Units in flight, as its
    /// last heartbeat counts them.
    pub(crate) fn in_flight_samples(&self) -> u64 {
        match &self.tally.heartbeat {
            Some(Heartbeat::Rendering { work_units, .. }) => work_units
                .iter()
                .filter(|(unit, _)| self.in_flight.contains(unit))
                .map(|&(_, samples)| samples)
                .sum(),
            Some(Heartbeat::Loading(_)) | None => 0,
        }
    }

    /// Takes in the progress it made up to `now` into its rate and its
    /// stall clock, and logs it once as it stalls, as `liveness` judges.
    pub(crate) fn tick(&mut self, now: Instant, liveness: Liveness) {
        let progress = self.progress();
        let rendered = self.tally.rendered_samples + self.in_flight_samples();
        self.tally.rate.tick(now, rendered);
        // An idle Worker, ready with nothing in flight, is waiting, not
        // stalled; a Worker whose progress fell, its Work Units cancelled,
        // is moving.
        let idle = self.readiness == Readiness::Ready && self.in_flight.is_empty();
        if progress != self.tally.progress || idle {
            self.tally.progress = progress;
            self.tally.grew = now;
        }
        let stalled = self.state(now, liveness) == State::Stalled;
        match (stalled, self.stall) {
            (true, Stall::Unlogged) => {
                warn!(
                    "{} is stalled: no progress for {} s",
                    self.name(),
                    liveness.stall_time.as_secs()
                );
                self.stall = Stall::Logged;
            }
            (false, Stall::Logged) => self.stall = Stall::Unlogged,
            (true, Stall::Logged) | (false, Stall::Unlogged) => {}
        }
    }

    /// What grows while it does its job: the bytes of the Scene it has
    /// received while it loads, its Pixel Samples rendered afterwards.
    fn progress(&self) -> u64 {
        match (&self.tally.heartbeat, self.readiness) {
            (Some(Heartbeat::Loading(loading)), Readiness::Loading) => match *loading {
                Loading::Receiving { bytes } => bytes,
                Loading::Building { bytes } => bytes + 1,
            },
            (Some(_) | None, _) => self.tally.rendered_samples + self.in_flight_samples(),
        }
    }

    /// What it is doing at `now`.
    fn state(&self, now: Instant, liveness: Liveness) -> State {
        if !self.is_present() {
            return State::Dead;
        }
        let stuck = now.duration_since(self.tally.grew) >= liveness.stall_time;
        if now.duration_since(self.tally.heard) > STALE_AFTER {
            State::Stale
        } else if self.readiness == Readiness::Loading && stuck {
            State::Stalled
        } else if self.readiness == Readiness::Loading {
            State::Loading(match self.tally.heartbeat {
                Some(Heartbeat::Loading(loading)) => Some(loading),
                Some(Heartbeat::Rendering { .. }) | None => None,
            })
        } else if stuck && !self.in_flight.is_empty() {
            State::Stalled
        } else {
            State::Alive
        }
    }

    /// Its line on the status line at `now`, with its Worker Slot's
    /// `failures`: `Worker 1 · p620 node 0 cpu · 16 threads, 12 busy · 4
    /// in flight · 1.25 M Pixel Samples/s · heartbeat 0.4 s · alive · 0
    /// failures`.
    pub(crate) fn status_line(&self, now: Instant, liveness: Liveness, failures: u32) -> String {
        format!(
            "{} · {} · {} threads, {} busy · {} in flight · {} Pixel Samples/s · heartbeat {:.1} s · {} · {failures} failures",
            self.name(),
            self.slot(),
            self.hello.threads,
            self.busy_threads(),
            self.in_flight.len(),
            Si(self.tally.rate.per_second()),
            now.duration_since(self.tally.heard).as_secs_f64(),
            self.state(now, liveness),
        )
    }

    /// Its row in the Render Job Report, its presence ending at `end` at
    /// the latest.
    pub(crate) fn report_row(&self, end: Instant) -> String {
        let hello = &self.hello;
        let tally = &self.tally;
        let until = match self.presence {
            Presence::Present => end,
            Presence::Gone(at) => at.min(end),
        };
        let present = until.duration_since(tally.joined);
        let busy = tally.busy
            + tally.busy_since.map_or(Duration::ZERO, |since| {
                until.saturating_duration_since(since)
            });
        let threads = usize::try_from(hello.threads).unwrap_or(usize::MAX);
        format!(
            "{} on {}, {}, node {}, build {}, {} threads, {} Work Units, {} Pixel Samples, busy {}, idle {}, occupancy {}, {}",
            self.kind,
            hello.render_device,
            hello.host,
            hello.numa_node,
            Build(&hello.build_identity),
            hello.threads,
            tally.merged_units,
            tally.merged_samples,
            Seconds(busy),
            Seconds(present.saturating_sub(busy)),
            Occupancy::new(tally.thread_time, threads, present),
            LoadedText(self.loaded()),
        )
    }

    /// How it loaded the Scene, once a heartbeat has said so.
    fn loaded(&self) -> Option<Loaded> {
        match self.tally.heartbeat {
            Some(Heartbeat::Rendering { loaded, .. }) => Some(loaded),
            Some(Heartbeat::Loading(_)) | None => None,
        }
    }
}

/// Whether a Work Result went into the Film.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum Merged {
    Yes,
    No,
}

/// The share of `thread_time` that counts for the CPU's occupancy: all of
/// it on the CPU, none on a GPU.
fn cpu_time(device: RenderDevice, thread_time: Duration) -> Duration {
    match device {
        RenderDevice::Cpu => thread_time,
        RenderDevice::Gpu(_) => Duration::ZERO,
    }
}

/// The Pixel Samples of `unit` in `decomposition`.
pub(crate) fn pixel_samples(decomposition: &Decomposition, unit: WorkUnit) -> u64 {
    let pixels = decomposition.tile_bounds(unit.tile).area() as u64;
    pixels * decomposition.sample_indices(unit.pass).len() as u64
}

impl Display for State {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Loading(None) => f.write_str("loading"),
            Self::Loading(Some(Loading::Receiving { bytes })) => {
                write!(f, "loading (receiving {}B)", Si(to_f64(*bytes)))
            }
            Self::Loading(Some(Loading::Building { .. })) => {
                f.write_str("loading (building the Scene)")
            }
            Self::Alive => f.write_str("alive"),
            Self::Stale => f.write_str("stale"),
            Self::Stalled => f.write_str("stalled"),
            Self::Dead => f.write_str("dead"),
        }
    }
}

/// A Build Identity as the report shows it: `9bd75ce (clean)`.
struct Build<'a>(&'a BuildIdentity);

impl Display for Build<'_> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let commit = self.0.commit.get(..7).unwrap_or(&self.0.commit);
        let tree = match self.0.tree {
            Tree::Clean => "clean",
            Tree::Dirty => "dirty",
        };
        write!(f, "{commit} ({tree})")
    }
}

/// How a Worker loaded the Scene, as the report shows it.
struct LoadedText(Option<Loaded>);

impl Display for LoadedText {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self.0 {
            Some(loaded) => write!(
                f,
                "Scene replication {}, Scene build {}, Scene bytes {}",
                Seconds(loaded.replication_time),
                Seconds(loaded.build_time),
                loaded.bytes,
            ),
            None => f.write_str("Scene not loaded"),
        }
    }
}

/// `n` as an `f64`, for a size shown with a prefix.
#[expect(clippy::cast_precision_loss, reason = "a size shown to three digits")]
fn to_f64(n: u64) -> f64 {
    n as f64
}
