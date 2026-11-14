//! The GPU's Renderer renders the camera-only Scene File as the CPU's does,
//! up to noise (#127, ADR 0026): one Work Result per Work Unit of a batch,
//! black, with the CPU's filter weights.

use std::num::NonZeroU32;
use std::path::Path;

use umbra_gpu::Renderer;
use umbra_parser::{Overrides, parse};
use umbra_render::{Cancellation, Rendered, WorkResult, WorkUnit};
use umbra_scene::{RenderJobOptions, Scene};

/// The camera-only Scene File, 40 × 24 pixels at 16 Pixel Samples, cut into
/// 16-pixel Tiles and Passes of 4: 6 Tiles × 4 Passes, more Work Units than
/// one batch.
fn camera_only() -> (Scene, RenderJobOptions) {
    let file = Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/gpu/scenes/camera-only.pbrt");
    let parsed = parse(&file, &Overrides::default()).expect("the camera-only Scene File parses");
    let options = RenderJobOptions {
        tile_size: NonZeroU32::new(16).expect("16 is not zero"),
        spp_per_pass: NonZeroU32::new(4).expect("4 is not zero"),
        ..parsed.options
    };
    (parsed.scene, options)
}

/// The GPU's Renderer on the first GPU.
fn gpu_renderer(scene: &Scene, options: &RenderJobOptions) -> Renderer {
    Renderer::new(0, scene, options).unwrap_or_else(|error| panic!("no GPU Renderer: {error}"))
}

/// Every Work Result of the Render Job on the GPU, in batches of the
/// Renderer's capacity, in the order of their Work Units.
fn render_on_gpu(renderer: &Renderer) -> Vec<WorkResult> {
    let units: Vec<WorkUnit> = renderer.decomposition().work_units().collect();
    let capacity = usize::try_from(Renderer::CAPACITY).expect("a capacity fits usize");
    assert!(
        units.len() > capacity,
        "the Render Job takes more than one batch"
    );
    let cancel = Cancellation::new();
    units
        .chunks(capacity)
        .flat_map(|batch| {
            let outcomes = renderer
                .render(batch, &cancel)
                .unwrap_or_else(|error| panic!("the GPU failed: {error}"));
            assert_eq!(outcomes.len(), batch.len(), "one Rendered per Work Unit");
            outcomes
        })
        .map(|rendered| match rendered {
            Rendered::Done(result) => result,
            Rendered::Cancelled => panic!("cancelled with no cancellation"),
        })
        .collect()
}

/// Every Work Result of the Render Job on the CPU, in the order of their
/// Work Units.
fn render_on_cpu(scene: &Scene, options: &RenderJobOptions) -> Vec<WorkResult> {
    let renderer = umbra_cpu::Renderer::new(scene, options).expect("a CPU Renderer");
    let cancel = Cancellation::new();
    renderer
        .decomposition()
        .work_units()
        .map(|unit| match renderer.render(unit, &cancel).rendered {
            Rendered::Done(result) => result,
            Rendered::Cancelled => panic!("cancelled with no cancellation"),
        })
        .collect()
}

#[test]
fn the_camera_only_scene_file_renders_on_the_gpu_as_on_the_cpu() {
    let (scene, options) = camera_only();
    let renderer = gpu_renderer(&scene, &options);
    let gpu = render_on_gpu(&renderer);
    let cpu = render_on_cpu(&scene, &options);

    assert_eq!(gpu.len(), cpu.len(), "one Work Result per Work Unit");
    for (g, c) in gpu.iter().zip(&cpu) {
        let unit = g.unit();
        assert_eq!(
            unit,
            c.unit(),
            "Work Results in the order of their Work Units"
        );
        assert_eq!(g.pixels().len(), c.pixels().len(), "the pixels of {unit:?}");
        for (i, (gpu_pixel, cpu_pixel)) in g.pixels().iter().zip(c.pixels()).enumerate() {
            assert_eq!(gpu_pixel.rgb, [0.0; 3], "pixel {i} of {unit:?} is black");
            assert!(
                agree(gpu_pixel.weight, cpu_pixel.weight),
                "pixel {i} of {unit:?}: filter weights {} on the GPU, {} on the CPU",
                gpu_pixel.weight,
                cpu_pixel.weight
            );
        }
    }
}

/// Whether a pixel's sums of filter weights on the GPU and on the CPU agree.
///
/// The camera-only Scene File lets a test observe no more than this: its
/// pixels are black, and importance sampling makes every Pixel Sample's
/// filter weight the filter's integral, up to rounding. So the sums agree
/// to a few `f32` ULP when each pixel got its Pass's Pixel Samples, however
/// the GPU rounds them (ADR 0028). Which samples the GPU draws shows only
/// once a Scene has radiance; until then, the core's storage tests check
/// that the tables the GPU samples are moved intact.
fn agree(gpu: f64, cpu: f64) -> bool {
    cpu > 0.0 && ((gpu - cpu) / cpu).abs() < 1e-6
}

#[test]
fn a_cancelled_batch_returns_no_work_result() {
    let (scene, options) = camera_only();
    let renderer = gpu_renderer(&scene, &options);
    let units: Vec<WorkUnit> = renderer.decomposition().work_units().take(3).collect();
    let cancel = Cancellation::new();
    cancel.cancel();
    let outcomes = renderer
        .render(&units, &cancel)
        .unwrap_or_else(|error| panic!("the GPU failed: {error}"));
    assert_eq!(outcomes, vec![Rendered::Cancelled; 3]);
}
