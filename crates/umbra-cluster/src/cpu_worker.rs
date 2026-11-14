//! A CPU Worker's render loop: it renders the Work Units it is assigned,
//! several at once on its own rayon pool (arc42 §8.8), each with its own
//! cancellation and progress in the Worker's [`Activity`].

use std::time::Instant;

use rayon::{Scope, ThreadPool};
use tokio::sync::mpsc::UnboundedSender;
use tracing::info_span;
use umbra_cpu::{Render, Renderer};
use umbra_render::{Rendered, WorkUnit};

use crate::activity::{Activity, InFlight};
use crate::transport::{ToCoordinator, ToWorker, WorkerEnd};

/// Renders with `renderer` until the Render Job finishes or is cancelled,
/// or the Coordinator goes away: it asks for as many Work Units as it
/// renders at once, and for one more for each it is done with, rendered
/// or cancelled.
pub(crate) fn work(
    renderer: &Renderer<'_>,
    pool: &ThreadPool,
    connection: WorkerEnd,
    activity: &Activity,
) {
    let WorkerEnd {
        to_coordinator,
        mut from_coordinator,
    } = connection;
    pool.in_place_scope(|scope| {
        // A send fails only once the Coordinator is gone, and then the
        // loop below ends too.
        let _ = to_coordinator.send(ToCoordinator::RequestWork(capacity(pool)));
        let renders = Renders {
            renderer,
            activity,
            to_coordinator: &to_coordinator,
        };
        while renders.take(from_coordinator.blocking_recv(), scope) == Flow::Continue {}
    });
}

/// Whether a render loop goes on.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Flow {
    Continue,
    Stop,
}

/// What a CPU Worker's renders share.
struct Renders<'r, 'a> {
    renderer: &'a Renderer<'r>,
    activity: &'a Activity,
    to_coordinator: &'a UnboundedSender<ToCoordinator>,
}

impl<'a> Renders<'_, 'a> {
    /// Takes in `message`, spawning a render into `scope` for an `Assign`.
    fn take<'s>(&self, message: Option<ToWorker>, scope: &Scope<'s>) -> Flow
    where
        'a: 's,
    {
        match message {
            Some(ToWorker::Assign(unit)) => {
                // In flight from now, so that a `Cancel` that comes before
                // its render starts stops it.
                let in_flight = self.activity.start(unit);
                let (renderer, activity) = (self.renderer, self.activity);
                let to_coordinator = self.to_coordinator.clone();
                scope.spawn(move |_| render(renderer, unit, &in_flight, activity, &to_coordinator));
                Flow::Continue
            }
            Some(ToWorker::Cancel(unit)) => {
                self.activity.cancel(unit);
                Flow::Continue
            }
            // Every Work Unit is merged, or nothing more is wanted: what is
            // still in flight is dropped, not sent.
            Some(ToWorker::JobFinished | ToWorker::Stop) | None => {
                self.activity.cancel_all();
                Flow::Stop
            }
        }
    }
}

/// Renders `unit`, stopped and counted by `in_flight`, and sends its Work
/// Result, with its render time and its threads' time, or nothing if it
/// was cancelled; then, done with it in `activity`, a credit for the next.
fn render(
    renderer: &Renderer<'_>,
    unit: WorkUnit,
    in_flight: &InFlight,
    activity: &Activity,
    to_coordinator: &UnboundedSender<ToCoordinator>,
) {
    let _span = info_span!("work_unit", tile = unit.tile.0, pass = unit.pass.0).entered();
    let start = Instant::now();
    let Render {
        rendered,
        thread_time,
    } = renderer.render(unit, &in_flight.cancel, &in_flight.meter);
    if let Rendered::Done(result) = rendered {
        let render_time = start.elapsed();
        let _ = to_coordinator.send(ToCoordinator::WorkResult {
            result,
            render_time,
            thread_time,
        });
    }
    activity.finish(unit);
    let _ = to_coordinator.send(ToCoordinator::RequestWork(1));
}

/// The Work Units a CPU Worker keeps in flight: about two per eight
/// threads, and at least one (arc42 §8.8).
fn capacity(pool: &ThreadPool) -> u32 {
    u32::try_from(pool.current_num_threads().div_ceil(4)).unwrap_or(u32::MAX)
}
