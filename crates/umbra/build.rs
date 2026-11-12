//! Embeds the Build Identity (arc42 §7.2, ADR 0012): the git commit, whether
//! the tree had uncommitted changes, and the CPU the binary is compiled
//! for, as the `UMBRA_COMMIT`, `UMBRA_TREE` and `UMBRA_TARGET_CPU`
//! environment variables of the compilation.
//!
//! A container build has no `.git`: it passes the commit as `UMBRA_COMMIT`,
//! and `UMBRA_TREE` (`clean` or `dirty`, `clean` by default when the commit
//! is given). Without either, the commit is `unknown`. The tree is checked
//! when the index or `HEAD` changes, not on every edit: a binary rebuilt
//! after an unstaged edit alone may still read `clean`.

use std::env;
use std::process::Command;

fn main() {
    for var in ["UMBRA_COMMIT", "UMBRA_TREE"] {
        println!("cargo::rerun-if-env-changed={var}");
    }
    for path in ["HEAD", "index"] {
        if let Some(path) = git(&["rev-parse", "--git-path", path]) {
            println!("cargo::rerun-if-changed={path}");
        }
    }
    let (commit, tree) = match env::var("UMBRA_COMMIT") {
        Ok(commit) => (
            commit,
            env::var("UMBRA_TREE").unwrap_or_else(|_| "clean".into()),
        ),
        Err(_) => (
            git(&["rev-parse", "HEAD"]).unwrap_or_else(|| "unknown".into()),
            git_tree(),
        ),
    };
    println!("cargo::rustc-env=UMBRA_COMMIT={commit}");
    println!("cargo::rustc-env=UMBRA_TREE={tree}");
    println!("cargo::rustc-env=UMBRA_TARGET_CPU={}", target_cpu());
}

/// The trimmed output of `git args`, if it succeeds.
fn git(args: &[&str]) -> Option<String> {
    let output = Command::new("git").args(args).output().ok()?;
    output
        .status
        .success()
        .then(|| String::from_utf8_lossy(&output.stdout).trim().to_owned())
}

/// `dirty` if a tracked file differs from `HEAD`, `clean` otherwise.
fn git_tree() -> String {
    match git(&["status", "--porcelain", "--untracked-files=no"]) {
        Some(changes) if changes.is_empty() => "clean".into(),
        Some(_) => "dirty".into(),
        None => "unknown".into(),
    }
}

/// The last `-C target-cpu` of the compilation's flags, or `default`.
fn target_cpu() -> String {
    let flags = env::var("CARGO_ENCODED_RUSTFLAGS").unwrap_or_default();
    let flags: Vec<&str> = flags.split('\u{1f}').collect();
    let joined = flags.windows(2).filter_map(|pair| match pair {
        ["-C", flag] => Some(*flag),
        _ => None,
    });
    let attached = flags.iter().filter_map(|f| f.strip_prefix("-C"));
    joined
        .chain(attached)
        .filter_map(|flag| flag.strip_prefix("target-cpu="))
        .next_back()
        .unwrap_or("default")
        .to_owned()
}
