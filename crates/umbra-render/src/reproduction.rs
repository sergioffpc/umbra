//! The Reproduction Record of a Render Job (arc42 §8.9): what is needed to
//! compute any of its Pixel Samples again.

use std::path::PathBuf;

use schemars::JsonSchema;
use serde::{Serialize, Serializer};
use umbra_scene::{ContentHash, RenderJobOptions};

/// Whether a binary was built from a tree with uncommitted changes.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, JsonSchema)]
#[serde(rename_all = "lowercase")]
pub enum Tree {
    /// Built from the commit, unchanged.
    Clean,
    /// Built with uncommitted changes on top of the commit.
    Dirty,
}

/// What a binary was built from (arc42 §7.2, ADR 0012): its commit, and
/// whether the tree was dirty.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, JsonSchema)]
pub struct BuildIdentity {
    /// The git commit, in full.
    pub commit: String,
    /// Whether the tree had uncommitted changes.
    pub tree: Tree,
}

/// The Reproduction Record of a Render Job: the Scene File and the content
/// hash of everything it read, the seed, every Render Job Option but the
/// output path, and the Build Identity. Its JSON is versioned, and the
/// Render Job Report carries it as `reproduction_record`.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, JsonSchema)]
pub struct ReproductionRecord {
    version: u32,
    scene_file: PathBuf,
    #[serde(serialize_with = "hex")]
    #[schemars(with = "String")]
    content_hash: ContentHash,
    seed: i32,
    tile_size: u32,
    spp_per_pass: u32,
    spp: u32,
    build_identity: BuildIdentity,
}

impl ReproductionRecord {
    /// The version of the record's JSON, raised whenever a field changes.
    pub const VERSION: u32 = 1;

    /// The record of a Render Job of `scene_file`, whose files hash to
    /// `content_hash`, rendered with `options` by `build_identity`.
    #[must_use]
    pub fn new(
        scene_file: PathBuf,
        content_hash: ContentHash,
        options: &RenderJobOptions,
        build_identity: BuildIdentity,
    ) -> Self {
        Self {
            version: Self::VERSION,
            scene_file,
            content_hash,
            seed: options.seed.0,
            tile_size: options.tile_size.get(),
            spp_per_pass: options.spp_per_pass.get(),
            spp: options.spp.get(),
            build_identity,
        }
    }
}

/// Serialises a content hash as its hexadecimal digits.
fn hex<S: Serializer>(hash: &ContentHash, serializer: S) -> Result<S::Ok, S::Error> {
    serializer.collect_str(hash)
}
