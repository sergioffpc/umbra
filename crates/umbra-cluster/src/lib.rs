#![doc = include_str!("../README.md")]

mod coordinator;
mod error;
mod local_worker;
mod report;
mod transport;

pub use coordinator::{CoordinatorOptions, RenderJob, RenderJobOutcome, Threads, coordinate};
pub use error::Error;
pub use report::{Outcome, RenderJobReport, SchemaVersion};
