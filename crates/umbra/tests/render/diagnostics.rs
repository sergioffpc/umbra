//! Scene Files outside the subset: refused with every Diagnostic, located,
//! and exit 3, with no image (ADR 0003).

use crate::cli::{CAMERA_ONLY, Workspace, code, stderr};

#[test]
fn every_unsupported_use_is_listed_with_its_location_and_nothing_is_rendered() {
    let ws = Workspace::new();
    let scene = r#"LookAt 0 0 5  0 0 0  0 1 0
Camera "orthographic"
Film "rgb" "string filename" "out.exr"
    "float iso" 100
Translate 1 0 0
Sampler "halton" "string randomization" "fastowen"
WorldBegin
Shape "sphere" "float radius" 1
"#;
    ws.file("scene.pbrt", scene);
    let output = ws.umbra(&["render", "scene.pbrt"]);
    assert_eq!(code(&output), 3, "{}", stderr(&output));
    let stderr = stderr(&output);
    for location in [
        "scene.pbrt:2:1: error",
        "scene.pbrt:4:5: error",
        "scene.pbrt:5:1: error",
        "scene.pbrt:6:18: error",
        "scene.pbrt:8:1: error",
    ] {
        assert!(stderr.contains(location), "{location} in:\n{stderr}");
    }
    assert!(!ws.path("out.exr").exists(), "no image");
}

#[test]
fn an_error_in_an_include_is_located_in_the_include() {
    let ws = Workspace::new();
    ws.file(
        "camera.pbrt",
        "Camera \"perspective\"\n  \"integer fov\" 45\n",
    );
    ws.file("scene.pbrt", "Include \"camera.pbrt\"\nWorldBegin\n");
    let output = ws.umbra(&["render", "scene.pbrt"]);
    assert_eq!(code(&output), 3);
    assert!(
        stderr(&output).contains("camera.pbrt:2:3: error"),
        "{}",
        stderr(&output)
    );
}

#[test]
fn a_missing_include_is_located_at_the_include() {
    let ws = Workspace::new();
    ws.file("scene.pbrt", "WorldBegin\n\nInclude \"missing.pbrt\"\n");
    let output = ws.umbra(&["render", "scene.pbrt"]);
    assert_eq!(code(&output), 3);
    assert!(
        stderr(&output).contains("scene.pbrt:3:1: error"),
        "{}",
        stderr(&output)
    );
}

#[test]
fn an_output_that_is_not_an_exr_is_refused() {
    let ws = Workspace::new();
    ws.file("scene.pbrt", CAMERA_ONLY);
    let output = ws.umbra(&["render", "scene.pbrt", "--outfile", "out.png"]);
    assert_eq!(code(&output), 3, "{}", stderr(&output));
    assert!(!ws.path("out.png").exists(), "no image");

    ws.file("png.pbrt", &CAMERA_ONLY.replace("black.exr", "black.png"));
    let output = ws.umbra(&["render", "png.pbrt"]);
    assert_eq!(code(&output), 3);
    assert!(
        stderr(&output).contains("png.pbrt:5:5: error"),
        "{}",
        stderr(&output)
    );
}

#[test]
fn an_unused_parameter_warns_and_the_scene_still_renders() {
    let ws = Workspace::new();
    let scene = CAMERA_ONLY.replace(
        "PixelFilter \"gaussian\"",
        "PixelFilter \"gaussian\" \"float alpha\" 2",
    );
    ws.file("scene.pbrt", &scene);
    let output = ws.umbra(&["render", "scene.pbrt"]);
    assert_eq!(code(&output), 0, "{}", stderr(&output));
    assert!(
        stderr(&output).contains("scene.pbrt:7:24: warning"),
        "{}",
        stderr(&output)
    );
}

#[test]
fn a_missing_scene_file_is_a_failure_not_a_refusal() {
    let ws = Workspace::new();
    assert_eq!(code(&ws.umbra(&["render", "missing.pbrt"])), 1);
}

#[test]
fn invalid_usage_exits_2() {
    let ws = Workspace::new();
    assert_eq!(code(&ws.umbra(&["render", "scene.pbrt", "--spp", "0"])), 2);
}
