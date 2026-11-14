//! Seam 3: the Renderer turns each Work Unit into its Work Result.

use std::time::{Duration, Instant};

use rayon::ThreadPoolBuilder;
use umbra_cpu::Renderer;
use umbra_render::{Cancellation, Film, FilmHash, Meter, PassIndex, Rendered, TileIndex, WorkUnit};
use umbra_scene::Scene;

use crate::scenes::{camera_only, options, render_all};

#[test]
fn a_camera_only_scene_renders_black() {
    let scene = camera_only();
    let options = options(4, 4);
    let renderer = Renderer::new(&scene, &options).unwrap();
    let results = render_all(&renderer);
    assert_eq!(results.len(), 6, "3 × 2 Tiles, one Pass");

    for result in &results {
        let bounds = renderer.decomposition().tile_bounds(result.unit().tile);
        assert_eq!(result.pixels().len(), bounds.area(), "{:?}", result.unit());
        for pixel in result.pixels() {
            assert_eq!(pixel.rgb, [0.0; 3], "black, in {:?}", result.unit());
            assert!(
                pixel.weight > 0.0,
                "filter weights summed, in {:?}",
                result.unit()
            );
        }
    }
}

#[test]
fn a_tile_cut_short_by_the_image_has_only_its_pixels() {
    let scene = camera_only();
    let options = options(1, 1);
    let renderer = Renderer::new(&scene, &options).unwrap();
    let bounds = renderer.decomposition().tile_bounds(TileIndex(5));
    assert_eq!(
        (bounds.width(), bounds.height()),
        (8, 8),
        "the bottom-right Tile"
    );
}

#[test]
fn work_results_do_not_depend_on_the_thread_count() {
    let scene = camera_only();
    let options = options(8, 4);
    let renderer = Renderer::new(&scene, &options).unwrap();
    let render_on = |threads| {
        let pool = ThreadPoolBuilder::new()
            .num_threads(threads)
            .build()
            .unwrap();
        pool.install(|| render_all(&renderer))
    };
    let one = render_on(1);
    assert_eq!(one, render_on(4), "1 thread and 4 threads");

    let single_pass = single_pass_film_hash(&scene, 1);
    assert_eq!(
        single_pass,
        single_pass_film_hash(&scene, 4),
        "single-Pass Film hash"
    );
}

/// The Film hash of a single-Pass render on `threads` threads.
fn single_pass_film_hash(scene: &Scene, threads: usize) -> FilmHash {
    let options = options(4, 4);
    let renderer = Renderer::new(scene, &options).expect("a Renderer");
    let pool = ThreadPoolBuilder::new()
        .num_threads(threads)
        .build()
        .expect("a thread pool");
    let results = pool.install(|| render_all(&renderer));
    let mut film = Film::new(scene, &options).expect("a Film");
    for result in &results {
        film.accept(result);
    }
    film.hash()
}

#[test]
fn a_render_is_a_pure_function_of_its_work_unit() {
    let scene = camera_only();
    let options = options(8, 4);
    let first = render_all(&Renderer::new(&scene, &options).unwrap());
    let renderer = Renderer::new(&scene, &options).unwrap();
    let mut units: Vec<WorkUnit> = renderer.decomposition().work_units().collect();
    units.reverse();
    let cancel = Cancellation::new();
    for unit in units {
        let Rendered::Done(result) = renderer.render(unit, &cancel, &Meter::new()).rendered else {
            panic!("{unit:?} cancelled");
        };
        let same = first.iter().find(|r| r.unit() == unit).unwrap();
        assert_eq!(&result, same, "{unit:?} rendered again, in another order");
    }
}

#[test]
fn a_cancelled_render_returns_no_work_result() {
    let scene = camera_only();
    let options = options(4, 4);
    let renderer = Renderer::new(&scene, &options).unwrap();
    let cancel = Cancellation::new();
    cancel.cancel();
    let unit = WorkUnit {
        tile: TileIndex(0),
        pass: PassIndex(0),
    };
    assert_eq!(
        renderer.render(unit, &cancel, &Meter::new()).rendered,
        Rendered::Cancelled
    );
}

#[test]
fn a_render_on_one_thread_takes_its_time_on_that_thread() {
    let scene = camera_only();
    let options = options(8, 4);
    let renderer = Renderer::new(&scene, &options).unwrap();
    let pool = ThreadPoolBuilder::new().num_threads(1).build().unwrap();
    let unit = WorkUnit {
        tile: TileIndex(0),
        pass: PassIndex(0),
    };
    let cancel = Cancellation::new();
    let start = Instant::now();
    let render = pool.install(|| renderer.render(unit, &cancel, &Meter::new()));
    let wall = start.elapsed();
    assert!(matches!(render.rendered, Rendered::Done(_)), "rendered");
    assert!(render.thread_time > Duration::ZERO, "{render:?}");
    assert!(
        render.thread_time <= wall,
        "{:?} > {wall:?}",
        render.thread_time
    );
}
