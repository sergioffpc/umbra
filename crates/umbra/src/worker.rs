//! `umbra worker`: a Worker for one Render Job, connected to its
//! Coordinator at a given address (arc42 §8.13). `umbra render` runs one
//! per GPU as that GPU's Local Worker.

use umbra_render::Cancellation;

use crate::cli::WorkerArgs;
use crate::error::Error;

/// Serves the Render Job of the Coordinator `args` names on the Render
/// Device it names, until the Render Job ends or `stop` is made.
pub(crate) fn work(args: &WorkerArgs, stop: &Cancellation) -> Result<(), Error> {
    umbra_cluster::work(args.render_device, args.coordinator, stop)?;
    Ok(())
}
