//! The status line (arc42 §8.10): one aggregate line and one line for the
//! Local Worker, redrawn in place on a terminal, or logged as `info`
//! events about every 10 s otherwise.

use std::fmt::{self, Display};
use std::io::Write;
use std::time::{Duration, Instant};

use tracing::info;

use crate::progress::{Eta, Progress};

/// How often the status is logged when it is not redrawn.
const LOG_INTERVAL: Duration = Duration::from_secs(10);

/// Where the status line goes.
pub enum StatusLine<'w> {
    /// Redrawn in place by this writer: each update is its two lines, then
    /// a flush, and the writer shows them in place of the previous ones.
    Redrawn(&'w mut dyn Write),
    /// Logged as `info` events, about every 10 s and once at the end.
    Logged,
}

impl fmt::Debug for StatusLine<'_> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Redrawn(_) => f.write_str("Redrawn"),
            Self::Logged => f.write_str("Logged"),
        }
    }
}

/// The status line of one Render Job, as it is shown.
#[derive(Debug)]
pub(crate) struct Status<'w> {
    line: StatusLine<'w>,
    threads: usize,
    last_logged: Option<Instant>,
}

impl<'w> Status<'w> {
    /// The status of a Render Job whose Local Worker has `threads`.
    pub(crate) fn new(line: StatusLine<'w>, threads: usize) -> Self {
        Self {
            line,
            threads,
            last_logged: None,
        }
    }

    /// The Local Worker's threads.
    pub(crate) fn threads(&self) -> usize {
        self.threads
    }

    /// Shows `progress` at `now`: redraws it, or logs it if the last log
    /// is old enough.
    pub(crate) fn show(&mut self, progress: &Progress<'_>, now: Instant) {
        match self.line {
            StatusLine::Redrawn(_) => self.redraw(progress, now),
            StatusLine::Logged if self.is_log_due(now) => self.log(progress, now),
            StatusLine::Logged => {}
        }
    }

    /// Whether [`LOG_INTERVAL`] has passed since the last log, at `now`.
    fn is_log_due(&self, now: Instant) -> bool {
        self.last_logged
            .is_none_or(|t| now.duration_since(t) >= LOG_INTERVAL)
    }

    /// Shows `progress` at the end of the Render Job, whatever the time
    /// since the last.
    pub(crate) fn finish(&mut self, progress: &Progress<'_>, now: Instant) {
        match self.line {
            StatusLine::Redrawn(_) => self.redraw(progress, now),
            StatusLine::Logged => self.log(progress, now),
        }
    }

    /// Logs the aggregate line and the Local Worker's.
    fn log(&mut self, progress: &Progress<'_>, now: Instant) {
        info!("{}", aggregate(progress, now));
        info!("{}", local_worker(progress, self.threads));
        self.last_logged = Some(now);
    }

    /// Writes both lines and flushes them, for the writer to show in
    /// place of the previous ones.
    fn redraw(&mut self, progress: &Progress<'_>, now: Instant) {
        let StatusLine::Redrawn(out) = &mut self.line else {
            return;
        };
        // The status line is informative: a closed terminal does not stop
        // the Render Job.
        let _ = write!(
            out,
            "{}\n{}\n",
            aggregate(progress, now),
            local_worker(progress, self.threads)
        );
        let _ = out.flush();
    }
}

/// `Pass 2/4 · 30/60 Work Units · 50.0 % · 1.25 M Pixel Samples/s · 0:00:12
/// elapsed · ETA 0:00:12`, with `~` before an ETA from Pass 0.
pub(crate) fn aggregate(progress: &Progress<'_>, now: Instant) -> String {
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
pub(crate) fn local_worker(progress: &Progress<'_>, threads: usize) -> String {
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
