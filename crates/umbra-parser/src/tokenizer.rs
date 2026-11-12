//! The lexical layer of a `.pbrt` file (pbrt-v4 `Tokenizer`,
//! `src/pbrt/parser.cpp`), with `nom` and `nom_locate` (ADR 0003).

use std::path::Path;
use std::sync::Arc;

use nom::branch::alt;
use nom::bytes::complete::{is_not, tag};
use nom::character::complete::{anychar, char, multispace1, not_line_ending};
use nom::combinator::{map, recognize, value};
use nom::multi::{many0_count, many1_count};
use nom::sequence::{delimited, pair};
use nom::{IResult, Parser};
use nom_locate::LocatedSpan;

use crate::error::{Location, ParserError};

/// The text of a file, with positions.
type Span<'a> = LocatedSpan<&'a str>;

/// What a token is.
#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) enum TokenKind {
    /// `[`.
    Open,
    /// `]`.
    Close,
    /// A quoted string, its escapes resolved.
    Quoted(String),
    /// Anything else: a directive name, a number, `true` or `false`.
    Bare(String),
}

/// A token and where it starts.
#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct Token {
    pub(crate) kind: TokenKind,
    pub(crate) location: Location,
}

/// The tokens of `text`, read from `file`, up to the first lexical error,
/// which is returned with them.
pub(crate) fn tokenize(file: &Arc<Path>, text: &str) -> (Vec<Token>, Option<ParserError>) {
    let mut tokens = Vec::new();
    let mut input = Span::new(text);
    loop {
        input = skip(input);
        if input.fragment().is_empty() {
            return (tokens, None);
        }
        let location = location(file, input);
        let Ok((rest, kind)) = token(input) else {
            let message = "a quoted string is not closed on its line";
            return (tokens, Some(ParserError::error(&location, message)));
        };
        tokens.push(Token { kind, location });
        input = rest;
    }
}

/// Where `input` starts in `file`.
fn location(file: &Arc<Path>, input: Span<'_>) -> Location {
    let column = input.get_utf8_column();
    Location {
        file: Arc::clone(file),
        line: input.location_line(),
        column: u32::try_from(column).unwrap_or(u32::MAX),
    }
}

/// `input` after any whitespace and comments.
fn skip(input: Span<'_>) -> Span<'_> {
    let comment = recognize(pair(char('#'), not_line_ending));
    let skipped: IResult<Span<'_>, usize> = many0_count(alt((multispace1, comment))).parse(input);
    skipped.map_or(input, |(rest, _)| rest)
}

/// One token.
fn token(input: Span<'_>) -> IResult<Span<'_>, TokenKind> {
    alt((
        value(TokenKind::Open, char('[')),
        value(TokenKind::Close, char(']')),
        map(quoted, |body| TokenKind::Quoted(unescape(body.fragment()))),
        map(is_not(" \t\r\n\"[]#"), |bare: Span<'_>| {
            TokenKind::Bare((*bare.fragment()).to_owned())
        }),
    ))
    .parse(input)
}

/// A quoted string on one line: its body, escapes unresolved.
fn quoted(input: Span<'_>) -> IResult<Span<'_>, Span<'_>> {
    let escape = recognize(pair(tag("\\"), anychar));
    let body = recognize(many1_count(alt((is_not("\"\\\n"), escape))));
    let empty = tag("");
    delimited(char('"'), alt((body, empty)), char('"')).parse(input)
}

/// `body` with pbrt-v4's escapes resolved: `\b`, `\f`, `\n`, `\r`, `\t`,
/// and any other character for itself.
fn unescape(body: &str) -> String {
    let mut out = String::with_capacity(body.len());
    let mut chars = body.chars();
    while let Some(c) = chars.next() {
        if c != '\\' {
            out.push(c);
            continue;
        }
        out.push(match chars.next() {
            Some('b') => '\u{8}',
            Some('f') => '\u{c}',
            Some('n') => '\n',
            Some('r') => '\r',
            Some('t') => '\t',
            Some(other) => other,
            None => '\\',
        });
    }
    out
}
