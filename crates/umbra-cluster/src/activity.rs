//! What a Worker is doing, shared between its render loop, which changes
//! it, and its heartbeat, which tells the Coordinator every second (arc42
//! §8.10, §8.13): loading the Scene, then rendering its Work Units in
//! flight, each with its own cancellation and progress.

use std::collections::BTreeMap;
use std::sync::{Arc, Mutex, MutexGuard, PoisonError};

use tokio::sync::Notify;
use umbra_render::{Cancellation, Meter, WorkUnit};

use crate::wire::{Heartbeat, Loaded, Loading};

/// A Work Unit in flight on a Worker: what stops it, and what counts its
/// progress.
#[derive(Debug, Default)]
pub(crate) struct InFlight {
    pub(crate) cancel: Cancellation,
    pub(crate) meter: Meter,
}

/// What a Worker is doing.
#[derive(Debug)]
pub(crate) struct Activity {
    phase: Mutex<Phase>,
    /// Wakes the heartbeat when the Worker becomes ready, so that the
    /// Coordinator learns how it loaded without waiting a second.
    changed: Notify,
}

/// Loading, then rendering.
#[derive(Debug)]
enum Phase {
    Loading(Loading),
    Rendering {
        loaded: Loaded,
        units: BTreeMap<WorkUnit, Arc<InFlight>>,
    },
}

impl Activity {
    /// Receiving the Scene, nothing received yet.
    pub(crate) fn new() -> Self {
        Self {
            phase: Mutex::new(Phase::Loading(Loading::Receiving { bytes: 0 })),
            changed: Notify::new(),
        }
    }

    /// The phase, even if a render thread panicked while holding it.
    fn phase(&self) -> MutexGuard<'_, Phase> {
        self.phase.lock().unwrap_or_else(PoisonError::into_inner)
    }

    /// The Worker is at `loading`.
    pub(crate) fn loading(&self, loading: Loading) {
        *self.phase() = Phase::Loading(loading);
    }

    /// The Worker loaded the Scene as `loaded` says, and renders.
    pub(crate) fn loaded(&self, loaded: Loaded) {
        *self.phase() = Phase::Rendering {
            loaded,
            units: BTreeMap::new(),
        };
        self.changed.notify_one();
    }

    /// Waits until the Worker becomes ready.
    pub(crate) async fn changed(&self) {
        self.changed.notified().await;
    }

    /// The Worker starts rendering `unit`: its cancellation and meter.
    pub(crate) fn start(&self, unit: WorkUnit) -> Arc<InFlight> {
        let in_flight = Arc::new(InFlight::default());
        if let Phase::Rendering { units, .. } = &mut *self.phase() {
            units.insert(unit, Arc::clone(&in_flight));
        }
        in_flight
    }

    /// The Worker is done with `unit`, rendered or dropped.
    pub(crate) fn finish(&self, unit: WorkUnit) {
        if let Phase::Rendering { units, .. } = &mut *self.phase() {
            units.remove(&unit);
        }
    }

    /// Cancels `unit` if it is still in flight.
    pub(crate) fn cancel(&self, unit: WorkUnit) {
        if let Phase::Rendering { units, .. } = &*self.phase()
            && let Some(in_flight) = units.get(&unit)
        {
            in_flight.cancel.cancel();
        }
    }

    /// Cancels every Work Unit in flight.
    pub(crate) fn cancel_all(&self) {
        if let Phase::Rendering { units, .. } = &*self.phase() {
            units
                .values()
                .map(|in_flight| &in_flight.cancel)
                .for_each(Cancellation::cancel);
        }
    }

    /// Its Work Units in flight.
    pub(crate) fn in_flight(&self) -> usize {
        match &*self.phase() {
            Phase::Loading(_) => 0,
            Phase::Rendering { units, .. } => units.len(),
        }
    }

    /// The heartbeat that says what the Worker is doing now.
    pub(crate) fn heartbeat(&self) -> Heartbeat {
        match &*self.phase() {
            Phase::Loading(loading) => Heartbeat::Loading(*loading),
            Phase::Rendering { loaded, units } => Heartbeat::Rendering {
                loaded: *loaded,
                work_units: units
                    .iter()
                    .map(|(&unit, in_flight)| (unit, in_flight.meter.samples()))
                    .collect(),
                busy_threads: units
                    .values()
                    .map(|in_flight| in_flight.meter.busy_threads())
                    .sum(),
            },
        }
    }
}
