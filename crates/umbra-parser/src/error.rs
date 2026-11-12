//! Why `parse` fails.

use std::fmt::{self, Display};
use std::io;

use thiserror::Error;

use crate::diagnostic::Diagnostic;

/// Why a Scene File was not parsed.
#[derive(Debug, Error)]
pub enum Error {
    /// The Scene File could not be read.
    #[error("cannot read the Scene File: {0}")]
    Io(#[source] io::Error),
    /// The Scene File is invalid: every `Diagnostic`, errors and warnings,
    /// in the order found. Nothing was built.
    #[error("invalid Scene File{}", on_lines(.0))]
    Invalid(Vec<Diagnostic>),
}

/// Each of `diagnostics` on a line of its own, each line started with a
/// newline.
fn on_lines(diagnostics: &[Diagnostic]) -> impl Display {
    fmt::from_fn(move |f| diagnostics.iter().try_for_each(|d| write!(f, "\n{d}")))
}
