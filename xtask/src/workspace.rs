//! Reading the core crates' sources from the repository tree.

use std::fmt;
use std::fs;
use std::io;
use std::path::{Component, Path, PathBuf};

use crate::Source;

/// The seven `#![no_std]` core crates of arc42 §5.1: the rendering core.
pub const CORE_CRATES: [&str; 7] = [
    "umbra-math",
    "umbra-sampling",
    "umbra-spectrum",
    "umbra-geometry",
    "umbra-shading",
    "umbra-camera",
    "umbra-integrators",
];

/// Why the core crates could not be checked.
#[derive(Debug)]
pub enum Error {
    /// A file or directory could not be read.
    Io(PathBuf, io::Error),
    /// A source file is not valid Rust.
    Parse(PathBuf, syn::Error),
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Io(path, error) => write!(f, "{}: {error}", path.display()),
            Self::Parse(path, error) => {
                let start = error.span().start();
                write!(
                    f,
                    "{}:{}:{}: {error}",
                    path.display(),
                    start.line,
                    start.column + 1
                )
            }
        }
    }
}

impl std::error::Error for Error {}

/// Every `.rs` file under `src/` of each core crate below `root`, in path
/// order, with paths relative to `root`.
pub(crate) fn core_sources(root: &Path) -> Result<Vec<Source>, Error> {
    let mut sources = Vec::new();
    for krate in CORE_CRATES {
        let src = Path::new("crates").join(krate).join("src");
        let mut files = Vec::new();
        rust_files(root, &src, &mut files)?;
        files.sort();
        for file in files {
            let text =
                fs::read_to_string(root.join(&file)).map_err(|e| Error::Io(file.clone(), e))?;
            let module = module_path(&krate.replace('-', "_"), &file, &src);
            sources.push(Source::new(file, module, text));
        }
    }
    Ok(sources)
}

/// Collects the `.rs` files below `root/dir`, as paths relative to `root`.
fn rust_files(root: &Path, dir: &Path, files: &mut Vec<PathBuf>) -> Result<(), Error> {
    let entries = fs::read_dir(root.join(dir)).map_err(|e| Error::Io(dir.to_owned(), e))?;
    for entry in entries {
        let entry = entry.map_err(|e| Error::Io(dir.to_owned(), e))?;
        let path = dir.join(entry.file_name());
        if entry.path().is_dir() {
            rust_files(root, &path, files)?;
        } else if path.extension().is_some_and(|ext| ext == "rs") {
            files.push(path);
        }
    }
    Ok(())
}

/// The module a file defines: `src/lib.rs` is the crate, `src/a/b.rs` (or
/// `src/a/b/mod.rs`) is `crate::a::b`.
fn module_path(krate: &str, file: &Path, src: &Path) -> String {
    let relative = file.strip_prefix(src).unwrap_or(file).with_extension("");
    let segments = relative
        .components()
        .filter_map(|component| match component {
            Component::Normal(name) => name.to_str(),
            Component::Prefix(_)
            | Component::RootDir
            | Component::CurDir
            | Component::ParentDir => None,
        });
    std::iter::once(krate)
        .chain(segments.filter(|segment| !matches!(*segment, "lib" | "mod")))
        .collect::<Vec<_>>()
        .join("::")
}
