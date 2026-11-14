//! How `umbra` shows the status of a running Render Job (arc42 §8.10):
//! redrawn at the bottom of stderr where it can be, or logged as `info`
//! events about every 10 s and once at the end otherwise.

use std::io::Write;
use std::time::{Duration, Instant};

use tracing::info;
use umbra_cluster::{Moment, Status};

use crate::footer::{Footer, Redrawn};

/// How often the status is logged when it is not redrawn.
const LOG_INTERVAL: Duration = Duration::from_secs(10);

/// Where the status of a Render Job goes.
#[derive(Debug)]
pub(crate) enum StatusLine<'f> {
    /// Redrawn at the bottom of stderr, at every update.
    Redrawn(Redrawn<'f>),
    /// Logged as `info` events, one per line: about every 10 s, and once at
    /// the end.
    Logged {
        /// When it was last logged.
        last: Option<Instant>,
    },
}

impl<'f> StatusLine<'f> {
    /// The status line on `stderr`: redrawn where the footer shows, logged
    /// where it does not.
    pub(crate) fn on(stderr: &'f Footer) -> Self {
        if stderr.is_hidden() {
            Self::Logged { last: None }
        } else {
            Self::Redrawn(stderr.redrawn())
        }
    }

    /// Shows `status`, sent at `moment`.
    pub(crate) fn show(&mut self, status: &Status, moment: Moment) {
        match self {
            Self::Redrawn(out) => redraw(out, status),
            Self::Logged { last } => log(last, status, moment),
        }
    }
}

/// Logs the lines of `status`, sent at `moment`, if it is due, and records
/// when in `last`.
fn log(last: &mut Option<Instant>, status: &Status, moment: Moment) {
    let now = Instant::now();
    if is_log_due(*last, moment, now) {
        status.lines().for_each(|line| info!("{line}"));
        *last = Some(now);
    }
}

/// Writes the lines of `status` to `out`, which shows them in place of the
/// previous ones.
fn redraw(out: &mut Redrawn<'_>, status: &Status) {
    // The status line is informative: a closed terminal does not stop the
    // Render Job.
    for line in status.lines() {
        let _ = writeln!(out, "{line}");
    }
    let _ = out.flush();
}

/// Whether a status sent at `moment` is logged at `now`, the last having
/// been logged at `last`: always at the end, and at most every
/// [`LOG_INTERVAL`] before.
fn is_log_due(last: Option<Instant>, moment: Moment, now: Instant) -> bool {
    match moment {
        Moment::Running => last.is_none_or(|t| now.duration_since(t) >= LOG_INTERVAL),
        Moment::Ended => true,
    }
}
