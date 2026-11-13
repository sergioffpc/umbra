//! How a Render Device's render of a Work Unit ends, and the request that
//! stops it early: the same for every Render Device (ADR 0033).

use std::sync::atomic::{AtomicBool, Ordering};

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

/// How a render of a Work Unit ended.
#[derive(Debug, Clone, PartialEq)]
pub enum Rendered {
    /// Every Pixel Sample was computed.
    Done(WorkResult),
    /// It was cancelled before the end; what was computed is dropped.
    Cancelled,
}
