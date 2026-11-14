//! The status of a running Render Job (arc42 §8.10): its content only, one
//! aggregate line and one line for the Local Worker. The caller shows it
//! as it sees fit.

use std::fmt::{self, Display};
use std::time::{Duration, Instant};

use crate::progress::{Eta, Progress};

/// What a running Render Job shows: the aggregate line, then one line per
/// Worker.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Status {
    lines: Vec<String>,
}

impl Status {
    /// The status of `progress` at `now`, the Local Worker having
    /// `threads`.
    pub(crate) fn of(progress: &Progress<'_>, threads: usize, now: Instant) -> Self {
        Self {
            lines: vec![aggregate(progress, now), local_worker(progress, threads)],
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
/// elapsed · ETA 0:00:12`, with `~` before an ETA from Pass 0.
fn aggregate(progress: &Progress<'_>, now: Instant) -> String {
    let total = progress.total_samples().max(1);
    format!(
        "Pass {}/{} · {}/{} Work Units · {:.1} % · {} Pixel Samples/s · {} elapsed · ETA {}",
        progress.pass().0 + 1,
        progress.passes(),
        progress.merged_units(),
        progress.total_units(),
        percent(progress.merged_samples(), total),
        Si(progress.rate()),
        Clock(progress.elapsed(now)),
        EtaText(progress.eta()),
    )
}

/// `Local Worker: 16 threads, 4 in flight, 1.25 M Pixel Samples/s, alive`.
fn local_worker(progress: &Progress<'_>, threads: usize) -> String {
    format!(
        "Local Worker: {threads} threads, {} in flight, {} Pixel Samples/s, alive",
        progress.in_flight(),
        Si(progress.rate()),
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
