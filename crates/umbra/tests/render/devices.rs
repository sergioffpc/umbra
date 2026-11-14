//! `--render-devices`: the Render Devices of the machine `umbra render`
//! starts Local Workers on, on a machine without a usable GPU (ADR 0026).
//! The GPU tests (`tests/gpu/`) render on a GPU.

use crate::cli::{CAMERA_ONLY, Workspace, code, stderr};

/// The lines of `stderr` that say no GPU is usable.
fn no_gpu_lines(stderr: &str) -> usize {
    stderr
        .lines()
        .filter(|l| l.contains("no usable GPU"))
        .count()
}

#[test]
fn without_a_gpu_the_default_renders_on_the_cpu_with_one_warning() {
    let ws = Workspace::new();
    ws.file("scene.pbrt", CAMERA_ONLY);
    let output = ws.umbra(&["render", "scene.pbrt"]);

    assert_eq!(code(&output), 0, "{}", stderr(&output));
    assert!(ws.path("black.exr").exists(), "the image");
    let stderr = stderr(&output);
    assert_eq!(no_gpu_lines(&stderr), 1, "{stderr}");
    let warning = stderr
        .lines()
        .find(|l| l.contains("no usable GPU"))
        .unwrap();
    assert!(warning.contains("WARN"), "{warning}");
}

#[test]
fn the_cpu_alone_renders_without_a_warning() {
    let ws = Workspace::new();
    ws.file("scene.pbrt", CAMERA_ONLY);
    let output = ws.umbra(&["render", "scene.pbrt", "--render-devices", "cpu"]);

    assert_eq!(code(&output), 0, "{}", stderr(&output));
    assert!(ws.path("black.exr").exists(), "the image");
    assert_eq!(no_gpu_lines(&stderr(&output)), 0, "{}", stderr(&output));
}

#[test]
fn an_explicit_gpu_without_one_is_an_error() {
    for devices in ["gpu", "cpu,gpu"] {
        let ws = Workspace::new();
        ws.file("scene.pbrt", CAMERA_ONLY);
        let output = ws.umbra(&["render", "scene.pbrt", "--render-devices", devices]);

        assert_eq!(code(&output), 1, "{devices}: {}", stderr(&output));
        assert!(!ws.path("black.exr").exists(), "{devices}: no image");
        let stderr = stderr(&output);
        assert!(
            stderr
                .lines()
                .any(|l| l.starts_with("error: no usable GPU")),
            "{devices}: {stderr}"
        );
    }
}

#[test]
fn an_unknown_render_device_is_invalid_usage() {
    let ws = Workspace::new();
    ws.file("scene.pbrt", CAMERA_ONLY);
    let output = ws.umbra(&["render", "scene.pbrt", "--render-devices", "cpu,tpu"]);

    assert_eq!(code(&output), 2, "{}", stderr(&output));
}
