//! `umbra render` with the GPU's Local Worker in a child process over
//! QUIC (ADR 0026): the GPU tests, which need an NVIDIA GPU and run with
//! `cargo nextest run --profile gpu` alone.

use std::fs;
use std::io::{BufRead, BufReader};
use std::process::{Command, Output, Stdio};

use serde_json::Value;
use tempfile::TempDir;

/// A camera-only Scene File of `side` × `side` pixels at `spp` Pixel
/// Samples per pixel, whose image is `black.exr`.
fn scene(side: u32, spp: u32) -> String {
    format!(
        r#"LookAt 0 0 5  0 0 0  0 1 0
Camera "perspective" "float fov" [ 45 ]
Film "rgb" "integer xresolution" [ {side} ] "integer yresolution" [ {side} ]
    "string filename" "black.exr"
Sampler "halton" "integer pixelsamples" {spp}
PixelFilter "gaussian"
Integrator "volpath" "integer maxdepth" [ 5 ]
WorldBegin
"#
    )
}

/// `umbra` with `args`, in `dir`, its logs JSON lines on stderr.
fn umbra(dir: &TempDir, args: &[&str]) -> Command {
    let mut command = Command::new(env!("CARGO_BIN_EXE_umbra"));
    command.args(args).current_dir(dir.path());
    command
}

/// The message of each JSON log line of `stderr`.
fn messages(stderr: &str) -> Vec<String> {
    stderr.lines().filter_map(message).collect()
}

/// The message of a JSON log line, if `line` is one.
fn message(line: &str) -> Option<String> {
    let event: Value = serde_json::from_str(line).ok()?;
    event["fields"]["message"].as_str().map(str::to_owned)
}

/// The Render Job Report's `Render Job` line.
fn outcome(output: &Output) -> String {
    String::from_utf8_lossy(&output.stdout)
        .lines()
        .find_map(|line| line.strip_prefix("Render Job: "))
        .unwrap_or_default()
        .to_owned()
}

/// Renders `scene` with `args` after the Scene File, and checks that it
/// finished and wrote the image; returns the log messages.
fn render(scene: &str, args: &[&str]) -> Vec<String> {
    let dir = TempDir::new().expect("a temporary directory");
    fs::write(dir.path().join("scene.pbrt"), scene).expect("the Scene File written");
    let output = umbra(&dir, &[&["render", "scene.pbrt"], args].concat())
        .output()
        .expect("umbra runs");
    let stderr = String::from_utf8_lossy(&output.stderr);
    assert!(output.status.success(), "{stderr}");
    assert_eq!(outcome(&output), "finished", "{stderr}");
    assert!(dir.path().join("black.exr").exists(), "the image");
    messages(&stderr)
}

#[test]
fn the_gpu_alone_renders_the_image() {
    let messages = render(&scene(64, 64), &["--render-devices", "gpu"]);
    assert!(
        messages
            .iter()
            .any(|m| m.starts_with("started the Local Worker of gpu:0")),
        "{messages:?}"
    );
}

#[test]
fn by_default_the_cpu_and_every_gpu_render_the_image() {
    let messages = render(&scene(64, 64), &[]);
    assert!(
        messages
            .iter()
            .any(|m| m.starts_with("started the Local Worker of gpu:0")),
        "{messages:?}"
    );
    assert!(
        !messages.iter().any(|m| m.contains("no usable GPU")),
        "{messages:?}"
    );
}

#[test]
fn killing_the_gpu_worker_mid_render_still_finishes_the_image() {
    let dir = TempDir::new().unwrap();
    // Long enough that the Render Job still renders once the GPU child is
    // ready: then it is killed, and eight CPU threads render the rest.
    fs::write(dir.path().join("scene.pbrt"), scene(128, 8192)).unwrap();
    let mut umbra = umbra(&dir, &["render", "scene.pbrt", "--nthreads", "8"])
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .unwrap();
    let stderr = BufReader::new(umbra.stderr.take().unwrap());
    let mut lines = stderr.lines().map_while(Result::ok);
    let started = lines
        .by_ref()
        .filter_map(|line| message(&line))
        .find(|m| m.starts_with("started the Local Worker of gpu:0"))
        .expect("the GPU's Local Worker started");
    let pid = started.rsplit(' ').next().unwrap().to_owned();
    // Ready: it has asked for its first Work Units, and holds some from
    // then until the end.
    lines
        .by_ref()
        .filter_map(|line| message(&line))
        .find(|m| m.ends_with("on gpu:0 is ready"))
        .expect("the GPU's Local Worker became ready");

    let kill = Command::new("kill").args(["-KILL", &pid]).status().unwrap();
    assert!(
        kill.success(),
        "the GPU's Local Worker still ran: {started}"
    );

    let rest: Vec<String> = lines.filter_map(|line| message(&line)).collect();
    let output = umbra.wait_with_output().unwrap();
    assert!(output.status.success(), "{rest:?}");
    assert_eq!(outcome(&output), "finished", "{rest:?}");
    assert!(dir.path().join("black.exr").exists(), "the image");
    assert!(
        rest.iter().any(|m| m.contains("go back to the queue")),
        "its Work Units in flight went back to the queue: {rest:?}"
    );
}
