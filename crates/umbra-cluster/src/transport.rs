//! The typed messages between the Coordinator and a Worker, which QUIC
//! carries (arc42 §8.13). Inside a Worker's process, its connection's relay
//! and its render loop pass them over a pair of channels; on the
//! Coordinator's side, every Worker's messages arrive as [`Event`]s on one
//! channel.

use std::fmt::{self, Display};
use std::time::Duration;

use serde::{Deserialize, Serialize};

use tokio::sync::mpsc::{UnboundedReceiver, UnboundedSender, unbounded_channel};
use umbra_render::{WorkResult, WorkUnit};

use crate::children::ProcessId;
use crate::device::RenderDevice;

/// From the Coordinator to a Worker.
#[derive(Debug, Clone, Copy)]
pub(crate) enum ToWorker {
    /// Render this Work Unit.
    Assign(WorkUnit),
    /// Every Work Unit is merged: stop.
    JobFinished,
    /// The Render Job is cancelled: stop what is in flight, send nothing
    /// more.
    Cancel,
}

/// From a Worker to the Coordinator.
#[derive(Debug)]
pub(crate) enum ToCoordinator {
    /// Add this many Work Units to the Worker's demand.
    RequestWork(u32),
    /// A finished Work Unit, how long the Worker took to render it, and
    /// the time its threads spent on it, summed over them: metadata outside
    /// the Work Result's content (arc42 §8.10).
    WorkResult {
        result: WorkResult,
        render_time: Duration,
        thread_time: Duration,
    },
    /// The Worker cannot continue, for this reason; it goes away next.
    Fatal(String),
}

/// A Worker, as the Coordinator numbers them in the order they join.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Serialize, Deserialize)]
pub(crate) struct WorkerId(pub(crate) u32);

impl WorkerId {
    /// The identifier after this one.
    pub(crate) fn next(self) -> Self {
        Self(self.0 + 1)
    }
}

impl Display for WorkerId {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}", self.0)
    }
}

/// What happens to the Coordinator's Workers, in the order it happens.
#[derive(Debug)]
pub(crate) enum Event {
    /// A Worker is ready for its first `RequestWork`.
    Joined(Joined),
    /// A message from a joined Worker.
    Message {
        worker: WorkerId,
        message: ToCoordinator,
    },
    /// The Worker's connection closed: nothing more arrives from it.
    Left { worker: WorkerId },
    /// The process of a Local Worker exited, joined or not.
    Exited { process: ProcessId },
}

/// A Worker that joined: who it is, as its `Hello` says, and the channel
/// that reaches it.
#[derive(Debug)]
pub(crate) struct Joined {
    pub(crate) worker: WorkerId,
    pub(crate) device: RenderDevice,
    /// Its CPU threads; none on a GPU.
    pub(crate) threads: u32,
    pub(crate) process: ProcessId,
    pub(crate) to_worker: UnboundedSender<ToWorker>,
}

/// The relay's end of a Worker's connection, in its process.
#[derive(Debug)]
pub(crate) struct CoordinatorEnd {
    pub(crate) to_worker: UnboundedSender<ToWorker>,
    pub(crate) from_worker: UnboundedReceiver<ToCoordinator>,
}

/// The render loop's end of a Worker's connection.
#[derive(Debug)]
pub(crate) struct WorkerEnd {
    pub(crate) to_coordinator: UnboundedSender<ToCoordinator>,
    pub(crate) from_coordinator: UnboundedReceiver<ToWorker>,
}

/// The channels between a Worker's relay and its render loop.
pub(crate) fn connect() -> (CoordinatorEnd, WorkerEnd) {
    let (to_worker, from_coordinator) = unbounded_channel();
    let (to_coordinator, from_worker) = unbounded_channel();
    (
        CoordinatorEnd {
            to_worker,
            from_worker,
        },
        WorkerEnd {
            to_coordinator,
            from_coordinator,
        },
    )
}
