//! What can go wrong on the GPU.

use cudarc::driver::DriverError;
use thiserror::Error;

/// An error of the GPU path: CUDA, OptiX, or the kernels' build.
#[derive(Debug, Error)]
pub enum Error {
    /// A CUDA driver call failed, or no CUDA driver or device was found.
    #[error("CUDA: {0}")]
    Cuda(#[from] DriverError),
    /// The driver's OptiX library, `libnvoptix.so.1`, could not be loaded:
    /// no NVIDIA driver, or a container without the `graphics` driver
    /// capability (arc42 §7).
    #[error("cannot load OptiX: {0}")]
    OptixLibrary(#[source] libloading::Error),
    /// The driver's OptiX has no function of this name at ABI 105: a driver
    /// older than OptiX 9.0.
    #[error("the driver's OptiX has no `{0}` at ABI 105 (OptiX 9.0)")]
    OptixFunction(&'static str),
    /// An OptiX call failed.
    #[error("`{call}` failed: {result}{}{log}", if .log.is_empty() { "" } else { "\n" })]
    Optix {
        /// The OptiX function.
        call: &'static str,
        /// The name OptiX gives the result code.
        result: String,
        /// The compile log, for the calls that compile.
        log: String,
    },
    /// The kernels' PTX still calls `libdevice` functions it should have
    /// inlined: `umbra-gpu` was built without `libdevice.10.bc` (see its
    /// README).
    #[error(
        "the kernels were built without libdevice.10.bc; set CUDA_PATH or UMBRA_LIBDEVICE and rebuild"
    )]
    UnlinkedPtx,
    /// More of something than one call takes: rays per launch, or program
    /// groups per pipeline.
    #[error("too many {0} for one call")]
    TooMany(&'static str),
    /// A hit record came back with a kind the kernels never write.
    #[error("a hit record has the unknown kind {0}")]
    HitKind(u32),
}
