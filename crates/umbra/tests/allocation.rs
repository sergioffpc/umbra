//! The counting-allocator test (ADR 0008, arc42 §8.7): rendering allocates
//! per Work Unit, never per Pixel Sample, so a Work Unit with twice the
//! Pixel Samples makes the same number of allocations. Every integrator
//! joins it; `volpath` is the only one so far.
//!
//! `dhat`'s allocator counts every heap block of the process, so this file
//! is a test binary of its own, with a single test.

use std::num::NonZeroU32;

use rayon::ThreadPoolBuilder;
use umbra_cpu::Renderer;
use umbra_math::Point2i;
use umbra_render::{Cancellation, Meter, PassIndex, Rendered, TileIndex, WorkUnit};
use umbra_scene::{FilmParameters, RenderJobOptions, SceneBuilder, Seed};

#[global_allocator]
static ALLOCATOR: dhat::Alloc = dhat::Alloc;

/// The heap blocks allocated while rendering one Work Unit of a single-Pass
/// Render Job of `spp` Pixel Samples per pixel, on one thread: the second
/// render, after a first has warmed up the thread pool.
fn blocks_per_work_unit(spp: u32) -> u64 {
    let mut builder = SceneBuilder::new();
    builder.film(FilmParameters {
        resolution: Point2i::new(32, 32),
    });
    let scene = builder.build().expect("a camera-only Scene");
    let spp = NonZeroU32::new(spp).expect("a pixel has a Pixel Sample");
    let options = RenderJobOptions {
        seed: Seed(0),
        tile_size: NonZeroU32::new(16).expect("16 is not zero"),
        spp_per_pass: spp,
        spp,
    };
    let renderer = Renderer::new(&scene, &options).expect("a Renderer");
    let pool = ThreadPoolBuilder::new()
        .num_threads(1)
        .build()
        .expect("a thread pool");
    let unit = WorkUnit {
        tile: TileIndex(3),
        pass: PassIndex(0),
    };
    let cancel = Cancellation::new();
    pool.install(|| {
        let warm_up = renderer.render(unit, &cancel, &Meter::new()).rendered;
        let before = dhat::HeapStats::get().total_blocks;
        let measured = renderer.render(unit, &cancel, &Meter::new()).rendered;
        let blocks = dhat::HeapStats::get().total_blocks - before;
        assert!(matches!(measured, Rendered::Done(_)), "rendered");
        assert_eq!(measured, warm_up, "the same Work Result twice");
        blocks
    })
}

#[test]
fn rendering_allocates_per_work_unit_not_per_pixel_sample() {
    let _profiler = dhat::Profiler::builder().testing().build();
    let n = blocks_per_work_unit(4);
    let two_n = blocks_per_work_unit(8);
    assert!(n > 0, "the Work Result buffer is allocated");
    assert_eq!(n, two_n, "allocations for N and 2N Pixel Samples");
}
