#![doc = include_str!("../README.md")]

mod coordinator;
mod local_worker;
mod report;
mod transport;

pub use coordinator::{
    ClusterError, CoordinatorOptions, RenderJob, RenderJobOutcome, Threads, coordinate,
};
pub use report::{Outcome, RenderJobReport};
