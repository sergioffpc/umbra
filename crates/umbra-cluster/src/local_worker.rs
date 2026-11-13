//! The CPU Local Worker: renders the Work Units it is assigned in this
//! process, several at once on its own rayon pool (arc42 §8.8).

use std::time::Instant;

use rayon::ThreadPool;
use tokio::sync::mpsc::{UnboundedReceiver, UnboundedSender};
use tracing::info_span;
use umbra_render::{Cancellation, Rendered, WorkUnit};

use crate::renderer::Renderer;
use crate::transport::{ToCoordinator, ToWorker, WorkerEnd};

/// Runs the Local Worker until the Render Job finishes or is cancelled, or
/// the Coordinator goes away: it asks for as many Work Units as it renders
/// at once, and for one more after each Work Result.
pub(crate) fn work(renderer: &Renderer<'_>, pool: &ThreadPool, connection: WorkerEnd) {
    let WorkerEnd {
        to_coordinator,
        mut from_coordinator,
    } = connection;
    let cancel = Cancellation::new();
    pool.in_place_scope(|scope| {
        // A send fails only once the Coordinator is gone, and then the
        // loop below ends too.
        let _ = to_coordinator.send(ToCoordinator::RequestWork(capacity(pool)));
        while let Some(unit) = next_unit(&mut from_coordinator, &cancel) {
            let (to_coordinator, cancel) = (to_coordinator.clone(), &cancel);
            scope.spawn(move |_| render(renderer, unit, cancel, &to_coordinator));
        }
    });
}

/// The next Work Unit the Coordinator assigns; `None` once the Render Job
/// is finished or cancelled, `cancel` being made then.
fn next_unit(
    from_coordinator: &mut UnboundedReceiver<ToWorker>,
    cancel: &Cancellation,
) -> Option<WorkUnit> {
    match from_coordinator.blocking_recv()? {
        ToWorker::Assign(unit) => Some(unit),
        ToWorker::JobFinished => None,
        ToWorker::Cancel => {
            cancel.cancel();
            None
        }
    }
}

/// Renders `unit` and sends its Work Result, with its render time and a
/// credit for the next.
fn render(
    renderer: &Renderer<'_>,
    unit: WorkUnit,
    cancel: &Cancellation,
    to_coordinator: &UnboundedSender<ToCoordinator>,
) {
    let _span = info_span!("work_unit", tile = unit.tile.0, pass = unit.pass.0).entered();
    let start = Instant::now();
    if let Rendered::Done(result) = renderer.render(unit, cancel) {
        let render_time = start.elapsed();
        let _ = to_coordinator.send(ToCoordinator::WorkResult {
            result,
            render_time,
        });
        let _ = to_coordinator.send(ToCoordinator::RequestWork(1));
    }
}

/// The Work Units a CPU Worker keeps in flight: about two per eight
/// threads, and at least one (arc42 §8.8).
fn capacity(pool: &ThreadPool) -> u32 {
    u32::try_from(pool.current_num_threads().div_ceil(4)).unwrap_or(u32::MAX)
}
