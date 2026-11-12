//! The directive grammar of a `.pbrt` file (pbrt-v4 `parse`,
//! `src/pbrt/parser.cpp`): it reads directives and hands them to a
//! [`ParserTarget`], reporting every error it can recover from.

use std::path::Path;
use std::sync::Arc;

use crate::diagnostic::{Diagnostic, Diagnostics, Location};
use crate::files::{SceneFiles, Unread};
use crate::params::{Param, ParamList, ParamType, Quoting, Value};
use crate::tokenizer::{Token, TokenKind, tokenize};

/// The directives that take a type name and a parameter list.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum TypedDirective {
    Camera,
    Film,
    Sampler,
    PixelFilter,
    Integrator,
}

impl TypedDirective {
    /// The directive's name in a Scene File.
    pub(crate) fn name(self) -> &'static str {
        match self {
            Self::Camera => "Camera",
            Self::Film => "Film",
            Self::Sampler => "Sampler",
            Self::PixelFilter => "PixelFilter",
            Self::Integrator => "Integrator",
        }
    }
}

/// A typed directive: its kind, its type name and where it is.
#[derive(Debug, Clone, Copy)]
pub(crate) struct Directive<'a> {
    pub(crate) kind: TypedDirective,
    pub(crate) type_name: &'a str,
    pub(crate) location: &'a Location,
}

/// What the grammar hands its directives to: the graphics state and the
/// Scene builder (pbrt-v4 `ParserTarget`, ADR 0003).
pub(crate) trait ParserTarget {
    /// `LookAt eye look up`.
    fn look_at(&mut self, values: [f32; 9], location: &Location, diagnostics: &mut Diagnostics);
    /// A typed directive, such as `Camera "perspective"`, with its
    /// parameters.
    fn typed(
        &mut self,
        directive: &Directive<'_>,
        params: ParamList,
        diagnostics: &mut Diagnostics,
    );
    /// `WorldBegin`.
    fn world_begin(&mut self, location: &Location, diagnostics: &mut Diagnostics);
}

/// pbrt-v4's directives that Umbra does not support yet.
const UNSUPPORTED: &[&str] = &[
    "Accelerator",
    "ActiveTransform",
    "AreaLightSource",
    "Attribute",
    "AttributeBegin",
    "AttributeEnd",
    "ColorSpace",
    "ConcatTransform",
    "CoordinateSystem",
    "CoordSysTransform",
    "Identity",
    "Import",
    "LightSource",
    "MakeNamedMaterial",
    "MakeNamedMedium",
    "Material",
    "MediumInterface",
    "NamedMaterial",
    "ObjectBegin",
    "ObjectEnd",
    "ObjectInstance",
    "Option",
    "ReverseOrientation",
    "Rotate",
    "Scale",
    "Shape",
    "Texture",
    "Transform",
    "TransformBegin",
    "TransformEnd",
    "TransformTimes",
    "Translate",
];

/// The grammar over the files of one parse.
pub(crate) struct Parser<'p> {
    pub(crate) files: &'p mut SceneFiles,
    pub(crate) target: &'p mut dyn ParserTarget,
    pub(crate) diagnostics: &'p mut Diagnostics,
}

