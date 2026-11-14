//! The renderer of a Worker's Render Device (ADR 0033). Only the CPU's so
//! far: the GPU's, which renders Work Units in batches, joins with the GPU
//! Local Worker (#128).

use umbra_cpu::Render;
use umbra_render::{Cancellation, WorkUnit};
use umbra_scene::{RenderJobOptions, Scene};

use crate::error::Error;

/// The hardware a Worker renders on.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum RenderDevice {
    /// The cores of one NUMA node.
    Cpu,
}

/// A Worker's renderer, for its Render Device.
#[derive(Debug)]
pub(crate) enum Renderer<'s> {
    /// The CPU's.
    Cpu(umbra_cpu::Renderer<'s>),
}

impl<'s> Renderer<'s> {
    /// The renderer on `device` of a Render Job of `scene` with `options`.
    pub(crate) fn new(
        device: RenderDevice,
        scene: &'s Scene,
        options: &RenderJobOptions,
    ) -> Result<Self, Error> {
        Ok(match device {
            RenderDevice::Cpu => Self::Cpu(umbra_cpu::Renderer::new(scene, options)?),
        })
    }

    /// Renders `unit` into its Work Result, unless `cancel` stops it first,
    /// with the threads' time on it.
    pub(crate) fn render(&self, unit: WorkUnit, cancel: &Cancellation) -> Render {
        match self {
            Self::Cpu(renderer) => renderer.render(unit, cancel),
        }
    }
}
