//! `umbra worker`: a Worker for one Render Job, connected to its
//! Coordinator at a given address (arc42 §8.13). `umbra render` runs one
//! per Render Device of its machine as that device's Local Worker. A Worker
//! writes nothing on its own to the terminal: what it says, it logs.

use umbra_cluster::{Threads, WorkerOptions};
use umbra_render::Cancellation;

use crate::cli::WorkerArgs;
use crate::error::Error;

/// Serves the Render Job of the Coordinator `args` names on the Render
/// Device it names, until the Render Job ends or `stop` is made.
pub(crate) fn work(args: &WorkerArgs, stop: &Cancellation) -> Result<(), Error> {
    let options = WorkerOptions {
        render_device: args.render_device,
        threads: args.nthreads.map_or(Threads::All, Threads::Count),
        coordinator: args.coordinator,
    };
    umbra_cluster::work(&options, stop)?;
    Ok(())
}
