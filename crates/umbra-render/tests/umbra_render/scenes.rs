//! The Scenes and Render Job Options the tests render.

use std::num::NonZeroU32;

use umbra_math::Point2i;
use umbra_render::{Cancellation, Rendered, Renderer, WorkResult};
use umbra_scene::{FilmParameters, RenderJobOptions, Scene, SceneBuilder, Seed};

/// A camera-only Scene of 40 × 24 pixels: with 16-pixel Tiles, three
/// across and two down, the right and bottom ones cut short.
pub(crate) fn camera_only() -> Scene {
    let mut builder = SceneBuilder::new();
    builder.film(FilmParameters {
        resolution: Point2i::new(40, 24),
    });
    builder.build().expect("a camera-only Scene")
}

/// 16-pixel Tiles and `spp` Pixel Samples per pixel in Passes of `per_pass`.
pub(crate) fn options(spp: u32, per_pass: u32) -> RenderJobOptions {
    RenderJobOptions {
        seed: Seed(0),
        tile_size: NonZeroU32::new(16).expect("16 is not zero"),
        spp_per_pass: NonZeroU32::new(per_pass).expect("a Pass has a Pixel Sample"),
        spp: NonZeroU32::new(spp).expect("a pixel has a Pixel Sample"),
    }
}

/// Every Work Result of the Render Job, in the order of its Work Units.
pub(crate) fn render_all(renderer: &Renderer<'_>) -> Vec<WorkResult> {
    let cancel = Cancellation::new();
    renderer
        .decomposition()
        .work_units()
        .map(|unit| match renderer.render(unit, &cancel) {
            Rendered::Done(result) => result,
            Rendered::Cancelled => panic!("{unit:?} cancelled with no cancellation"),
        })
        .collect()
}
