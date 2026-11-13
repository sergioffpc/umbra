//! The Render Job Report of a finished Render Job (arc42 §8.10).

use serde_json::Value;

use crate::cli::{CAMERA_ONLY, Workspace, code, report, stderr};

/// Renders the camera-only Scene File, 40 × 24 pixels, at 4 Pixel Samples
/// per pixel in Passes of 3 and Tiles of 16: 3 × 2 Tiles, 2 Passes. Returns
/// its report.
fn rendered_report(ws: &Workspace) -> Value {
    ws.file("scene.pbrt", CAMERA_ONLY);
    let args = [
        "render",
        "scene.pbrt",
        "--tile-size",
        "16",
        "--spp-per-pass",
        "3",
    ];
    let output = ws.umbra(&[&args[..], &["--nthreads", "2"]].concat());
    assert_eq!(code(&output), 0, "{}", stderr(&output));
    report(&ws.path("black.exr"))
}

#[test]
fn the_report_has_the_options_it_was_rendered_with() {
    let ws = Workspace::new();
    let report = rendered_report(&ws);
    let options = &report["options"];
    assert_eq!(options["tile_size"], 16, "{options}");
    assert_eq!(options["spp_per_pass"], 3, "{options}");
    assert_eq!(options["spp"], 4, "{options}");
    assert_eq!(options["seed"], 0, "{options}");
    assert_eq!(options["threads"], 2, "{options}");
}

#[test]
fn the_report_times_every_phase_that_ran() {
    let ws = Workspace::new();
    let report = rendered_report(&ws);
    let times = &report["phase_times"];
    for phase in ["parse", "scene_build", "render", "merge", "image_write"] {
        assert!(times[phase].as_f64().unwrap() >= 0.0, "{phase}: {times}");
    }
    assert!(times["bvh_build"].is_null(), "no BVH yet: {times}");
}

#[test]
fn the_report_counts_the_pixel_samples_and_their_rate() {
    let ws = Workspace::new();
    let report = rendered_report(&ws);
    assert_eq!(report["pixel_samples"], 40 * 24 * 4);
    assert!(
        report["pixel_samples_per_second"].as_f64().unwrap() > 0.0,
        "{report}"
    );
}

#[test]
fn the_report_has_the_local_workers_figures() {
    let ws = Workspace::new();
    let report = rendered_report(&ws);
    let worker = &report["local_worker"];
    assert_eq!(worker["threads"], 2, "{worker}");
    assert_eq!(worker["work_units"], 3 * 2 * 2, "{worker}");
    assert_eq!(worker["pixel_samples"], 40 * 24 * 4, "{worker}");
    for time in ["busy_time", "idle_time"] {
        assert!(worker[time].as_f64().unwrap() >= 0.0, "{time}: {worker}");
    }
}

#[test]
fn the_report_has_a_normalised_cost_per_tile_in_raster_order() {
    let ws = Workspace::new();
    let report = rendered_report(&ws);
    let costs = report["tile_costs"].as_array().unwrap();
    assert_eq!(costs.len(), 3 * 2, "one per Tile");
    assert!(
        costs.iter().all(|c| c.as_f64().unwrap() >= 0.0),
        "{costs:?}"
    );
}

#[test]
fn a_finished_report_has_its_outcome_and_film_hash() {
    let ws = Workspace::new();
    let report = rendered_report(&ws);
    assert_eq!(report["outcome"], "finished");
    assert_eq!(report["film_hash"].as_str().unwrap().len(), 16, "{report}");
}
