//! The final image of a camera-only Scene File.

use std::fs;
use std::path::Path;

use image::{ColorType, ImageReader};

use crate::cli::{CAMERA_ONLY, Workspace, code, report, stderr};

#[test]
fn a_camera_only_scene_file_renders_a_black_image() {
    let ws = Workspace::new();
    ws.file("scene.pbrt", CAMERA_ONLY);
    let output = ws.umbra(&["render", "scene.pbrt"]);
    assert_eq!(code(&output), 0, "{}", stderr(&output));

    let image = ImageReader::open(ws.path("black.exr"))
        .unwrap()
        .decode()
        .unwrap();
    assert_eq!((image.width(), image.height()), (40, 24), "the resolution");
    assert_eq!(image.color(), ColorType::Rgb32F, "RGB floats only");
    assert!(image.into_rgb32f().iter().all(|&v| v == 0.0), "black");
    assert_eq!(report(&ws.path("black.exr"))["outcome"], "finished");
}

#[test]
fn the_image_file_is_the_same_bytes_every_time() {
    let ws = Workspace::new();
    ws.file("scene.pbrt", CAMERA_ONLY);
    let render = |name: &str| {
        let output = ws.umbra(&["render", "scene.pbrt", "--outfile", name]);
        assert_eq!(code(&output), 0, "{}", stderr(&output));
        fs::read(ws.path(name)).unwrap()
    };
    assert_eq!(render("first.exr"), render("second.exr"));
}

#[test]
fn the_report_carries_the_reproduction_record() {
    let ws = Workspace::new();
    ws.file("scene.pbrt", CAMERA_ONLY);
    let args = ["render", "scene.pbrt", "--seed", "-7", "--spp", "3"];
    let output = ws.umbra(&[&args[..], &["--tile-size", "8"]].concat());
    assert_eq!(code(&output), 0, "{}", stderr(&output));

    let record = reproduction_record(&ws.path("black.exr"));
    assert_eq!(record["version"], 1);
    assert_eq!(
        record["scene_file"],
        ws.path("scene.pbrt")
            .canonicalize()
            .unwrap()
            .to_str()
            .unwrap()
    );
    assert_eq!(
        record["content_hash"].as_str().unwrap().len(),
        64,
        "SHA-256"
    );
    assert_eq!(record["seed"], -7, "--seed");
    assert_eq!(record["spp"], 3, "--spp");
    assert_eq!(record["tile_size"], 8);
    assert_eq!(record["spp_per_pass"], 64, "the default");
    assert!(record["build_identity"]["commit"].is_string(), "{record}");
    assert!(record.get("outfile").is_none(), "no output path");
}

#[test]
fn spp_per_pass_cuts_the_spp_into_passes() {
    let ws = Workspace::new();
    ws.file("scene.pbrt", CAMERA_ONLY);
    let args = ["render", "scene.pbrt", "--spp", "8", "--spp-per-pass", "3"];
    let output = ws.umbra(&args);
    assert_eq!(code(&output), 0, "{}", stderr(&output));

    let record = reproduction_record(&ws.path("black.exr"));
    assert_eq!(record["spp"], 8, "the count is kept");
    assert_eq!(record["spp_per_pass"], 3, "only the cut changes");
}

#[test]
fn the_seed_defaults_to_0_and_the_samplers_seed_is_overridden() {
    let ws = Workspace::new();
    ws.file("default.pbrt", CAMERA_ONLY);
    let seeded = CAMERA_ONLY.replace("pixelsamples\" 4", "pixelsamples\" 4 \"integer seed\" 5");
    ws.file("seeded.pbrt", &seeded);

    assert_eq!(code(&ws.umbra(&["render", "default.pbrt"])), 0);
    assert_eq!(reproduction_record(&ws.path("black.exr"))["seed"], 0);
    assert_eq!(code(&ws.umbra(&["render", "seeded.pbrt"])), 0);
    assert_eq!(reproduction_record(&ws.path("black.exr"))["seed"], 5);
    assert_eq!(
        code(&ws.umbra(&["render", "seeded.pbrt", "--seed", "2"])),
        0
    );
    assert_eq!(reproduction_record(&ws.path("black.exr"))["seed"], 2);
}

#[test]
fn the_film_hash_does_not_depend_on_the_thread_count() {
    let ws = Workspace::new();
    ws.file("scene.pbrt", CAMERA_ONLY);
    let film_hash = |threads: &[&str]| {
        let output = ws.umbra(&[&["render", "scene.pbrt", "--tile-size", "8"], threads].concat());
        assert_eq!(code(&output), 0, "{}", stderr(&output));
        let report = report(&ws.path("black.exr"));
        report["film_hash"].as_str().unwrap().to_owned()
    };
    assert_eq!(film_hash(&["--nthreads", "1"]), film_hash(&[]));
}

#[test]
fn the_film_filename_is_relative_to_the_scene_file_and_outfile_to_the_working_directory() {
    let ws = Workspace::new();
    fs::create_dir(ws.path("scenes")).unwrap();
    ws.file("scenes/scene.pbrt", CAMERA_ONLY);
    assert_eq!(code(&ws.umbra(&["render", "scenes/scene.pbrt"])), 0);
    assert!(ws.path("scenes/black.exr").is_file(), "the Film's filename");
    assert!(
        ws.path("scenes/black.exr.report.json").is_file(),
        "the report"
    );

    let outfile = ["render", "scenes/scene.pbrt", "--outfile", "out.exr"];
    assert_eq!(code(&ws.umbra(&outfile)), 0);
    assert!(ws.path("out.exr").is_file(), "--outfile");

    let unnamed = CAMERA_ONLY.replace("\"string filename\" \"black.exr\"", "");
    ws.file("scenes/unnamed.pbrt", &unnamed);
    assert_eq!(code(&ws.umbra(&["render", "scenes/unnamed.pbrt"])), 0);
    assert!(ws.path("pbrt.exr").is_file(), "pbrt-v4's default");
}

#[test]
fn an_include_is_read_relative_to_the_scene_file_and_hashed() {
    let ws = Workspace::new();
    fs::create_dir(ws.path("scenes")).unwrap();
    let (head, world) = CAMERA_ONLY.split_once("WorldBegin").unwrap();
    ws.file("scenes/camera.pbrt", head);
    ws.file(
        "scenes/scene.pbrt",
        &format!("Include \"camera.pbrt\"\nWorldBegin{world}"),
    );
    let content_hash = || {
        let output = ws.umbra(&["render", "scenes/scene.pbrt"]);
        assert_eq!(code(&output), 0, "{}", stderr(&output));
        let record = reproduction_record(&ws.path("scenes/black.exr"));
        record["content_hash"].as_str().unwrap().to_owned()
    };
    let before = content_hash();
    ws.file("scenes/camera.pbrt", &format!("{head}# a comment\n"));
    assert_ne!(content_hash(), before, "the include is in the content hash");
}

/// The Reproduction Record in the Render Job Report of the image at
/// `path`.
fn reproduction_record(path: &Path) -> serde_json::Value {
    report(path)["reproduction_record"].clone()
}