impl Parser<'_> {
    /// Reads the directives of `text`, the contents of `file`, which
    /// [`SceneFiles::open`] has opened, and closes it.
    pub(crate) fn parse(&mut self, file: &Path, text: &str) {
        let (tokens, error) = tokenize(&Arc::from(file), text);
        let mut cursor = Cursor {
            tokens: &tokens,
            next: 0,
        };
        while let Some(token) = cursor.next() {
            self.statement(token, &mut cursor);
        }
        if let Some(error) = error {
            self.diagnostics.push(error);
        }
        self.files.close();
    }

    /// The directive that starts with `token`, its arguments next in
    /// `cursor`.
    fn statement(&mut self, token: &Token, cursor: &mut Cursor<'_>) {
        if let TokenKind::Bare(name) = &token.kind {
            return self.directive(name, &token.location, cursor);
        }
        self.error(&token.location, "expected a directive");
        cursor.skip_arguments();
    }

    /// The directive `name` at `location`, its arguments next in `cursor`.
    fn directive(&mut self, name: &str, location: &Location, cursor: &mut Cursor<'_>) {
        let typed = match name {
            "LookAt" => return self.look_at(location, cursor),
            "WorldBegin" => return self.target.world_begin(location, self.diagnostics),
            "Include" => return self.include(location, cursor),
            "Camera" => TypedDirective::Camera,
            "Film" => TypedDirective::Film,
            "Sampler" => TypedDirective::Sampler,
            "PixelFilter" => TypedDirective::PixelFilter,
            "Integrator" => TypedDirective::Integrator,
            _ => return self.unsupported(name, location, cursor),
        };
        self.typed(typed, location, cursor);
    }

    /// An unsupported or unknown directive, skipped with its arguments.
    fn unsupported(&mut self, name: &str, location: &Location, cursor: &mut Cursor<'_>) {
        let message = if UNSUPPORTED.contains(&name) {
            format!("directive {name} is not supported yet")
        } else {
            format!("unknown directive {name}")
        };
        self.error(location, message);
        cursor.skip_arguments();
    }

    /// `LookAt` and its nine numbers.
    fn look_at(&mut self, location: &Location, cursor: &mut Cursor<'_>) {
        let numbers: Option<Vec<f32>> = cursor
            .arguments()
            .iter()
            .map(|t| match &t.kind {
                TokenKind::Bare(text) => text.parse().ok().filter(|v: &f32| v.is_finite()),
                TokenKind::Open | TokenKind::Close | TokenKind::Quoted(_) => None,
            })
            .collect();
        let values = numbers.and_then(|v| <[f32; 9]>::try_from(v).ok());
        match values {
            Some(values) => self.target.look_at(values, location, self.diagnostics),
            None => self.error(location, "LookAt takes nine numbers"),
        }
    }

    /// `Include "file"`: the file's directives, read in place.
    fn include(&mut self, location: &Location, cursor: &mut Cursor<'_>) {
        let Some(name) = cursor.quoted() else {
            self.error(location, "Include takes a quoted file name");
            return cursor.skip_arguments();
        };
        let path = self.files.resolve(&name);
        match self.files.open(&path) {
            Ok(text) => self.parse(&path, &text),
            Err(Unread::Cycle) => {
                let message = format!("{} includes itself", path.display());
                self.error(location, message);
            }
            Err(Unread::Io(error)) => {
                let message = format!("cannot read {}: {error}", path.display());
                self.error(location, message);
            }
        }
    }

    /// A typed directive: its quoted type name, then its parameter list.
    fn typed(&mut self, directive: TypedDirective, location: &Location, cursor: &mut Cursor<'_>) {
        let Some(type_name) = cursor.quoted() else {
            let message = format!("{} takes a quoted type name", directive.name());
            self.error(location, message);
            return cursor.skip_arguments();
        };
        let context = format!("{} \"{type_name}\"", directive.name());
        let params = ParamList::new(self.params(cursor), context);
        let directive = Directive {
            kind: directive,
            type_name: &type_name,
            location,
        };
        self.target.typed(&directive, params, self.diagnostics);
    }

    /// The parameters next in `cursor`: each a quoted `"type name"` and a
    /// value or a bracketed list of them. Malformed ones are reported and
    /// left out.
    fn params(&mut self, cursor: &mut Cursor<'_>) -> Vec<Param> {
        let mut params = Vec::new();
        while let Some(token) = cursor.peek_quoted() {
            let location = &token.location;
            let declaration = cursor.quoted().unwrap_or_default();
            match (declaration_of(&declaration), cursor.values()) {
                (Some((ty, name)), Some(values)) => params.push(Param {
                    ty,
                    name,
                    values,
                    location: location.clone(),
                }),
                (None, _) => self.error(
                    location,
                    format!("\"{declaration}\" is not a parameter type and name"),
                ),
                (Some(_), None) => self.error(location, "a parameter has no value"),
            }
        }
        params
    }

    /// Reports an error at `location`.
    fn error(&mut self, location: &Location, message: impl Into<String>) {
        self.diagnostics.push(Diagnostic::error(location, message));
    }
}

