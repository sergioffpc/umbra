//! The version line `umbra` writes to stderr when it starts, and the
//! notice of `--version`.

use crate::cli::{CAMERA_ONLY, Workspace, code, stderr};

/// Whether `line` is `umbra <version> (<short commit>[-dirty])`.
fn is_version_line(line: &str) -> bool {
    let Some(rest) = line.strip_prefix(&format!("umbra {} (", env!("CARGO_PKG_VERSION"))) else {
        return false;
    };
    let Some(commit) = rest.strip_suffix(')') else {
        return false;
    };
    let commit = commit.strip_suffix("-dirty").unwrap_or(commit);
    commit == "unknown" || (commit.len() == 7 && commit.chars().all(|c| c.is_ascii_hexdigit()))
}

#[test]
fn version_writes_the_version_line_the_copyright_and_the_licences_to_stderr() {
    let ws = Workspace::new();
    for flag in ["--version", "-V"] {
        let output = ws.umbra(&[flag]);
        assert_eq!(code(&output), 0, "{flag}");
        assert!(output.stdout.is_empty(), "{flag}: nothing on stdout");
        let stderr = stderr(&output);
        let lines: Vec<&str> = stderr.lines().collect();
        assert_eq!(lines.len(), 3, "{flag}: {stderr}");
        assert!(is_version_line(lines[0]), "{flag}: {}", lines[0]);
        assert!(lines[1].starts_with("Copyright (c) "), "{}", lines[1]);
        assert!(lines[2].starts_with("MIT License."), "{}", lines[2]);
        assert!(lines[2].contains("pbrt-v4"), "{}", lines[2]);
    }
}

#[test]
fn render_starts_with_the_version_line_alone() {
    let ws = Workspace::new();
    ws.file("scene.pbrt", CAMERA_ONLY);
    let output = ws.umbra(&["render", "scene.pbrt"]);
    assert_eq!(code(&output), 0, "{}", stderr(&output));

    let stderr = stderr(&output);
    let mut lines = stderr.lines();
    assert!(
        is_version_line(lines.next().unwrap_or_default()),
        "{stderr}"
    );
    assert!(!stderr.contains("Copyright"), "no notice: {stderr}");
}
