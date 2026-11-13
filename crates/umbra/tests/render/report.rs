//! The Render Job Report of a finished Render Job, on stdout (arc42 §8.10).

use std::collections::BTreeMap;
use std::fs;

use crate::cli::{CAMERA_ONLY, Workspace, code, field, report, stderr};

/// Renders the camera-only Scene File, 40 × 24 pixels, at 4 Pixel Samples
/// per pixel in Passes of 3 and Tiles of 16: 3 × 2 Tiles, 2 Passes, on 2
/// threads. Returns its report.
fn rendered_report(ws: &Workspace) -> BTreeMap<String, String> {
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
    report(&output)
}

/// The seconds of `name` in `list`, as `name 0.123 s`.
fn seconds(list: &str, name: &str) -> f64 {
    let value = field(list, name);
    let seconds = value
        .strip_suffix(" s")
        .unwrap_or_else(|| panic!("{value}"));
    seconds.parse().expect("seconds")
}

#[test]
fn the_report_times_every_phase_that_ran() {
    let ws = Workspace::new();
    let report = rendered_report(&ws);
    let times = &report["Phase times"];
    for phase in ["parse", "Scene build", "render", "merge", "image write"] {
        assert!(seconds(times, phase) >= 0.0, "{phase}: {times}");
    }
    assert!(!times.contains("BVH build"), "no BVH yet: {times}");
}

#[test]
fn the_report_counts_the_pixel_samples_and_their_rate() {
    let ws = Workspace::new();
    let report = rendered_report(&ws);
    let samples = &report["Pixel Samples"];
    let merged = 40 * 24 * 4;
    assert!(
        samples.starts_with(&format!("{merged} merged, ")),
        "{samples}"
    );
    assert!(samples.ends_with(" per second"), "{samples}");
}

#[test]
fn the_report_has_the_local_workers_figures() {
    let ws = Workspace::new();
    let report = rendered_report(&ws);
    let worker = &report["Local Worker"];
    assert_eq!(field(worker, "2"), "threads", "{worker}");
    assert_eq!(field(worker, "12"), "Work Units", "{worker}");
    assert_eq!(field(worker, "3840"), "Pixel Samples", "{worker}");
    for time in ["busy", "idle"] {
        assert!(seconds(worker, time) >= 0.0, "{time}: {worker}");
    }
}

#[test]
fn the_report_names_the_local_workers_host_and_cpu() {
    let ws = Workspace::new();
    let report = rendered_report(&ws);
    let worker = &report["Local Worker"];
    let host = fs::read_to_string("/proc/sys/kernel/hostname").expect("Linux names its host");
    assert_eq!(field(worker, "host"), host.trim(), "{worker}");
    assert!(!field(worker, "CPU").is_empty(), "{worker}");
}

#[test]
fn the_report_summarises_the_tile_costs() {
    let ws = Workspace::new();
    let report = rendered_report(&ws);
    let costs = &report["Tile costs"];
    for part in ["min ", "mean ", "max "] {
        assert!(costs.contains(part), "{part} in {costs}");
    }
    let costliest: u32 = costs
        .rsplit_once("(Tile ")
        .and_then(|(_, tile)| tile.strip_suffix(')'))
        .unwrap()
        .parse()
        .unwrap();
    assert!(costliest < 3 * 2, "one of the 6 Tiles: {costs}");
}

#[test]
fn the_report_lists_the_render_job_options_with_the_output_path() {
    let ws = Workspace::new();
    let report = rendered_report(&ws);
    assert_eq!(
        report["Options"],
        "seed 0, spp 4, spp per pass 3, tile size 16, output black.exr"
    );
}

#[test]
fn a_finished_report_has_its_outcome_and_film_hash() {
    let ws = Workspace::new();
    let report = rendered_report(&ws);
    assert_eq!(report["Render Job"], "finished");
    assert_eq!(report["Film hash"].len(), 16, "{report:?}");
}
