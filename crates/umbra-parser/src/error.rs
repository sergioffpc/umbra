//! What is wrong with a Scene File, and where (ADR 0003), and why `parse`
//! fails.

use std::fmt::{self, Display};
use std::io;
use std::path::Path;
use std::sync::Arc;

use thiserror::Error;

/// A place in a Scene File or one of its includes: the file, and the line
/// and column, both from 1.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Location {
    /// The file, as resolved from the Scene File's path.
    pub file: Arc<Path>,
    /// The line, from 1.
    pub line: u32,
    /// The column, in characters, from 1.
    pub column: u32,
}

impl Display for Location {
    /// `file:line:column`.
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}:{}:{}", self.file.display(), self.line, self.column)
    }
}

/// How much a `ParserError` matters.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Severity {
    /// The Scene File is refused: nothing is built.
    Error,
    /// The Scene File renders; something in it has no effect.
    Warning,
}

impl Display for Severity {
    /// `error` or `warning`.
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(match self {
            Self::Error => "error",
            Self::Warning => "warning",
        })
    }
}

/// One reason a Scene File is refused, or one warning about it, with where
/// it is when it is in a file. It displays as `file:line:column: error:
/// message`, as compilers write it.
#[derive(Debug, Clone, PartialEq, Eq, Error)]
#[error("{}{severity}: {message}", located(.location.as_ref()))]
pub struct ParserError {
    /// Whether it refuses the Scene File.
    pub severity: Severity,
    /// Where it is; `None` for what is not in a file, such as `--outfile`.
    pub location: Option<Location>,
    /// What is wrong.
    pub message: String,
}

impl ParserError {
    /// An error at `location`.
    pub(crate) fn error(location: &Location, message: impl Into<String>) -> Self {
        Self {
            severity: Severity::Error,
            location: Some(location.clone()),
            message: message.into(),
        }
    }

    /// A warning at `location`.
    pub(crate) fn warning(location: &Location, message: impl Into<String>) -> Self {
        Self {
            severity: Severity::Warning,
            location: Some(location.clone()),
            message: message.into(),
        }
    }

    /// An error in no file.
    pub(crate) fn unlocated(message: impl Into<String>) -> Self {
        Self {
            severity: Severity::Error,
            location: None,
            message: message.into(),
        }
    }
}

/// `file:line:column: ` for a `location`, nothing without one.
fn located(location: Option<&Location>) -> impl Display {
    fmt::from_fn(move |f| match location {
        Some(location) => write!(f, "{location}: "),
        None => Ok(()),
    })
}

/// Why a Scene File was not parsed.
#[derive(Debug, Error)]
pub enum ParseError {
    /// The Scene File could not be read.
    #[error("cannot read the Scene File: {0}")]
    Io(#[source] io::Error),
    /// The Scene File is invalid: every `ParserError`, errors and warnings,
    /// in the order found. Nothing was built.
    #[error("invalid Scene File{}", on_lines(.0))]
    Invalid(Vec<ParserError>),
}

/// Each of `diagnostics` on a line of its own, each line started with a
/// newline.
fn on_lines(diagnostics: &[ParserError]) -> impl Display {
    fmt::from_fn(move |f| diagnostics.iter().try_for_each(|d| write!(f, "\n{d}")))
}

/// The Diagnostics of one parse, in the order they are found.
#[derive(Debug, Default)]
pub(crate) struct Diagnostics(Vec<ParserError>);

impl Diagnostics {
    /// Adds `diagnostic`.
    pub(crate) fn push(&mut self, diagnostic: ParserError) {
        self.0.push(diagnostic);
    }

    /// Whether any is an error.
    pub(crate) fn has_errors(&self) -> bool {
        self.0.iter().any(|d| d.severity == Severity::Error)
    }

    /// Every `ParserError`.
    pub(crate) fn into_vec(self) -> Vec<ParserError> {
        self.0
    }
}
