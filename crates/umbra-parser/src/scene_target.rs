//! The graphics state and the Scene builder behind the grammar (pbrt-v4
//! `BasicSceneBuilder`, `src/pbrt/scene.cpp`): what each supported
//! directive sets, and the strict validation of its type and parameters
//! (ADR 0003).

use std::num::NonZeroU32;
use std::path::PathBuf;

use umbra_camera::ThinLens;
use umbra_geometry::Bounds2f;
use umbra_integrators::VolPathIntegrator;
use umbra_math::{Point2f, Point2i, Point3f, Transform, Vector2f, Vector3f, look_at};
use umbra_sampling::RandomizeStrategy;
use umbra_scene::{
    FilmParameters, GaussianFilterParameters, HaltonSamplerParameters, PerspectiveCameraParameters,
    RenderJobOptions, Scene, SceneBuilder, Seed,
};

use crate::Overrides;
use crate::diagnostic::{Diagnostic, Diagnostics, Location};
use crate::params::{ParamList, ParamType};
use crate::syntax::{Directive, ParserTarget, TypedDirective};

/// pbrt-v4's `RGBFilm` parameters that Umbra does not honour yet
/// ([#84](https://github.com/sergioffpc/umbra/issues/84)).
const UNSUPPORTED_FILM_PARAMETERS: &[&str] = &[
    "cropwindow",
    "pixelbounds",
    "diagonal",
    "savefp16",
    "maxcomponentvalue",
    "sensor",
    "iso",
    "whitebalance",
];

/// The block a directive is in.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Block {
    /// Before `WorldBegin`: the camera, film, sampler, filter and
    /// integrator.
    Options,
    /// After `WorldBegin`.
    World,
}

/// What the Render Job Options and output path take from a Scene File.
#[derive(Debug, Default)]
struct FileOptions {
    pixel_samples: Option<NonZeroU32>,
    seed: Option<Seed>,
    filename: Option<(String, Location)>,
}

/// What a Scene File has set so far.
#[derive(Debug)]
pub(crate) struct SceneTarget {
    ctm: Transform,
    block: Block,
    builder: SceneBuilder,
    file_options: FileOptions,
    film: Option<Location>,
    filter: Option<Location>,
}

/// What a parse produces when it has no error.
#[derive(Debug)]
pub(crate) struct Built {
    pub(crate) scene: Scene,
    pub(crate) options: RenderJobOptions,
    pub(crate) output: PathBuf,
}

impl SceneTarget {
    /// The state at the start of a Scene File: pbrt-v4's defaults.
    pub(crate) fn new() -> Self {
        Self {
            ctm: Transform::default(),
            block: Block::Options,
            builder: SceneBuilder::new(),
            file_options: FileOptions::default(),
            film: None,
            filter: None,
        }
    }

    /// The Scene, its Render Job Options and its output path, with
    /// `overrides` applied, unless `diagnostics` hold an error or the
    /// Scene cannot be built. `resolve` resolves a file name as an
    /// `Include` does.
    pub(crate) fn finish(
        self,
        overrides: &Overrides,
        resolve: impl Fn(&str) -> PathBuf,
        diagnostics: &mut Diagnostics,
    ) -> Option<Built> {
        if self.block == Block::Options {
            diagnostics.push(Diagnostic::unlocated("the Scene File has no WorldBegin"));
        }
        let filename = self.file_options.filename.as_ref();
        let output = output_path(overrides, filename, resolve, diagnostics);
        let options = self.options(overrides);
        if diagnostics.has_errors() {
            return None;
        }
        Some(Built {
            scene: self.build(diagnostics)?,
            options,
            output: output?,
        })
    }

    /// The Render Job Options: the overrides, else the Scene File's
    /// settings, else the defaults.
    fn options(&self, overrides: &Overrides) -> RenderJobOptions {
        let defaults = RenderJobOptions::default();
        let seed = overrides
            .seed
            .or(self.file_options.seed)
            .unwrap_or_default();
        let spp = overrides.spp.or(self.file_options.pixel_samples);
        RenderJobOptions {
            seed,
            pixel_samples_per_pixel: spp.unwrap_or(defaults.pixel_samples_per_pixel),
            ..defaults
        }
    }

    /// The Scene, or a `Diagnostic` located at the directive that makes it
    /// impossible to build.
    fn build(self, diagnostics: &mut Diagnostics) -> Option<Scene> {
        let (film, filter) = (self.film, self.filter);
        self.builder
            .build()
            .map_err(|error| {
                let location = match error {
                    umbra_scene::Error::EmptyResolution { .. } => film,
                    umbra_scene::Error::FilterRadiusTooSmall => filter,
                    umbra_scene::Error::TooManyObjects { .. } | _ => None,
                };
                diagnostics.push(Diagnostic {
                    location,
                    ..Diagnostic::unlocated(error.to_string())
                });
            })
            .ok()
    }

