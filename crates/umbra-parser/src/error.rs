//! What is wrong with a Scene File, and where (ADR 0003), and why `parse`
//! fails.

use std::error::Error;
use std::fmt;
use std::io;
use std::path::Path;
use std::sync::Arc;

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

impl fmt::Display for Location {
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

/// One reason a Scene File is refused, or one warning about it, with where
/// it is when it is in a file.
#[derive(Debug, Clone, PartialEq, Eq)]
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

impl fmt::Display for ParserError {
    /// `file:line:column: error: message`, as compilers write it.
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        if let Some(location) = &self.location {
            write!(f, "{location}: ")?;
        }
        let severity = match self.severity {
            Severity::Error => "error",
            Severity::Warning => "warning",
        };
        write!(f, "{severity}: {}", self.message)
    }
}

/// Why a Scene File was not parsed.
#[derive(Debug)]
pub enum ParseError {
    /// The Scene File could not be read.
    Io(io::Error),
    /// The Scene File is invalid: every `ParserError`, errors and warnings,
    /// in the order found. Nothing was built.
    Invalid(Vec<ParserError>),
}

impl fmt::Display for ParseError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Io(error) => write!(f, "cannot read the Scene File: {error}"),
            Self::Invalid(diagnostics) => {
                write!(f, "invalid Scene File")?;
                diagnostics.iter().try_for_each(|d| write!(f, "\n{d}"))
            }
        }
    }
}

impl Error for ParseError {
    fn source(&self) -> Option<&(dyn Error + 'static)> {
        match self {
            Self::Io(error) => Some(error),
            Self::Invalid(_) => None,
        }
    }
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
