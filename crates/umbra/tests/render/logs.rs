//! `tracing` logs on stderr: JSON lines without a TTY, text with
//! `--log-format pretty` or `compact`, filtered by `--log-filter` (arc42
//! §8.10).

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

/// The stderr of rendering a camera alone with the log format `format`.
fn logged_in(format: &str) -> String {
    let ws = Workspace::new();
    ws.file("scene.pbrt", CAMERA_ONLY);
    let output = ws.umbra(&["--log-format", format, "render", "scene.pbrt"]);
    assert_eq!(code(&output), 0, "{}", stderr(&output));
    stderr(&output)
}

/// The text event of `stderr` that carries the final status, without the
/// module that logged it.
fn status_line(stderr: &str) -> &str {
    let status = stderr
        .lines()
        .rfind(|l| l.contains("Work Units"))
        .unwrap_or_else(|| panic!("a status event in {stderr}"));
    assert!(status.contains("INFO"), "{status}");
    assert!(serde_json::from_str::<Value>(status).is_err(), "{status}");
    assert!(!status.contains("umbra::status"), "{status}");
    status
}

#[test]
fn log_format_pretty_writes_each_span_on_a_line_of_its_own() {
    let stderr = logged_in("pretty");
    let status = status_line(&stderr);

    let spans: Vec<&str> = stderr
        .lines()
        .skip_while(|&l| l != status)
        .skip(1)
        .take_while(|l| !l.is_empty())
        .map(str::trim)
        .collect();
    assert_eq!(spans, ["in render_job"], "{stderr}");
    assert!(!stderr.contains(".rs:"), "no source location in {stderr}");
}

#[test]
fn log_format_compact_writes_each_event_on_one_line() {
    let stderr = logged_in("compact");
    let status = status_line(&stderr);

    assert!(status.contains("render_job: Pass 1/1"), "{status}");
    assert!(
        stderr.lines().all(|l| !l.trim_start().starts_with("in ")),
        "no span on a line of its own in {stderr}"
    );
}

#[test]
fn log_filters_the_events() {
    let ws = Workspace::new();
    ws.file("scene.pbrt", CAMERA_ONLY);
    // On the CPU alone: without a GPU, the default warns once.
    let output = ws.umbra(&[
        "--log-filter",
        "warn",
        "render",
        "scene.pbrt",
        "--render-devices",
        "cpu",
    ]);
    assert_eq!(code(&output), 0, "{}", stderr(&output));
    assert!(events(&stderr(&output)).is_empty(), "{}", stderr(&output));
}

/// Whether `rate` reads as a count with an SI prefix: `950`, `12.50 k`,
/// `1.25 M`, `3.10 G`.
fn is_si(rate: &str) -> bool {
    let digits = |s: &str| !s.is_empty() && s.chars().all(|c| c.is_ascii_digit());
    match rate.split_once(' ') {
        None => digits(rate),
        Some((number, prefix)) => {
            ["k", "M", "G"].contains(&prefix)
                && number.split_once('.').is_some_and(|(whole, cents)| {
                    digits(whole) && digits(cents) && cents.len() == 2
                })
        }
    }
}

#[test]
fn the_status_reads_its_rate_with_an_si_prefix_and_its_times_as_a_clock() {
    let ws = Workspace::new();
    ws.file("scene.pbrt", CAMERA_ONLY);
    let output = ws.umbra(&["render", "scene.pbrt", "--tile-size", "16"]);
    assert_eq!(code(&output), 0, "{}", stderr(&output));

    let events = events(&stderr(&output));
    let status = events
        .iter()
        .map(message)
        .rfind(|m| m.contains("Work Units"))
        .expect("the final status");
    let parts: Vec<&str> = status.split(" · ").collect();
    let [.., rate, elapsed, eta] = parts[..] else {
        panic!("{status}");
    };
    let rate = rate.strip_suffix(" Pixel Samples/s").expect(status);
    assert!(is_si(rate), "{rate} in {status}");
    // A render of a few milliseconds, finished: nothing is left.
    assert_eq!(elapsed, "0:00:00 elapsed", "{status}");
    assert_eq!(eta, "ETA 0:00:00", "{status}");
}
