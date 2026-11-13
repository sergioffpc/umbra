//! Why a Render Job did not finish.

use std::io;

use rayon::ThreadPoolBuildError;
use thiserror::Error;

/// Why a Render Job did not finish.
#[derive(Debug, Error)]
#[non_exhaustive]
pub enum Error {
    /// The Render Job cannot be cut into Work Units.
    #[error("cannot render the Render Job: {0}")]
    Render(#[from] umbra_render::Error),
    /// The GPU cannot render the Render Job.
    #[error("cannot render on the GPU: {0}")]
    Gpu(#[from] umbra_gpu::Error),
    /// The runtime could not start.
    #[error("cannot start the Coordinator's runtime: {0}")]
    Runtime(#[source] io::Error),
    /// The Local Worker's threads could not start.
    #[error("cannot start the Local Worker's threads: {0}")]
    ThreadPool(#[source] ThreadPoolBuildError),
    /// Every Worker went away before the last Work Unit was merged.
    #[error("every Worker went away before the end")]
    NoWorkers,
}
