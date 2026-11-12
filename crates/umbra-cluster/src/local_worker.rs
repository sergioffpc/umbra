//! The CPU Local Worker: renders the Work Units it is assigned in this
//! process, several at once on its own rayon pool (arc42 §8.8).

use rayon::ThreadPool;
use tokio::sync::mpsc::UnboundedSender;
use umbra_render::{Cancellation, Rendered, Renderer, WorkUnit};

use crate::transport::{ToCoordinator, ToWorker, WorkerEnd};

/// Runs the Local Worker until the Render Job finishes or the Coordinator
/// goes away: it asks for as many Work Units as it renders at once, and
/// for one more after each Work Result.
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
        while let Some(ToWorker::Assign(unit)) = from_coordinator.blocking_recv() {
            let (to_coordinator, cancel) = (to_coordinator.clone(), &cancel);
            scope.spawn(move |_| render(renderer, unit, cancel, &to_coordinator));
        }
    });
}

/// Renders `unit` and sends its Work Result with a credit for the next.
fn render(
    renderer: &Renderer<'_>,
    unit: WorkUnit,
    cancel: &Cancellation,
    to_coordinator: &UnboundedSender<ToCoordinator>,
) {
    if let Rendered::Done(result) = renderer.render(unit, cancel) {
        let _ = to_coordinator.send(ToCoordinator::WorkResult(result));
        let _ = to_coordinator.send(ToCoordinator::RequestWork(1));
    }
}

/// The Work Units a CPU Worker keeps in flight: about two per eight
/// threads, and at least one (arc42 §8.8).
fn capacity(pool: &ThreadPool) -> u32 {
    u32::try_from(pool.current_num_threads().div_ceil(4)).unwrap_or(u32::MAX)
}
