//! How a Render Device's render of a Work Unit ends, the request that stops
//! it early, and its progress as it goes: the same for every Render Device
//! (ADR 0033).

use std::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering};

use crate::work_result::WorkResult;

/// A request to stop rendering, shared between whoever cancels and the
/// renders it stops. Rendering checks it between rows of a Tile.
#[derive(Debug, Default)]
pub struct Cancellation {
    cancelled: AtomicBool,
}

impl Cancellation {
    /// A request not made yet.
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// Makes the request: renders still running stop at their next row.
    pub fn cancel(&self) {
        self.cancelled.store(true, Ordering::Relaxed);
    }

    /// Whether the request has been made.
    #[must_use]
    pub fn is_cancelled(&self) -> bool {
        self.cancelled.load(Ordering::Relaxed)
    }
}

/// What a render of a Work Unit reports as it goes (arc42 §8.10): the
/// Pixel Samples it has completed, and its threads rendering a row now.
/// The render counts; a Worker's heartbeat reads.
#[derive(Debug, Default)]
pub struct Meter {
    samples: AtomicU64,
    busy_threads: AtomicU32,
}

impl Meter {
    /// Nothing counted yet.
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// A thread starts rendering a row.
    pub fn row_started(&self) {
        self.busy_threads.fetch_add(1, Ordering::Relaxed);
    }

    /// A thread finished a row, of `samples` Pixel Samples, none if it was
    /// cancelled.
    pub fn row_finished(&self, samples: u64) {
        self.busy_threads.fetch_sub(1, Ordering::Relaxed);
        self.samples.fetch_add(samples, Ordering::Relaxed);
    }

    /// The Pixel Samples completed so far.
    #[must_use]
    pub fn samples(&self) -> u64 {
        self.samples.load(Ordering::Relaxed)
    }

    /// The threads rendering a row now.
    #[must_use]
    pub fn busy_threads(&self) -> u32 {
        self.busy_threads.load(Ordering::Relaxed)
    }
}

/// How a render of a Work Unit ended.
#[derive(Debug, Clone, PartialEq)]
pub enum Rendered {
    /// Every Pixel Sample was computed.
    Done(WorkResult),
    /// It was cancelled before the end; what was computed is dropped.
    Cancelled,
}
