//! The Build Identity of this binary, from `build.rs` (ADR 0012).

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
        target_cpu: env!("UMBRA_TARGET_CPU").to_owned(),
    }
}
