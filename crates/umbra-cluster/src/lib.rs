#![doc = include_str!("../README.md")]

mod coordinator;
mod error;
mod local_worker;
mod progress;
mod report;
mod status;
mod transport;

pub use coordinator::{CoordinatorOptions, RenderJob, RenderJobOutcome, Threads, coordinate};
pub use error::Error;
pub use report::{Outcome, Phase, RenderJobReport, SchemaVersion};
pub use status::StatusLine;
