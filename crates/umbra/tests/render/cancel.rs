//! Ctrl-C cancels the Render Job: a report, no image, exit 130 (arc42
//! §8.10, ADR 0004).

use std::io::{BufRead, BufReader};
use std::process::{Command, Stdio};

use crate::cli::{CAMERA_ONLY, Workspace, report};

/// The camera-only Scene File, at enough Pixel Samples per pixel that it
/// is still rendering when it is cancelled.
fn long_scene() -> String {
    CAMERA_ONLY
        .replace("xresolution\" [ 40 ]", "xresolution\" [ 256 ]")
        .replace("yresolution\" [ 24 ]", "yresolution\" [ 256 ]")
        .replace("pixelsamples\" 4", "pixelsamples\" 1048576")
}

#[test]
fn ctrl_c_writes_a_cancelled_report_and_no_image_and_exits_130() {
    let ws = Workspace::new();
    ws.file("scene.pbrt", &long_scene());
    let mut umbra = Command::new(env!("CARGO_BIN_EXE_umbra"))
        .args(["render", "scene.pbrt", "--spp-per-pass", "65536"])
        .current_dir(ws.path(""))
        .stderr(Stdio::piped())
        .spawn()
        .expect("umbra runs");
    // The first status event comes once the render phase has started.
    let stderr = BufReader::new(umbra.stderr.take().unwrap());
    let mut lines = stderr.lines();
    let rendering = lines
        .by_ref()
        .map_while(Result::ok)
        .any(|l| l.contains("Work Units"));
    assert!(rendering, "a status event before the end");
    let pid = umbra.id().to_string();
    let kill = Command::new("kill").args(["-INT", &pid]).status().unwrap();
    assert!(kill.success(), "SIGINT sent");
    lines.for_each(drop);
    let status = umbra.wait().unwrap();

    assert_eq!(status.code(), Some(130), "{status:?}");
    assert!(!ws.path("black.exr").exists(), "no image");
    let report = report(&ws.path("black.exr"));
    assert_eq!(report["outcome"], "cancelled");
    assert!(report["film_hash"].is_null(), "{report}");
    let times = &report["phase_times"];
    for phase in ["parse", "scene_build", "render"] {
        assert!(times[phase].as_f64().is_some(), "{phase}: {times}");
    }
    assert!(times["image_write"].is_null(), "{times}");
    let merged = report["pixel_samples"].as_u64().unwrap();
    assert!(merged < 256 * 256 * 1_048_576, "{merged} merged");
}
