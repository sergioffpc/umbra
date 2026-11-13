//! The in-memory transport between the Coordinator and its Local Worker:
//! the typed messages of the wire protocol, unencoded (arc42 §8.13).

use std::time::Duration;

use tokio::sync::mpsc::{UnboundedReceiver, UnboundedSender, unbounded_channel};
use umbra_render::{WorkResult, WorkUnit};

/// From the Coordinator to a Worker.
#[derive(Debug)]
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
    /// A finished Work Unit, and how long the Worker took to render it:
    /// metadata outside the Work Result's content (arc42 §8.10).
    WorkResult {
        result: WorkResult,
        render_time: Duration,
    },
}

/// The Coordinator's end of a connection.
#[derive(Debug)]
pub(crate) struct CoordinatorEnd {
    pub(crate) to_worker: UnboundedSender<ToWorker>,
    pub(crate) from_worker: UnboundedReceiver<ToCoordinator>,
}

/// A Worker's end of a connection.
#[derive(Debug)]
pub(crate) struct WorkerEnd {
    pub(crate) to_coordinator: UnboundedSender<ToCoordinator>,
    pub(crate) from_coordinator: UnboundedReceiver<ToWorker>,
}

/// A connection between the Coordinator and a Local Worker.
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