/// `"type name"` split into its type and name.
fn declaration_of(declaration: &str) -> Option<(ParamType, String)> {
    let mut words = declaration.split_whitespace();
    let (ty, name) = (words.next()?, words.next()?);
    words
        .next()
        .is_none()
        .then(|| ParamType::from_name(ty).map(|ty| (ty, name.to_owned())))
        .flatten()
}

/// Whether `token` can be an argument: anything but a bare word that
/// starts with a letter and is not a boolean, which is a directive.
fn is_argument(token: &Token) -> bool {
    match &token.kind {
        TokenKind::Bare(text) => {
            !text.starts_with(|c: char| c.is_ascii_alphabetic())
                || text == "true"
                || text == "false"
        }
        TokenKind::Open | TokenKind::Close | TokenKind::Quoted(_) => true,
    }
}

/// A position in the tokens of one file.
struct Cursor<'t> {
    tokens: &'t [Token],
    next: usize,
}

impl<'t> Cursor<'t> {
    /// The next token, consumed.
    fn next(&mut self) -> Option<&'t Token> {
        let token = self.tokens.get(self.next)?;
        self.next += 1;
        Some(token)
    }

    /// The next token, if it is quoted, not consumed.
    fn peek_quoted(&self) -> Option<&'t Token> {
        self.tokens
            .get(self.next)
            .filter(|t| matches!(t.kind, TokenKind::Quoted(_)))
    }

    /// The next token's text, consumed, if it is quoted.
    fn quoted(&mut self) -> Option<String> {
        let TokenKind::Quoted(text) = &self.peek_quoted()?.kind else {
            return None;
        };
        self.next += 1;
        Some(text.clone())
    }

    /// Every argument token up to the next directive, consumed.
    fn arguments(&mut self) -> &'t [Token] {
        let start = self.next;
        let count = self.tokens[start..]
            .iter()
            .take_while(|t| is_argument(t))
            .count();
        self.next += count;
        &self.tokens[start..self.next]
    }

    /// Skips every argument up to the next directive.
    fn skip_arguments(&mut self) {
        self.arguments();
    }

    /// The value of a parameter, consumed: one quoted or bare token, or a
    /// bracketed list of them; `None` if there is none.
    fn values(&mut self) -> Option<Vec<Value>> {
        let token = self.tokens.get(self.next).filter(|t| is_argument(t))?;
        if token.kind != TokenKind::Open {
            self.next += 1;
            return value(token).map(|v| vec![v]);
        }
        self.next += 1;
        let list: Vec<Option<Value>> = self.tokens[self.next..]
            .iter()
            .take_while(|t| t.kind != TokenKind::Close && is_argument(t))
            .map(value)
            .collect();
        self.next += list.len();
        let closed = self
            .tokens
            .get(self.next)
            .filter(|t| t.kind == TokenKind::Close);
        self.next += usize::from(closed.is_some());
        closed.and_then(|_| list.into_iter().collect())
    }
}

/// `token` as a parameter value, if it is one.
fn value(token: &Token) -> Option<Value> {
    match &token.kind {
        TokenKind::Quoted(text) => Some(Value {
            text: text.clone(),
            quoting: Quoting::Quoted,
        }),
        TokenKind::Bare(text) => Some(Value {
            text: text.clone(),
            quoting: Quoting::Bare,
        }),
        TokenKind::Open | TokenKind::Close => None,
    }
}
