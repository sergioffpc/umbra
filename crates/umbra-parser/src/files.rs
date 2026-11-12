//! The files a Scene is read from: where an `Include` resolves, and the
//! content hash of everything read (arc42 §8.9).

use std::fs;
use std::io;
use std::path::{Path, PathBuf};

use sha2::{Digest, Sha256};
use umbra_scene::ContentHash;

/// The files of one parse: the directory relative paths resolve against,
/// the files being read, innermost last, and the hash of every file read.
#[derive(Debug)]
pub(crate) struct SceneFiles {
    search_directory: PathBuf,
    open: Vec<PathBuf>,
    hasher: Sha256,
}

/// Why a file was not read.
#[derive(Debug)]
pub(crate) enum Unread {
    /// It is already being read: it includes itself.
    Cycle,
    /// The file system refused it, or it is not UTF-8.
    Io(io::Error),
}

impl SceneFiles {
    /// The files of the Scene File at `scene_file`: relative paths resolve
    /// against its directory, as pbrt-v4's search directory.
    pub(crate) fn new(scene_file: &Path) -> Self {
        Self {
            search_directory: scene_file.parent().map_or_default(Path::to_path_buf),
            open: Vec::new(),
            hasher: Sha256::new(),
        }
    }

    /// Where `name` is: itself if absolute, otherwise in the search
    /// directory.
    ///
    /// pbrt-v4 `ResolveFilename`.
    pub(crate) fn resolve(&self, name: &str) -> PathBuf {
        self.search_directory.join(name)
    }

    /// The text of `path`, which is now being read until [`Self::close`],
    /// and is hashed.
    pub(crate) fn open(&mut self, path: &Path) -> Result<String, Unread> {
        let identity = fs::canonicalize(path).map_err(Unread::Io)?;
        if self.open.contains(&identity) {
            return Err(Unread::Cycle);
        }
        let bytes = fs::read(path).map_err(Unread::Io)?;
        self.hasher
            .update(u64::try_from(bytes.len()).unwrap_or(u64::MAX).to_le_bytes());
        self.hasher.update(&bytes);
        let text = String::from_utf8(bytes)
            .map_err(|e| Unread::Io(io::Error::new(io::ErrorKind::InvalidData, e)))?;
        self.open.push(identity);
        Ok(text)
    }

    /// Ends reading the innermost file.
    pub(crate) fn close(&mut self) {
        self.open.pop();
    }

    /// The hash of every file read, each as its length and its bytes, in
    /// the order read.
    pub(crate) fn content_hash(self) -> ContentHash {
        ContentHash(self.hasher.finalize().into())
    }
}
