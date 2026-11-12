//! Seam 2: the Film merges each Work Result as it arrives, in any order,
//! and discards a second one for the same Work Unit unread (ADR 0022).

use std::path::PathBuf;

use umbra_math::Point2i;
use umbra_render::{
    Acceptance, BuildIdentity, Film, PassIndex, PixelSum, Renderer, ReproductionRecord, TileIndex,
    Tree, WorkResult, WorkUnit,
};
use umbra_scene::ContentHash;

use crate::scenes::{camera_only, options, render_all};

/// A Work Result of `unit`, a Tile of `area` pixels, every pixel's sums
/// `value`.
fn uniform(unit: WorkUnit, area: usize, value: f64) -> WorkResult {
    let sum = PixelSum {
        rgb: [value, 2.0 * value, 3.0 * value],
        weight: value,
    };
    WorkResult::new(unit, vec![sum; area])
}

#[test]
fn single_pass_work_results_merge_to_the_same_film_in_any_order() {
    let scene = camera_only();
    let options = options(4, 4);
    let results = render_all(&Renderer::new(&scene, &options).unwrap());
    let film_of = |order: &[usize]| {
        let mut film = Film::new(&scene, &options).unwrap();
        for &i in order {
            assert_eq!(film.accept(&results[i]), Acceptance::Merged, "result {i}");
        }
        assert!(film.is_complete(), "every Work Unit merged");
        film
    };

    let n = results.len();
    let in_order: Vec<usize> = (0..n).collect();
    let reversed: Vec<usize> = (0..n).rev().collect();
    let interleaved: Vec<usize> = (0..n).step_by(2).chain((1..n).step_by(2)).collect();
    let rotated: Vec<usize> = (0..n).map(|i| (i + 3) % n).collect();
    let expected = film_of(&in_order);
    for order in [reversed, interleaved, rotated] {
        let film = film_of(&order);
        assert_eq!(film.hash(), expected.hash(), "order {order:?}");
        assert_eq!(film.pixels(), expected.pixels(), "order {order:?}");
    }
}

#[test]
fn a_duplicate_leaves_the_film_unchanged() {
    let scene = camera_only();
    let options = options(4, 4);
    let mut film = Film::new(&scene, &options).unwrap();
    let unit = WorkUnit {
        tile: TileIndex(4),
        pass: PassIndex(0),
    };
    let area = 16 * 8;
    assert_eq!(film.accept(&uniform(unit, area, 1.0)), Acceptance::Merged);
    let before = film.clone();

    // Other sums for the same Work Unit: merging them would change the Film.
    assert_eq!(
        film.accept(&uniform(unit, area, 5.0)),
        Acceptance::Duplicate
    );
    assert_eq!(film.hash(), before.hash(), "the duplicate was not merged");
    assert_eq!(film, before);
}

#[test]
fn a_work_result_lands_on_its_tile() {
    let scene = camera_only();
    let options = options(4, 4);
    let mut film = Film::new(&scene, &options).unwrap();
    // Tile 5 is the bottom-right one: x in 32..40, y in 16..24.
    let unit = WorkUnit {
        tile: TileIndex(5),
        pass: PassIndex(0),
    };
    film.accept(&uniform(unit, 8 * 8, 1.0));

    for (i, pixel) in film.pixels().iter().enumerate() {
        let (x, y) = (i % 40, i / 40);
        let expected = if x >= 32 && y >= 16 {
            PixelSum {
                rgb: [1.0, 2.0, 3.0],
                weight: 1.0,
            }
        } else {
            PixelSum::default()
        };
        assert_eq!(*pixel, expected, "pixel ({x}, {y})");
    }
}

#[test]
fn the_passes_of_a_tile_add_up() {
    let scene = camera_only();
    let options = options(8, 4);
    let mut film = Film::new(&scene, &options).unwrap();
    for (pass, value) in [(1, 0.25), (0, 0.5)] {
        let unit = WorkUnit {
            tile: TileIndex(0),
            pass: PassIndex(pass),
        };
        assert_eq!(
            film.accept(&uniform(unit, 16 * 16, value)),
            Acceptance::Merged
        );
    }
    let expected = PixelSum {
        rgb: [0.75, 1.5, 2.25],
        weight: 0.75,
    };
    assert_eq!(film.pixels()[0], expected, "pixel (0, 0)");
    assert_eq!(film.pixels()[15 * 40 + 15], expected, "pixel (15, 15)");
    assert_eq!(film.pixels()[16], PixelSum::default(), "pixel (16, 0)");
    assert!(!film.is_complete(), "the other Tiles are missing");
}

/// A Reproduction Record for tests: what it holds does not matter here.
fn record() -> ReproductionRecord {
    let identity = BuildIdentity {
        commit: "test".to_owned(),
        tree: Tree::Clean,
    };
    ReproductionRecord::new(
        PathBuf::from("scene.pbrt"),
        ContentHash([0; 32]),
        &options(4, 4),
        identity,
    )
}

#[test]
fn a_finished_film_is_each_pixels_weighted_mean_in_srgb() {
    let scene = camera_only();
    let options = options(4, 4);
    let mut film = Film::new(&scene, &options).unwrap();
    let units: Vec<WorkUnit> = Renderer::new(&scene, &options)
        .unwrap()
        .decomposition()
        .work_units()
        .collect();
    for unit in units {
        // Tile 0 is 16 × 16; its sums are XYZ (1, 2, 3) times a weight of 2.
        let area = [256, 256, 128, 128, 128, 64][unit.tile.0 as usize];
        let value = if unit.tile.0 == 0 { 2.0 } else { 0.0 };
        film.accept(&uniform(unit, area, value));
    }
    let image = film.finish(record());

    assert_eq!(image.resolution(), Point2i::new(40, 24));
    // The default sensor's RGB is XYZ; sRGB from XYZ, Rec. ITU-R BT.709
    // with D65 white, applied to (1, 2, 3).
    let expected = [
        3.240_454_2 - 1.537_138_5 * 2.0 - 0.498_531_4 * 3.0,
        -0.969_266 + 1.876_010_8 * 2.0 + 0.041_556 * 3.0,
        0.055_643_4 - 0.204_025_9 * 2.0 + 1.057_225_2 * 3.0,
    ];
    let pixel = image.pixels()[0];
    for (channel, (got, want)) in pixel.iter().zip(expected).enumerate() {
        assert!(
            (got - want).abs() < 2e-3,
            "channel {channel}: {got} vs {want}"
        );
    }
    assert_eq!(image.pixels()[39], [0.0; 3], "no weight: black");
}
