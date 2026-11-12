//! The parameter lists of directives (pbrt-v4 `ParameterDictionary`,
//! `src/pbrt/paramdict.h`): typed lookups that consume what they read, so
//! what is left over is unsupported or unused.

use crate::diagnostic::{Diagnostic, Diagnostics, Location};

/// The type a parameter is declared with, as in `"float fov"`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum ParamType {
    Integer,
    Float,
    Point2,
    Vector2,
    Point3,
    Vector3,
    Normal3,
    Bool,
    String,
    Texture,
    Spectrum,
    Rgb,
    Blackbody,
}

impl ParamType {
    /// The type `name` declares, with pbrt-v4's synonyms (`point`,
    /// `vector`, `normal`, `color`), or `None` for no type.
    pub(crate) fn from_name(name: &str) -> Option<Self> {
        Some(match name {
            "integer" => Self::Integer,
            "float" => Self::Float,
            "point2" => Self::Point2,
            "vector2" => Self::Vector2,
            "point3" | "point" => Self::Point3,
            "vector3" | "vector" => Self::Vector3,
            "normal3" | "normal" => Self::Normal3,
            "bool" => Self::Bool,
            "string" => Self::String,
            "texture" => Self::Texture,
            "spectrum" => Self::Spectrum,
            "rgb" | "color" => Self::Rgb,
            "blackbody" => Self::Blackbody,
            _ => return None,
        })
    }

    /// The type's name in a Scene File.
    fn name(self) -> &'static str {
        match self {
            Self::Integer => "integer",
            Self::Float => "float",
            Self::Point2 => "point2",
            Self::Vector2 => "vector2",
            Self::Point3 => "point3",
            Self::Vector3 => "vector3",
            Self::Normal3 => "normal3",
            Self::Bool => "bool",
            Self::String => "string",
            Self::Texture => "texture",
            Self::Spectrum => "spectrum",
            Self::Rgb => "rgb",
            Self::Blackbody => "blackbody",
        }
    }
}

/// How a value is written.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum Quoting {
    /// In double quotes.
    Quoted,
    /// Bare: a number, `true` or `false`.
    Bare,
}

/// One value of a parameter, as written.
#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct Value {
    pub(crate) text: String,
    pub(crate) quoting: Quoting,
}

/// One parameter of a directive.
#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct Param {
    pub(crate) ty: ParamType,
    pub(crate) name: String,
    pub(crate) values: Vec<Value>,
    pub(crate) location: Location,
}

/// The parameters of one directive, and what it is called in Diagnostics,
/// such as `Camera "perspective"`.
#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct ParamList {
    params: Vec<Param>,
    context: String,
}

impl ParamList {
    /// The list `params` of the directive named `context`.
    pub(crate) fn new(params: Vec<Param>, context: String) -> Self {
        Self { params, context }
    }

    /// The value of the one-`float` parameter `name`, if given and valid.
    pub(crate) fn float(&mut self, name: &str, diagnostics: &mut Diagnostics) -> Option<f32> {
        let [value] = self.floats::<1>(name, ParamType::Float, diagnostics)?;
        Some(value)
    }

    /// The `N` values of the parameter `name` of type `ty`, all numbers, if
    /// given and valid.
    pub(crate) fn floats<const N: usize>(
        &mut self,
        name: &str,
        ty: ParamType,
        diagnostics: &mut Diagnostics,
    ) -> Option<[f32; N]> {
        let param = self.take(name, ty, diagnostics)?;
        let values = Self::exactly::<N>(&param, diagnostics)?;
        let parsed = values.map(|v| parse_float(v, &param.location, diagnostics));
        parsed
            .iter()
            .all(Option::is_some)
            .then(|| parsed.map(Option::unwrap_or_default))
    }

    /// The value of the one-`integer` parameter `name`, and where it is, if
    /// given and valid.
    pub(crate) fn integer(
        &mut self,
        name: &str,
        diagnostics: &mut Diagnostics,
    ) -> Option<(i32, Location)> {
        let param = self.take(name, ParamType::Integer, diagnostics)?;
        let [value] = Self::exactly::<1>(&param, diagnostics)?;
        let parsed = (value.quoting == Quoting::Bare)
            .then(|| value.text.parse::<i32>().ok())
            .flatten();
        if parsed.is_none() {
            let message = format!("\"{}\" is not an integer", value.text);
            diagnostics.push(Diagnostic::error(&param.location, message));
        }
        parsed.map(|v| (v, param.location))
    }

