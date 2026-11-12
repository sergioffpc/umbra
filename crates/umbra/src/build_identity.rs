//! The Build Identity of this binary, from `build.rs` (ADR 0012), the
//! version line `umbra` writes when it starts, and the notice of
//! `--version`.

use umbra_render::{BuildIdentity, Tree};

/// What this binary was built from. A tree whose state is unknown counts as
/// dirty.
pub(crate) fn build_identity() -> BuildIdentity {
    BuildIdentity {
        commit: env!("UMBRA_COMMIT").to_owned(),
        tree: match env!("UMBRA_TREE") {
            "clean" => Tree::Clean,
            _ => Tree::Dirty,
        },
    }
}

/// `umbra 0.1.0 (9bd75ce)`: the name, the version and the short commit,
/// with `-dirty` after the commit when the tree had uncommitted changes.
pub(crate) fn version_line() -> String {
    let identity = build_identity();
    let commit = identity.commit.get(..7).unwrap_or(&identity.commit);
    let tree = match identity.tree {
        Tree::Clean => "",
        Tree::Dirty => "-dirty",
    };
    format!("umbra {} ({commit}{tree})", env!("CARGO_PKG_VERSION"))
}

/// The year of the copyright, as in `LICENSE`.
const COPYRIGHT_YEAR: u16 = 2026;

/// What `--version` writes: the version line, the copyright and the
/// licences, Umbra's and that of the pbrt-v4 code it includes (`NOTICE`).
pub(crate) fn version_notice() -> String {
    format!(
        "{}\nCopyright (c) {COPYRIGHT_YEAR} {}\n{} License. Includes code from pbrt-v4, under the Apache License 2.0; see NOTICE.",
        version_line(),
        env!("CARGO_PKG_AUTHORS").replace(':', ", "),
        env!("CARGO_PKG_LICENSE"),
    )
}
