//! The GPU's Renderer: the same shape as the CPU's (ADR 0033), a Work Unit
//! in and its Work Result out, for the GPU Worker.

use std::convert::Infallible;
use std::marker::PhantomData;

use umbra_render::{Cancellation, Rendered, WorkUnit};
use umbra_scene::{RenderJobOptions, Scene};

use crate::Error;

/// Renders the Work Units of a Render Job of one Scene into Work Results on
/// one GPU.
///
/// The wavefront that renders them is not here yet, so none can be built:
/// [`Renderer::new`] refuses every Scene, and the type has no value.
#[derive(Debug)]
pub struct Renderer<'s> {
    scene: PhantomData<&'s Scene>,
    unbuilt: Infallible,
}

impl<'s> Renderer<'s> {
    /// The Renderer of a Render Job of `scene` with `options`.
    ///
    /// # Errors
    ///
    /// [`Error::NoRenderer`] for every Scene, until the GPU renders them.
    pub fn new(scene: &'s Scene, options: &RenderJobOptions) -> Result<Self, Error> {
        let _ = (scene, options);
        Err(Error::NoRenderer)
    }

    /// Renders `unit` into its Work Result, unless `cancel` stops it first.
    #[must_use]
    pub fn render(&self, unit: WorkUnit, cancel: &Cancellation) -> Rendered {
        let _ = (unit, cancel, self.scene);
        match self.unbuilt {}
    }
}
