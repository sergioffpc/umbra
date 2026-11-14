//! What both sides run on: a `tokio` runtime of their own, on the caller's
//! thread, and the tick at which they look at what they cannot await.

use std::time::Duration;

use tokio::runtime::{Builder, Runtime};
use tokio::time::{self, Interval, MissedTickBehavior};

use crate::error::Error;

/// How often each side checks what it cannot await: a cancellation, a
/// stop, the status to show.
const TICK: Duration = Duration::from_millis(250);

/// A current-thread runtime with its I/O, timers and child processes.
pub(crate) fn runtime() -> Result<Runtime, Error> {
    Builder::new_current_thread()
        .enable_all()
        .build()
        .map_err(Error::Runtime)
}

/// A tick every [`TICK`], which skips the ticks it misses rather than
/// bursting to catch up.
pub(crate) fn ticker() -> Interval {
    let mut ticker = time::interval(TICK);
    ticker.set_missed_tick_behavior(MissedTickBehavior::Delay);
    ticker
}
