//! Why `umbra render` did not write an image.

use std::fmt::{self, Display};
use std::io;

use thiserror::Error;
use umbra_parser::Diagnostic;

/// Why `umbra render` did not write an image.
#[derive(Debug, Error)]
pub(crate) enum Error {
    /// The Scene File is invalid: every `Diagnostic`.
    #[error("{}", lines(.0))]
    Refused(Vec<Diagnostic>),
    /// The Scene File could not be read.
    #[error("error: cannot read the Scene File: {0}")]
    Unreadable(#[source] io::Error),
    /// A file could not be resolved or written.
    #[error("error: {0}")]
    Io(#[from] io::Error),
    /// The Render Job did not finish.
    #[error("error: {0}")]
    Cluster(#[from] umbra_cluster::Error),
}

/// `diagnostics`, one per line.
fn lines(diagnostics: &[Diagnostic]) -> impl Display {
    fmt::from_fn(move |f| {
        diagnostics
            .iter()
            .enumerate()
            .try_for_each(|(i, d)| match i {
                0 => write!(f, "{d}"),
                _ => write!(f, "\n{d}"),
            })
    })
}
