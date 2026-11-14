//! `umbra worker`: a Worker that joins the Render Jobs of its Cluster as
//! they are announced, until it is stopped; or, with `--coordinator`, the
//! one Render Job of the Coordinator at that address, which is how `umbra
//! render` runs the Local Worker of each Render Device (arc42 §8.13). A
//! Worker writes nothing on its own to the terminal: what it says, it
//! logs.

use umbra_cluster::{CoordinatorAddress, Threads, WorkerOptions};
use umbra_render::Cancellation;

use crate::build_identity::build_identity;
use crate::cli::WorkerArgs;
use crate::error::Error;

/// Serves Render Jobs as `args` say, until `stop` is made, or, with
/// `--coordinator`, until its one Render Job ends.
pub(crate) fn work(args: &WorkerArgs, stop: &Cancellation) -> Result<(), Error> {
    let options = WorkerOptions {
        render_device: args.render_device,
        threads: args.nthreads.map_or(Threads::All, Threads::Count),
        coordinator: args
            .coordinator
            .map_or(CoordinatorAddress::Discovered, CoordinatorAddress::Given),
        cluster: args.cluster.clone(),
        build_identity: build_identity(),
    };
    umbra_cluster::work(&options, stop)?;
    Ok(())
}
