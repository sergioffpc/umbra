//! `cargo xtask`: repository tooling.
//!
//! `cargo xtask book-map` checks the book citations of the rendering core
//! (QS-4) and writes `docs/book-map.md`; `--check` instead fails when the
//! committed map is out of date, as CI does.

use std::fs;
use std::path::{Path, PathBuf};
use std::process::ExitCode;

/// The generated index, relative to the repository root.
const BOOK_MAP: &str = "docs/book-map.md";

/// What `book-map` does with the generated map.
#[derive(Debug, Clone, Copy)]
enum Mode {
    /// Write it to `docs/book-map.md`.
    Write,
    /// Compare it with the committed `docs/book-map.md`.
    Check,
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let mode = match args
        .iter()
        .map(String::as_str)
        .collect::<Vec<_>>()
        .as_slice()
    {
        ["book-map"] => Mode::Write,
        ["book-map", "--check"] => Mode::Check,
        _ => return fail("usage: cargo xtask book-map [--check]"),
    };
    match book_map(&repository_root(), mode) {
        Ok(code) => code,
        Err(message) => fail(&message),
    }
}

/// Runs the citation check, then writes or compares the book map.
fn book_map(root: &Path, mode: Mode) -> Result<ExitCode, String> {
    let audit = xtask::audit_workspace(root).map_err(|e| e.to_string())?;
    for violation in audit.violations() {
        report(&violation.to_string());
    }
    let map = audit.book_map();
    let path = root.join(BOOK_MAP);
    let fresh = match mode {
        Mode::Write => {
            fs::write(&path, &map).map_err(|e| format!("{BOOK_MAP}: {e}"))?;
            true
        }
        Mode::Check => fs::read_to_string(&path).is_ok_and(|committed| committed == map),
    };
    if !fresh {
        report(&format!(
            "{BOOK_MAP} is out of date: run `cargo xtask book-map` and commit it"
        ));
    }
    let passed = fresh && audit.violations().is_empty();
    Ok(if passed {
        ExitCode::SUCCESS
    } else {
        ExitCode::FAILURE
    })
}

/// The repository root: the parent of this crate's directory.
fn repository_root() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .map_or_else(|| PathBuf::from("."), Path::to_owned)
}

#[expect(clippy::print_stderr, reason = "a command-line tool reports on stderr")]
fn report(message: &str) {
    eprintln!("{message}");
}

fn fail(message: &str) -> ExitCode {
    report(message);
    ExitCode::FAILURE
}
