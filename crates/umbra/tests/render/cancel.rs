//! Ctrl-C cancels the Render Job: a report, no image, exit 130 (arc42
//! §8.10, ADR 0004).

use std::io::{BufRead, BufReader};
use std::process::{Command, Output, Stdio};
use std::thread;
use std::time::Duration;

use crate::cli::{CAMERA_ONLY, Workspace, report};

/// The camera-only Scene File, at enough Pixel Samples per pixel that it
/// is still rendering when it is cancelled.
fn long_scene() -> String {
    CAMERA_ONLY
        .replace("xresolution\" [ 40 ]", "xresolution\" [ 256 ]")
        .replace("yresolution\" [ 24 ]", "yresolution\" [ 256 ]")
        .replace("pixelsamples\" 4", "pixelsamples\" 1048576")
}

/// What `umbra` did when interrupted: the first status it logged, the
/// stderr lines after it, and how it ended.
struct Interrupted {
    first_status: String,
    later: Vec<String>,
    output: Output,
}

/// Runs `umbra` with `args` in `ws`, and sends it SIGINT once it has logged
/// its first status, which comes once the render phase has started, and
/// shortly after its Local Worker is ready, so that Work Units merge before
/// the cancellation.
fn interrupt(ws: &Workspace, args: &[&str]) -> Interrupted {
    let mut umbra = ws
        .command(args)
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .expect("umbra runs");
    let stderr = BufReader::new(umbra.stderr.take().expect("stderr is piped"));
    let mut lines = stderr.lines().map_while(Result::ok);
    let mut first_status = None;
    let mut later = Vec::new();
    let mut ready = false;
    for line in lines.by_ref() {
        ready |= line.contains("is ready");
        match first_status {
            None if line.contains("Work Units") => first_status = Some(line),
            None => {}
            Some(_) => later.push(line),
        }
        if ready && first_status.is_some() {
            break;
        }
    }
    let first_status = first_status.expect("a status event before the end");
    // Time for its first Work Units to merge.
    thread::sleep(Duration::from_millis(300));
    let pid = umbra.id().to_string();
    let kill = Command::new("kill")
        .args(["-INT", &pid])
        .status()
        .expect("kill runs");
    assert!(kill.success(), "SIGINT sent");
    later.extend(lines);
    let output = umbra.wait_with_output().expect("umbra exits");
    Interrupted {
        first_status,
        later,
        output,
    }
}

#[test]
fn ctrl_c_writes_a_cancelled_report_and_no_image_and_exits_130() {
    let ws = Workspace::new();
    ws.file("scene.pbrt", &long_scene());
    let Interrupted { output, .. } =
        interrupt(&ws, &["render", "scene.pbrt", "--spp-per-pass", "65536"]);

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

#[test]
fn the_eta_is_unknown_before_any_merge_and_provisional_during_pass_0() {
    let ws = Workspace::new();
    // One Pass of 4096 Pixel Samples per pixel, in Tiles of one pixel:
    // Work Units merge within milliseconds of the Worker being ready, and
    // Pass 0 lasts minutes.
    let scene = long_scene().replace("pixelsamples\" 1048576", "pixelsamples\" 4096");
    ws.file("scene.pbrt", &scene);
    let args = ["render", "scene.pbrt", "--tile-size", "1"];
    let run = interrupt(&ws, &[&args[..], &["--spp-per-pass", "4096"]].concat());

    assert!(run.first_status.contains("ETA --"), "{}", run.first_status);
    let last_status = run
        .later
        .iter()
        .rfind(|l| l.contains("Work Units"))
        .expect("the status at the cancellation");
    assert!(last_status.contains("Pass 1/1"), "{last_status}");
    assert!(last_status.contains("ETA ~"), "{last_status}");
}
