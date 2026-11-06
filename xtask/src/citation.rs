//! Reading a doc comment's citation of the book and of pbrt-v4.

use std::collections::BTreeSet;
use std::fmt;

/// The marker before a book section: `PBR 4e §9.3.2`.
const SECTION_MARKER: &str = "PBR 4e §";
/// The marker before a pbrt-v4 symbol: ``pbrt-v4 `DiffuseBxDF::Sample_f` ``.
const SYMBOL_MARKER: &str = "pbrt-v4 `";
/// The marker of an item that implements no pbrt-v4 algorithm.
const OPT_OUT_MARKER: &str = "Not in pbrt-v4:";

/// A section of *Physically Based Rendering*, 4th edition: `9.3.2` or `B.2`.
///
/// Orders as the book does: chapters by number, then the appendices.
#[derive(Debug, Clone, PartialEq, Eq, PartialOrd, Ord)]
pub(crate) struct Section {
    /// Chapters number from 1; appendix `A` is chapter 1001, and so on.
    key: Vec<u32>,
    text: String,
}

/// Offset of the appendices after every chapter in [`Section`]'s order.
const APPENDIX_BASE: u32 = 1000;

impl Section {
    /// Parses `9.3.2` or `B.2`; `None` if it is not a section number.
    fn parse(text: &str) -> Option<Self> {
        let mut parts = text.split('.');
        let first = parts.next()?;
        let chapter = match first.as_bytes() {
            [letter @ b'A'..=b'Z'] => APPENDIX_BASE + u32::from(letter - b'A') + 1,
            _ => first.parse().ok()?,
        };
        let key = std::iter::once(Some(chapter))
            .chain(parts.map(|part| part.parse().ok()))
            .collect::<Option<Vec<u32>>>()?;
        Some(Self {
            key,
            text: text.to_owned(),
        })
    }
}

impl fmt::Display for Section {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "§{}", self.text)
    }
}

/// What a doc comment says about the book.
#[derive(Debug, PartialEq, Eq)]
pub(crate) enum Reference {
    /// It cites at least one section and one pbrt-v4 symbol.
    Cited {
        sections: Vec<Section>,
        symbols: Vec<String>,
    },
    /// It declares that the item implements no pbrt-v4 algorithm.
    NotInPbrt,
    /// It names no section.
    MissingSection,
    /// It names no pbrt-v4 symbol.
    MissingSymbol,
    /// It names neither.
    MissingBoth,
}

impl Reference {
    /// Reads the citation, or its absence, from a doc comment.
    pub(crate) fn parse(doc: &str) -> Self {
        if opts_out(doc) {
            return Self::NotInPbrt;
        }
        let sections = sections(doc);
        let symbols = symbols(doc);
        match (sections.is_empty(), symbols.is_empty()) {
            (false, false) => Self::Cited { sections, symbols },
            (true, false) => Self::MissingSection,
            (false, true) => Self::MissingSymbol,
            (true, true) => Self::MissingBoth,
        }
    }
}

/// True when a line declares the item outside pbrt-v4, with a reason.
fn opts_out(doc: &str) -> bool {
    doc.lines().any(|line| {
        line.trim()
            .strip_prefix(OPT_OUT_MARKER)
            .is_some_and(|reason| !reason.trim().is_empty())
    })
}

/// Every well-formed section cited in `doc`.
fn sections(doc: &str) -> Vec<Section> {
    doc.split(SECTION_MARKER)
        .skip(1)
        .filter_map(|rest| {
            let end = rest
                .find(|c: char| !(c.is_ascii_alphanumeric() || c == '.'))
                .unwrap_or(rest.len());
            Section::parse(rest[..end].trim_end_matches('.'))
        })
        .collect::<BTreeSet<_>>()
        .into_iter()
        .collect()
}

/// Every pbrt-v4 symbol cited in `doc`.
fn symbols(doc: &str) -> Vec<String> {
    doc.split(SYMBOL_MARKER)
        .skip(1)
        .filter_map(|rest| rest.split_once('`'))
        .map(|(symbol, _)| symbol.trim())
        .filter(|symbol| !symbol.is_empty())
        .map(str::to_owned)
        .collect()
}