    /// `Camera "perspective"`.
    ///
    /// pbrt-v4 `PerspectiveCamera::Create`.
    fn camera(&mut self, mut params: ParamList, diagnostics: &mut Diagnostics) {
        let d = PerspectiveCameraParameters::default();
        let mut float = |name, default| params.float(name, diagnostics).unwrap_or(default);
        let camera = PerspectiveCameraParameters {
            world_from_camera: self.ctm.inverse(),
            fov: float("fov", d.fov),
            lens: ThinLens {
                radius: float("lensradius", d.lens.radius),
                focal_distance: float("focaldistance", d.lens.focal_distance),
            },
            shutter_open: float("shutteropen", d.shutter_open),
            shutter_close: float("shutterclose", d.shutter_close),
            frame_aspect_ratio: params.float("frameaspectratio", diagnostics),
            screen_window: params
                .floats::<4>("screenwindow", ParamType::Float, diagnostics)
                .map(|[x0, x1, y0, y1]| Bounds2f::new(Point2f::new(x0, y0), Point2f::new(x1, y1))),
        };
        params.warn_unused(diagnostics);
        self.builder.camera(&camera);
    }

    /// `Film "rgb"`, as far as Umbra honours it.
    ///
    /// pbrt-v4 `RGBFilm::Create`.
    fn film(&mut self, mut params: ParamList, location: &Location, diagnostics: &mut Diagnostics) {
        let d = FilmParameters::default().resolution;
        let mut integer = |name, default| {
            params
                .integer(name, diagnostics)
                .map_or(default, |(value, _)| value)
        };
        let resolution = Point2i::new(integer("xresolution", d.x), integer("yresolution", d.y));
        self.file_options.filename = params.string("filename", diagnostics);
        params.reject(UNSUPPORTED_FILM_PARAMETERS, diagnostics);
        params.warn_unused(diagnostics);
        self.builder.film(FilmParameters { resolution });
        self.film = Some(location.clone());
    }

    /// `Sampler "halton"`.
    ///
    /// pbrt-v4 `HaltonSampler::Create`.
    fn sampler(&mut self, mut params: ParamList, diagnostics: &mut Diagnostics) {
        if let Some((spp, location)) = params.integer("pixelsamples", diagnostics) {
            self.file_options.pixel_samples = pixel_samples(spp, &location, diagnostics);
        }
        if let Some((seed, _)) = params.integer("seed", diagnostics) {
            self.file_options.seed = Some(Seed(seed));
        }
        let randomize = params
            .string("randomization", diagnostics)
            .and_then(|(name, location)| randomization(&name, &location, diagnostics))
            .unwrap_or_default();
        params.warn_unused(diagnostics);
        self.builder.sampler(HaltonSamplerParameters { randomize });
    }

    /// `PixelFilter "gaussian"`.
    ///
    /// pbrt-v4 `GaussianFilter::Create`.
    fn pixel_filter(
        &mut self,
        mut params: ParamList,
        location: &Location,
        diagnostics: &mut Diagnostics,
    ) {
        let d = GaussianFilterParameters::default();
        let mut float = |name, default| params.float(name, diagnostics).unwrap_or(default);
        let filter = GaussianFilterParameters {
            radius: Vector2f::new(float("xradius", d.radius.x), float("yradius", d.radius.y)),
            sigma: float("sigma", d.sigma),
        };
        params.warn_unused(diagnostics);
        self.builder.filter(filter);
        self.filter = Some(location.clone());
    }

    /// `Integrator "volpath"`, without media, regularisation or any light
    /// sampler but `bvh`.
    ///
    /// pbrt-v4 `VolPathIntegrator::Create`.
    fn integrator(&mut self, mut params: ParamList, diagnostics: &mut Diagnostics) {
        let max_depth = params
            .integer("maxdepth", diagnostics)
            .and_then(|(depth, location)| max_depth(depth, &location, diagnostics))
            .unwrap_or_else(|| VolPathIntegrator::default().max_depth());
        if let Some((true, location)) = params.boolean("regularize", diagnostics) {
            let message = "Integrator \"volpath\" regularization is not supported yet";
            diagnostics.push(Diagnostic::error(&location, message));
        }
        if let Some((sampler, location)) = params.string("lightsampler", diagnostics)
            && sampler != "bvh"
        {
            let message = format!("light sampler \"{sampler}\" is not supported yet");
            diagnostics.push(Diagnostic::error(&location, message));
        }
        params.warn_unused(diagnostics);
        self.builder.integrator(VolPathIntegrator::new(max_depth));
    }
}

