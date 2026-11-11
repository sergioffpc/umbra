//! What can go wrong on the GPU.

use std::fmt;

use cudarc::driver::DriverError;

/// An error of the GPU path: CUDA, OptiX, or the kernels' build.
#[derive(Debug)]
pub enum Error {
    /// A CUDA driver call failed, or no CUDA driver or device was found.
    Cuda(DriverError),
    /// The driver's OptiX library, `libnvoptix.so.1`, could not be loaded:
    /// no NVIDIA driver, or a container without the `graphics` driver
    /// capability (arc42 §7).
    OptixLibrary(libloading::Error),
    /// The driver's OptiX has no function of this name at ABI 105: a driver
    /// older than OptiX 9.0.
    OptixFunction(&'static str),
    /// An OptiX call failed.
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
    UnlinkedPtx,
    /// More of something than one call takes: rays per launch, or program
    /// groups per pipeline.
    TooMany(&'static str),
    /// A hit record came back with a kind the kernels never write.
    HitKind(u32),
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Cuda(error) => write!(f, "CUDA: {error}"),
            Self::OptixLibrary(error) => write!(f, "cannot load OptiX: {error}"),
            Self::OptixFunction(name) => {
                write!(
                    f,
                    "the driver's OptiX has no `{name}` at ABI 105 (OptiX 9.0)"
                )
            }
            Self::Optix { call, result, log } if log.is_empty() => {
                write!(f, "`{call}` failed: {result}")
            }
            Self::Optix { call, result, log } => write!(f, "`{call}` failed: {result}\n{log}"),
            Self::UnlinkedPtx => write!(
                f,
                "the kernels were built without libdevice.10.bc; set CUDA_PATH or UMBRA_LIBDEVICE and rebuild"
            ),
            Self::TooMany(what) => write!(f, "too many {what} for one call"),
            Self::HitKind(kind) => write!(f, "a hit record has the unknown kind {kind}"),
        }
    }
}

impl std::error::Error for Error {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Self::Cuda(error) => Some(error),
            Self::OptixLibrary(error) => Some(error),
            Self::OptixFunction(_)
            | Self::Optix { .. }
            | Self::UnlinkedPtx
            | Self::TooMany(_)
            | Self::HitKind(_) => None,
        }
    }
}

impl From<DriverError> for Error {
    fn from(error: DriverError) -> Self {
        Self::Cuda(error)
    }
}
