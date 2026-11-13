//! Running the `umbra` binary on Scene Files written to a temporary
//! directory.

use std::collections::BTreeMap;
use std::fs;
use std::path::PathBuf;
use std::process::{Command, Output};

use tempfile::TempDir;

/// A camera-only Scene File: every directive of the subset, nothing in the
/// world block.
pub(crate) const CAMERA_ONLY: &str = r#"# A camera and nothing else.
LookAt 0 0 5  0 0 0  0 1 0
Camera "perspective" "float fov" [ 45 ]
Film "rgb" "integer xresolution" [ 40 ] "integer yresolution" [ 24 ]
    "string filename" "black.exr"
Sampler "halton" "integer pixelsamples" 4
PixelFilter "gaussian"
Integrator "volpath" "integer maxdepth" [ 5 ]
WorldBegin
"#;

/// A temporary directory holding Scene Files, where the images are written.
pub(crate) struct Workspace {
    dir: TempDir,
}

impl Workspace {
    pub(crate) fn new() -> Self {
        Self {
            dir: TempDir::new().expect("a temporary directory"),
        }
    }

    /// Writes `contents` to `name` in the workspace, and returns its path.
    pub(crate) fn file(&self, name: &str, contents: &str) -> PathBuf {
        let path = self.path(name);
        fs::write(&path, contents).expect("a Scene File written");
        path
    }

    /// The path of `name` in the workspace.
    pub(crate) fn path(&self, name: &str) -> PathBuf {
        self.dir.path().join(name)
    }

    /// Runs `umbra` with `args`, in the workspace.
    pub(crate) fn umbra(&self, args: &[&str]) -> Output {
        Command::new(env!("CARGO_BIN_EXE_umbra"))
            .args(args)
            .current_dir(self.dir.path())
            .output()
            .expect("umbra runs")
    }
}

/// The exit code of `output`.
pub(crate) fn code(output: &Output) -> i32 {
    output.status.code().expect("umbra exited")
}

/// The standard error of `output`, as text.
pub(crate) fn stderr(output: &Output) -> String {
    String::from_utf8_lossy(&output.stderr).into_owned()
}

/// The Render Job Report `umbra` wrote to stdout: each `Name: value` line.
pub(crate) fn report(output: &Output) -> BTreeMap<String, String> {
    String::from_utf8_lossy(&output.stdout)
        .lines()
        .map(|line| {
            let (name, value) = line.split_once(": ").expect("a `Name: value` line");
            (name.to_owned(), value.to_owned())
        })
        .collect()
}

/// The value of `name` in a `name value, name value, …` list.
pub(crate) fn field<'a>(list: &'a str, name: &str) -> &'a str {
    list.split(", ")
        .find_map(|part| part.strip_prefix(name)?.strip_prefix(' '))
        .unwrap_or_else(|| panic!("{name} in {list}"))
}