impl ParserTarget for SceneTarget {
    /// pbrt-v4 `BasicSceneBuilder::LookAt`: the current transformation
    /// times the look-at transformation.
    fn look_at(&mut self, values: [f32; 9], location: &Location, diagnostics: &mut Diagnostics) {
        let [ex, ey, ez, lx, ly, lz, ux, uy, uz] = values;
        let eye = Point3f::new(ex, ey, ez);
        let look = Point3f::new(lx, ly, lz);
        let up = Vector3f::new(ux, uy, uz);
        match look_at(eye, look, up) {
            Some(t) => self.ctm = self.ctm * t,
            None => diagnostics.push(Diagnostic::error(
                location,
                "LookAt's up vector is parallel to its viewing direction",
            )),
        }
    }

    fn typed(
        &mut self,
        directive: &Directive<'_>,
        params: ParamList,
        diagnostics: &mut Diagnostics,
    ) {
        let Directive {
            kind: directive,
            type_name,
            location,
        } = *directive;
        if self.block == Block::World {
            let message = format!("{} is not allowed after WorldBegin", directive.name());
            return diagnostics.push(Diagnostic::error(location, message));
        }
        let supported = match directive {
            TypedDirective::Camera => "perspective",
            TypedDirective::Film => "rgb",
            TypedDirective::Sampler => "halton",
            TypedDirective::PixelFilter => "gaussian",
            TypedDirective::Integrator => "volpath",
        };
        if type_name != supported {
            let message = format!("{} \"{type_name}\" is not supported", directive.name());
            return diagnostics.push(Diagnostic::error(location, message));
        }
        match directive {
            TypedDirective::Camera => self.camera(params, diagnostics),
            TypedDirective::Film => self.film(params, location, diagnostics),
            TypedDirective::Sampler => self.sampler(params, diagnostics),
            TypedDirective::PixelFilter => self.pixel_filter(params, location, diagnostics),
            TypedDirective::Integrator => self.integrator(params, diagnostics),
        }
    }

    /// pbrt-v4 `BasicSceneBuilder::WorldBegin`: the current transformation
    /// is reset.
    fn world_begin(&mut self, location: &Location, diagnostics: &mut Diagnostics) {
        if self.block == Block::World {
            diagnostics.push(Diagnostic::error(location, "WorldBegin appears twice"));
        }
        self.block = Block::World;
        self.ctm = Transform::default();
    }
}

/// The output path: `--outfile`, else the Film's `filename` resolved as an
/// `Include`, else `pbrt.exr`, as pbrt-v4 does; `None` with a `Diagnostic` if
/// it is not an EXR (arc42 §8.15).
fn output_path(
    overrides: &Overrides,
    filename: Option<&(String, Location)>,
    resolve: impl Fn(&str) -> PathBuf,
    diagnostics: &mut Diagnostics,
) -> Option<PathBuf> {
    let (path, location) = match (&overrides.outfile, filename) {
        (Some(outfile), _) => (outfile.clone(), None),
        (None, Some((name, location))) => (resolve(name), Some(location)),
        (None, None) => (PathBuf::from("pbrt.exr"), None),
    };
    let is_exr = path
        .extension()
        .is_some_and(|e| e.eq_ignore_ascii_case("exr"));
    if is_exr {
        return Some(path);
    }
    let message = format!("the output {} is not an EXR (.exr) file", path.display());
    diagnostics.push(match location {
        Some(location) => Diagnostic::error(location, message),
        None => Diagnostic::unlocated(message),
    });
    None
}

/// The Halton sampler's randomization `name`.
fn randomization(
    name: &str,
    location: &Location,
    diagnostics: &mut Diagnostics,
) -> Option<RandomizeStrategy> {
    let strategy = match name {
        "none" => Some(RandomizeStrategy::None),
        "permutedigits" => Some(RandomizeStrategy::PermuteDigits),
        "owen" => Some(RandomizeStrategy::Owen),
        _ => None,
    };
    if strategy.is_none() {
        let message = format!("Sampler \"halton\" has no randomization \"{name}\"");
        diagnostics.push(Diagnostic::error(location, message));
    }
    strategy
}

/// `pixelsamples`, given as `value` at `location`, if it is at least 1.
fn pixel_samples(
    value: i32,
    location: &Location,
    diagnostics: &mut Diagnostics,
) -> Option<NonZeroU32> {
    let positive = u32::try_from(value).ok().and_then(NonZeroU32::new);
    if positive.is_none() {
        let message = format!("parameter \"pixelsamples\" must be at least 1, not {value}");
        diagnostics.push(Diagnostic::error(location, message));
    }
    positive
}

/// `maxdepth`, given as `value` at `location`, if it is not negative.
fn max_depth(value: i32, location: &Location, diagnostics: &mut Diagnostics) -> Option<u32> {
    let depth = u32::try_from(value).ok();
    if depth.is_none() {
        let message = format!("parameter \"maxdepth\" must not be negative, not {value}");
        diagnostics.push(Diagnostic::error(location, message));
    }
    depth
}
