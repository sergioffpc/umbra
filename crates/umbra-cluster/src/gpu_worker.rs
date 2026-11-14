//! A GPU Worker's render loop: it keeps [`Renderer::CAPACITY`] Work Units
//! in flight and renders whatever it has been assigned as one wavefront
//! batch (ADR 0026, arc42 §8.8).

use std::time::{Duration, Instant};

use tokio::sync::mpsc::error::TryRecvError;
use tokio::sync::mpsc::{UnboundedReceiver, UnboundedSender};
use tracing::info_span;
use umbra_gpu::Renderer;
use umbra_render::{Cancellation, Rendered, WorkUnit};

use crate::activity::Activity;
use crate::transport::{ToCoordinator, ToWorker, WorkerEnd};

/// How a GPU Worker's loop ended.
#[derive(Debug)]
pub(crate) enum End {
    /// The Render Job finished or was cancelled, or the Coordinator went
    /// away.
    Done,
    /// The GPU failed: the Worker cannot continue.
    Failed(umbra_gpu::Error),
}

/// Renders with `renderer` until the Render Job finishes or is cancelled,
/// the Coordinator goes away, or the GPU fails: it asks for
/// [`Renderer::CAPACITY`] Work Units, then for one more per Work Result.
pub(crate) fn work(renderer: &Renderer, connection: WorkerEnd, activity: &Activity) -> End {
    let WorkerEnd {
        to_coordinator,
        mut from_coordinator,
    } = connection;
    let cancel = Cancellation::new();
    // A send fails only once the Coordinator is gone, and then the next
    // receive ends the loop too.
    let _ = to_coordinator.send(ToCoordinator::RequestWork(Renderer::CAPACITY));
    loop {
        let Some(batch) = next_batch(&mut from_coordinator, &to_coordinator, &cancel) else {
            return End::Done;
        };
        for &unit in &batch {
            activity.start(unit);
        }
        let start = Instant::now();
        let results = match info_span!("batch", work_units = batch.len())
            .in_scope(|| renderer.render(&batch, &cancel))
        {
            Ok(results) => results,
            Err(error) => return End::Failed(error),
        };
        let times = render_times(renderer, &batch, start.elapsed());
        for &unit in &batch {
            activity.finish(unit);
        }
        results
            .into_iter()
            .zip(times)
            .for_each(|(result, time)| send(result, time, &to_coordinator));
    }
}

/// Sends `rendered`, if the batch was not cancelled, with its share of the
/// batch's render time, `render_time`, and a credit for the next.
fn send(
    rendered: Rendered,
    render_time: Duration,
    to_coordinator: &UnboundedSender<ToCoordinator>,
) {
    if let Rendered::Done(result) = rendered {
        // No CPU thread renders a GPU's Work Units.
        let _ = to_coordinator.send(ToCoordinator::WorkResult {
            result,
            render_time,
            thread_time: Duration::ZERO,
        });
        let _ = to_coordinator.send(ToCoordinator::RequestWork(1));
    }
}

/// Every Work Unit assigned so far and not cancelled, waiting for the
/// first; `None` once the Render Job is finished or cancelled, `cancel`
/// being made then. A Work Unit cancelled before it is batched is dropped,
/// and its credit sent back on `to_coordinator`; one cancelled after, in a
/// batch already rendered, is ignored: its Work Result went, or goes,
/// anyway.
fn next_batch(
    from_coordinator: &mut UnboundedReceiver<ToWorker>,
    to_coordinator: &UnboundedSender<ToCoordinator>,
    cancel: &Cancellation,
) -> Option<Vec<WorkUnit>> {
    let mut batch = Vec::new();
    loop {
        let message = if batch.is_empty() {
            from_coordinator.blocking_recv()?
        } else {
            match from_coordinator.try_recv() {
                Ok(message) => message,
                Err(TryRecvError::Empty) => return Some(batch),
                Err(TryRecvError::Disconnected) => return None,
            }
        };
        match message {
            ToWorker::Assign(unit) => batch.push(unit),
            ToWorker::Cancel(unit) => drop_cancelled(&mut batch, unit, to_coordinator),
            ToWorker::JobFinished => return None,
            ToWorker::Stop => {
                cancel.cancel();
                return None;
            }
        }
    }
}

/// Drops `unit` from `batch`, if it is there, and sends its credit back on
/// `to_coordinator`.
fn drop_cancelled(
    batch: &mut Vec<WorkUnit>,
    unit: WorkUnit,
    to_coordinator: &UnboundedSender<ToCoordinator>,
) {
    let before = batch.len();
    batch.retain(|&held| held != unit);
    if batch.len() < before {
        let _ = to_coordinator.send(ToCoordinator::RequestWork(1));
    }
}

/// Each Work Unit's share of `time`, the render time of their batch, by
/// its Pixel Samples.
fn render_times(renderer: &Renderer, batch: &[WorkUnit], time: Duration) -> Vec<Duration> {
    let decomposition = renderer.decomposition();
    let samples: Vec<u64> = batch
        .iter()
        .map(|unit| {
            let pixels = decomposition.tile_bounds(unit.tile).area();
            let indices = decomposition.sample_indices(unit.pass).len();
            u64::try_from(pixels * indices).unwrap_or(u64::MAX)
        })
        .collect();
    let total: u64 = samples.iter().sum();
    samples
        .into_iter()
        .map(|n| match total {
            0 => Duration::ZERO,
            total => time.mul_f64(to_f64(n) / to_f64(total)),
        })
        .collect()
}

/// `n` as an `f64`, for a share of a time.
#[expect(clippy::cast_precision_loss, reason = "a share of a time")]
fn to_f64(n: u64) -> f64 {
    n as f64
}
