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
    let mut umbra = ws
        .command(&["render", "scene.pbrt", "--spp-per-pass", "65536"])
        .stdout(Stdio::piped())
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
    let output = umbra.wait_with_output().unwrap();

    assert_eq!(output.status.code(), Some(130), "{:?}", output.status);
    assert!(!ws.path("black.exr").exists(), "no image");
    let report = report(&output);
    assert_eq!(report["Render Job"], "cancelled");
    assert!(!report.contains_key("Film hash"), "{report:?}");
    let times = &report["Phase times"];
    for phase in ["parse", "Scene build", "render"] {
        assert!(times.contains(&format!("{phase} ")), "{phase}: {times}");
    }
    assert!(!times.contains("image write"), "{times}");
    let samples = &report["Pixel Samples"];
    let (merged, _) = samples.split_once(" merged").expect("a count merged");
    let merged: u64 = merged.parse().unwrap();
    assert!(merged < 256 * 256 * 1_048_576, "{samples}");
}
