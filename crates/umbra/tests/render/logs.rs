//! `tracing` logs on stderr: JSON lines without a TTY, human-readable with
//! `--log-format human`, filtered by `--log` (arc42 §8.10).

use serde_json::Value;

use crate::cli::{CAMERA_ONLY, Workspace, code, stderr};

/// The JSON lines of `stderr`: every line but the version line.
fn events(stderr: &str) -> Vec<Value> {
    stderr
        .lines()
        .skip(1)
        .map(|line| serde_json::from_str(line).unwrap_or_else(|_| panic!("JSON: {line}")))
        .collect()
}

/// The message of `event`.
fn message(event: &Value) -> &str {
    event["fields"]["message"].as_str().unwrap_or_default()
}

#[test]
fn without_a_tty_the_logs_are_json_lines_with_the_final_status() {
    let ws = Workspace::new();
    ws.file("scene.pbrt", CAMERA_ONLY);
    let output = ws.umbra(&["render", "scene.pbrt", "--tile-size", "16"]);
    assert_eq!(code(&output), 0, "{}", stderr(&output));

    let events = events(&stderr(&output));
    let status = events
        .iter()
        .rfind(|e| message(e).contains("Work Units"))
        .unwrap_or_else(|| panic!("a status event in {events:?}"));
    assert_eq!(status["level"], "INFO");
    let line = message(status);
    for part in ["Pass 1/1", "6/6 Work Units", "100.0 %", "Pixel Samples/s"] {
        assert!(line.contains(part), "{part} in {line}");
    }
    assert!(
        events
            .iter()
            .any(|e| message(e).starts_with("Local Worker")),
        "the Local Worker's line in {events:?}"
    );
}

#[test]
fn log_format_human_writes_text() {
    let ws = Workspace::new();
    ws.file("scene.pbrt", CAMERA_ONLY);
    let output = ws.umbra(&["--log-format", "human", "render", "scene.pbrt"]);
    assert_eq!(code(&output), 0, "{}", stderr(&output));

    let stderr = stderr(&output);
    let status = stderr.lines().find(|l| l.contains("Work Units")).unwrap();
    assert!(status.contains("INFO"), "{status}");
    assert!(serde_json::from_str::<Value>(status).is_err(), "{status}");
}

#[test]
fn log_filters_the_events() {
    let ws = Workspace::new();
    ws.file("scene.pbrt", CAMERA_ONLY);
    // On the CPU alone: without a GPU, the default warns once.
    let output = ws.umbra(&[
        "--log",
        "warn",
        "render",
        "scene.pbrt",
        "--render-devices",
        "cpu",
    ]);
    assert_eq!(code(&output), 0, "{}", stderr(&output));
    assert!(events(&stderr(&output)).is_empty(), "{}", stderr(&output));
}
