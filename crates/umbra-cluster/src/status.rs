//! The status of a running Render Job (arc42 §8.10): its content only, one
//! aggregate line and one line per Worker. The caller shows it as it sees
//! fit.

use std::fmt::{self, Display};
use std::time::{Duration, Instant};

use crate::progress::{Eta, Progress};

/// What a running Render Job shows: the aggregate line, then one line per
/// Worker.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Status {
    lines: Vec<String>,
}

/// Whether any Worker is in the Render Job, or about to join it.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum Staffing {
    /// A Worker is present, or a Local Worker is starting.
    Workers,
    /// None: the Render Job waits for one to join.
    NoWorkers,
}

impl Status {
    /// The status of `progress` at `now`, `in_flight` Pixel Samples being
    /// done of the Work Units in flight, with `workers`, one line each.
    pub(crate) fn of(
        progress: &Progress<'_>,
        in_flight: u64,
        staffing: Staffing,
        workers: Vec<String>,
        now: Instant,
    ) -> Self {
        let first = match staffing {
            Staffing::Workers => aggregate(progress, in_flight, now),
            Staffing::NoWorkers => format!(
                "waiting: no Workers · {} elapsed",
                Clock(progress.elapsed(now))
            ),
        };
        Self {
            lines: std::iter::once(first).chain(workers).collect(),
        }
    }

    /// Its lines, the aggregate one first.
    pub fn lines(&self) -> impl Iterator<Item = &str> {
        self.lines.iter().map(String::as_str)
    }
}

/// When a [`Status`] is sent: while the Render Job runs, or once at its
/// end, finished or cancelled.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Moment {
    /// About every 250 ms while it runs.
    Running,
    /// Once, when it has ended.
    Ended,
}

/// `Pass 2/4 · 30/60 Work Units · 50.0 % · 1.25 M Pixel Samples/s · 0:00:12
/// elapsed · ETA 0:00:12`, with `~` before an ETA from Pass 0; the
/// percentage counts the Pixel Samples merged and the `in_flight` ones
/// done.
fn aggregate(progress: &Progress<'_>, in_flight: u64, now: Instant) -> String {
    let total = progress.total_samples().max(1);
    let done = (progress.merged_samples() + in_flight).min(total);
    format!(
        "Pass {}/{} · {}/{} Work Units · {:.1} % · {} Pixel Samples/s · {} elapsed · ETA {}",
        progress.pass().0 + 1,
        progress.passes(),
        progress.merged_units(),
        progress.total_units(),
        percent(done, total),
        Si(progress.rate()),
        Clock(progress.elapsed(now)),
        EtaText(progress.eta()),
    )
}

/// `part` over `total`, in percent.
#[expect(
    clippy::cast_precision_loss,
    reason = "a percentage shown to one decimal"
)]
fn percent(part: u64, total: u64) -> f64 {
    part as f64 * 100.0 / total as f64
}

/// A rate with an SI prefix: `950`, `12.5 k`, `1.25 M`, `3.10 G`.
pub(crate) struct Si(pub(crate) f64);

impl Display for Si {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let value = self.0;
        let (scaled, prefix) = [(1e9, " G"), (1e6, " M"), (1e3, " k")]
            .into_iter()
            .find(|&(scale, _)| value >= scale)
            .map_or((value, ""), |(scale, prefix)| (value / scale, prefix));
        let digits = match prefix {
            "" => 0,
            _ => 2,
        };
        write!(f, "{scaled:.digits$}{prefix}")
    }
}

/// A duration as `H:MM:SS`.
struct Clock(Duration);

impl Display for Clock {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let seconds = self.0.as_secs();
        let (hours, minutes) = (seconds / 3600, seconds / 60 % 60);
        write!(f, "{hours}:{minutes:02}:{:02}", seconds % 60)
    }
}

/// An ETA: `--` while unknown, `~0:01:30` while provisional.
struct EtaText(Eta);

impl Display for EtaText {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self.0 {
            Eta::Unknown => f.write_str("--"),
            Eta::Provisional(eta) => write!(f, "~{}", Clock(eta)),
            Eta::Measured(eta) => write!(f, "{}", Clock(eta)),
        }
    }
}
