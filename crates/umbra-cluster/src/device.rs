//! The Render Devices: the one a Worker renders on, and the ones the
//! Coordinator starts Local Workers on (ADR 0026).

use std::fmt::{self, Display};
use std::str::FromStr;

use serde::{Deserialize, Serialize};
use tracing::warn;

use crate::error::Error;

/// A GPU, by its CUDA ordinal among the devices the process sees.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Serialize, Deserialize)]
pub struct GpuOrdinal(pub u32);

impl From<GpuOrdinal> for usize {
    fn from(GpuOrdinal(n): GpuOrdinal) -> Self {
        Self::try_from(n).expect("a u32 fits in a usize")
    }
}

/// The hardware a Worker renders on: `cpu` or `gpu:N` on its command line.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Serialize, Deserialize)]
pub enum RenderDevice {
    /// The cores of the machine.
    Cpu,
    /// One GPU.
    Gpu(GpuOrdinal),
}

impl Display for RenderDevice {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Cpu => f.write_str("cpu"),
            Self::Gpu(GpuOrdinal(n)) => write!(f, "gpu:{n}"),
        }
    }
}

impl FromStr for RenderDevice {
    type Err = Error;

    fn from_str(s: &str) -> Result<Self, Self::Err> {
        let unknown = || Error::UnknownRenderDevice(s.to_owned());
        match s.split_once(':') {
            None if s == "cpu" => Ok(Self::Cpu),
            Some(("gpu", n)) => n
                .parse()
                .map(|n| Self::Gpu(GpuOrdinal(n)))
                .map_err(|_| unknown()),
            None | Some(_) => Err(unknown()),
        }
    }
}

/// The Render Devices of its own machine the Coordinator starts Local
/// Workers on (`--render-devices`): a Coordinator option, not a Render Job
/// Option, as it does not change the expected image (ADR 0026).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum RenderDevices {
    /// None chosen: the CPU and every GPU, or the CPU alone, with one
    /// warning, when no GPU is usable.
    #[default]
    Usable,
    /// The CPU alone.
    Cpu,
    /// Every GPU, which must be usable.
    Gpu,
    /// The CPU and every GPU, which must be usable.
    CpuAndGpu,
}

impl RenderDevices {
    /// Whether the CPU gets a Local Worker.
    #[must_use]
    pub fn includes_cpu(self) -> bool {
        match self {
            Self::Usable | Self::Cpu | Self::CpuAndGpu => true,
            Self::Gpu => false,
        }
    }

    /// The GPUs of this machine that get a Local Worker: every usable one,
    /// unless the CPU is chosen alone.
    ///
    /// # Errors
    ///
    /// [`Error::NoGpu`] when a GPU is chosen and none is usable.
    pub fn gpus(self) -> Result<Vec<GpuOrdinal>, Error> {
        let count = match self {
            Self::Cpu => return Ok(Vec::new()),
            Self::Usable => match umbra_gpu::device_count() {
                Ok(count) => count,
                Err(reason) => {
                    warn!("no usable GPU ({reason}): rendering on the CPU alone");
                    return Ok(Vec::new());
                }
            },
            Self::Gpu | Self::CpuAndGpu => umbra_gpu::device_count().map_err(Error::NoGpu)?,
        };
        Ok((0..count)
            .map_while(|n| u32::try_from(n).ok())
            .map(GpuOrdinal)
            .collect())
    }
}
