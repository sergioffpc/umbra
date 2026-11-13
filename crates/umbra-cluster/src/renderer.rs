//! The renderer of a Worker's Render Device: the CPU's or the GPU's, which
//! have the same shape (ADR 0033).

use umbra_render::{Cancellation, Rendered, WorkUnit};
use umbra_scene::{RenderJobOptions, Scene};

use crate::error::Error;

/// The hardware a Worker renders on.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum RenderDevice {
    /// The cores of one NUMA node.
    Cpu,
    /// One GPU.
    #[expect(
        dead_code,
        reason = "only the CPU Local Worker exists; the GPU's arrives with #128"
    )]
    Gpu,
}

/// A Worker's renderer, for its Render Device.
#[derive(Debug)]
pub(crate) enum Renderer<'s> {
    /// The CPU's.
    Cpu(umbra_cpu::Renderer<'s>),
    /// The GPU's.
    Gpu(umbra_gpu::Renderer<'s>),
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
            RenderDevice::Gpu => Self::Gpu(umbra_gpu::Renderer::new(scene, options)?),
        })
    }

    /// Renders `unit` into its Work Result, unless `cancel` stops it first.
    pub(crate) fn render(&self, unit: WorkUnit, cancel: &Cancellation) -> Rendered {
        match self {
            Self::Cpu(renderer) => renderer.render(unit, cancel),
            Self::Gpu(renderer) => renderer.render(unit, cancel),
        }
    }
}