    /// The value of the one-`string` parameter `name`, and where it is, if
    /// given and valid.
    pub(crate) fn string(
        &mut self,
        name: &str,
        diagnostics: &mut Diagnostics,
    ) -> Option<(String, Location)> {
        let param = self.take(name, ParamType::String, diagnostics)?;
        let [value] = Self::exactly::<1>(&param, diagnostics)?;
        if value.quoting == Quoting::Bare {
            let message = format!("{} is not a quoted string", value.text);
            diagnostics.push(Diagnostic::error(&param.location, message));
            return None;
        }
        Some((value.text.clone(), param.location))
    }

    /// The value of the one-`bool` parameter `name`, and where it is, if
    /// given and valid.
    pub(crate) fn boolean(
        &mut self,
        name: &str,
        diagnostics: &mut Diagnostics,
    ) -> Option<(bool, Location)> {
        let param = self.take(name, ParamType::Bool, diagnostics)?;
        let [value] = Self::exactly::<1>(&param, diagnostics)?;
        let parsed = match value.text.as_str() {
            "true" => Some(true),
            "false" => Some(false),
            _ => None,
        };
        if parsed.is_none() {
            let message = format!("\"{}\" is neither true nor false", value.text);
            diagnostics.push(Diagnostic::error(&param.location, message));
        }
        parsed.map(|b| (b, param.location))
    }

    /// Refuses each remaining parameter named in `names`: pbrt-v4 honours
    /// them, Umbra does not yet.
    pub(crate) fn reject(&mut self, names: &[&str], diagnostics: &mut Diagnostics) {
        let (rejected, kept) = self
            .params
            .drain(..)
            .partition(|p| names.contains(&p.name.as_str()));
        self.params = kept;
        for param in rejected {
            let message = format!(
                "{} parameter \"{}\" is not supported yet",
                self.context, param.name
            );
            diagnostics.push(Diagnostic::error(&param.location, message));
        }
    }

    /// Warns of every remaining parameter: no one read it.
    pub(crate) fn warn_unused(self, diagnostics: &mut Diagnostics) {
        for param in self.params {
            let message = format!("{} does not use parameter \"{}\"", self.context, param.name);
            diagnostics.push(Diagnostic::warning(&param.location, message));
        }
    }

    /// Removes and returns the parameter `name`, if it is declared `ty`;
    /// refuses it if it is declared otherwise.
    fn take(&mut self, name: &str, ty: ParamType, diagnostics: &mut Diagnostics) -> Option<Param> {
        let index = self.params.iter().position(|p| p.name == name)?;
        let param = self.params.remove(index);
        if param.ty == ty {
            return Some(param);
        }
        let message = format!(
            "{} parameter \"{name}\" is \"{}\", not \"{}\"",
            self.context,
            ty.name(),
            param.ty.name()
        );
        diagnostics.push(Diagnostic::error(&param.location, message));
        None
    }

    /// The `N` values of `param`, if it has exactly `N`.
    fn exactly<'p, const N: usize>(
        param: &'p Param,
        diagnostics: &mut Diagnostics,
    ) -> Option<[&'p Value; N]> {
        let values = <&[Value; N]>::try_from(param.values.as_slice()).ok();
        if values.is_none() {
            let message = format!(
                "parameter \"{}\" takes {N} value(s), not {}",
                param.name,
                param.values.len()
            );
            diagnostics.push(Diagnostic::error(&param.location, message));
        }
        values.map(<[Value; N]>::each_ref)
    }
}

/// `value` as a finite `f32`, or a `Diagnostic` at `location`.
fn parse_float(value: &Value, location: &Location, diagnostics: &mut Diagnostics) -> Option<f32> {
    let parsed = (value.quoting == Quoting::Bare)
        .then(|| value.text.parse::<f32>().ok())
        .flatten()
        .filter(|v| v.is_finite());
    if parsed.is_none() {
        let message = format!("\"{}\" is not a finite number", value.text);
        diagnostics.push(Diagnostic::error(location, message));
    }
    parsed
}
