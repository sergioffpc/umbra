//! `SceneBuilder::build`: what it refuses.

use umbra_math::{Point2i, Vector2f};
use umbra_scene::{Error, FilmParameters, GaussianFilterParameters, SceneBuilder};

#[test]
fn the_defaults_build() {
    let scene = SceneBuilder::new().build().unwrap();
    assert_eq!(
        scene.full_resolution(),
        Point2i::new(1280, 720),
        "pbrt-v4's default film"
    );
}

#[test]
fn a_film_without_pixels_is_refused() {
    for resolution in [Point2i::new(0, 10), Point2i::new(10, -1)] {
        let mut builder = SceneBuilder::new();
        builder.film(FilmParameters { resolution });
        assert_eq!(
            builder.build(),
            Err(Error::EmptyResolution { resolution }),
            "resolution {resolution:?}"
        );
    }
}

#[test]
fn a_filter_too_small_to_tabulate_is_refused() {
    let mut builder = SceneBuilder::new();
    builder.filter(GaussianFilterParameters {
        radius: Vector2f::new(1.0, 0.01),
        sigma: 0.5,
    });
    assert_eq!(builder.build(), Err(Error::FilterRadiusTooSmall));
}
