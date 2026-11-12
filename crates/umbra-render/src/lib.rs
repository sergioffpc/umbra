#![doc = include_str!("../README.md")]

mod decomposition;
mod film;
mod image;
mod renderer;
mod reproduction;
mod work_result;

pub use decomposition::{Decomposition, PassIndex, RenderError, TileBounds, TileIndex, WorkUnit};
pub use film::{Acceptance, Film, FilmHash};
pub use image::{ExrWriter, Image, ImageWriter, REPRODUCTION_RECORD_ATTRIBUTE, write_atomically};
pub use renderer::{Cancellation, Rendered, Renderer};
pub use reproduction::{BuildIdentity, ReproductionRecord, Tree};
pub use work_result::{PixelSum, WorkResult};
