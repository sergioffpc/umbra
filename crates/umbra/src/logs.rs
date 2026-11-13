//! `tracing` to stderr (arc42 §8.10): human-readable on a terminal, JSON
//! lines otherwise, filtered by `--log`; with the `tracy` feature, the
//! spans go to Tracy too (arc42 §8.4).

use std::io::{self, IsTerminal};

use clap::ValueEnum;
use tracing_subscriber::EnvFilter;
use tracing_subscriber::fmt;
use tracing_subscriber::prelude::*;

/// How the logs are written.
#[derive(Debug, Clone, Copy, PartialEq, Eq, ValueEnum)]
pub(crate) enum LogFormat {
    /// Text for a person to read.
    Human,
    /// One JSON object per line.
    Json,
}

impl LogFormat {
    /// `human` on a terminal, `json` otherwise.
    fn for_stderr() -> Self {
        if io::stderr().is_terminal() {
            Self::Human
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

/// Sends every `tracing` event that passes `filter` to stderr, in `format`
/// or the one that suits stderr.
pub(crate) fn init(format: Option<LogFormat>, filter: &str) {
    let (human, json) = match format.unwrap_or_else(LogFormat::for_stderr) {
        LogFormat::Human => (
            Some(
                fmt::layer()
                    .with_writer(io::stderr)
                    .with_ansi(io::stderr().is_terminal()),
            ),
            None,
        ),
        LogFormat::Json => (None, Some(fmt::layer().json().with_writer(io::stderr))),
    };
    let registry = tracing_subscriber::registry()
        .with(EnvFilter::new(filter))
        .with(human)
        .with(json);
    #[cfg(feature = "tracy")]
    let registry = registry.with(tracing_tracy::TracyLayer::default());
    registry.init();
}
