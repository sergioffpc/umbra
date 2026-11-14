//! `tracing` to stderr (arc42 §8.10): pretty text on a terminal and JSON
//! lines otherwise, or the format `--log-format` names, filtered by
//! `--log-filter`; with the `tracy` feature, the spans go to Tracy too
//! (arc42 §8.4).

use std::io::{self, IsTerminal};
use std::sync::Arc;

use clap::ValueEnum;
use tracing_subscriber::EnvFilter;
use tracing_subscriber::fmt;
use tracing_subscriber::prelude::*;

use crate::footer::Footer;

/// How the logs are written.
#[derive(Debug, Clone, Copy, PartialEq, Eq, ValueEnum)]
pub(crate) enum LogFormat {
    /// Text for a person to read: each event over several lines, with its
    /// fields and its spans each on a line of their own.
    Pretty,
    /// Text for a person to read: each event on one line, its spans before
    /// its message.
    Compact,
    /// One JSON object per line.
    Json,
}

impl LogFormat {
    /// `pretty` on a terminal, `json` otherwise.
    fn for_stderr() -> Self {
        if io::stderr().is_terminal() {
            Self::Pretty
        } else {
            Self::Json
        }
    }
}

/// Checks that `filter` is a `RUST_LOG`-style filter, for `clap`.
pub(crate) fn parse_filter(filter: &str) -> Result<String, String> {
    EnvFilter::try_new(filter)
        .map(|_| filter.to_owned())
        .map_err(|error| error.to_string())
}

/// Sends every `tracing` event that passes `filter` to `stderr`, above its
/// status line, in `format` or the one that suits stderr.
pub(crate) fn init(format: Option<LogFormat>, filter: &str, stderr: Arc<Footer>) {
    // The target and the source location name Umbra's own modules and
    // lines, and on the events relayed from Local Workers, the relay.
    let ansi = io::stderr().is_terminal();
    let layer = match format.unwrap_or_else(LogFormat::for_stderr) {
        LogFormat::Pretty => fmt::layer()
            .pretty()
            .map_event_format(|format| format.with_target(false).with_source_location(false))
            .with_writer(stderr)
            .with_ansi(ansi)
            .boxed(),
        LogFormat::Compact => fmt::layer()
            .compact()
            .with_target(false)
            .with_writer(stderr)
            .with_ansi(ansi)
            .boxed(),
        LogFormat::Json => fmt::layer().json().with_writer(stderr).boxed(),
    };
    let registry = tracing_subscriber::registry()
        .with(EnvFilter::new(filter))
        .with(layer);
    #[cfg(feature = "tracy")]
    let registry = registry.with(tracing_tracy::TracyLayer::default());
    registry.init();
}
