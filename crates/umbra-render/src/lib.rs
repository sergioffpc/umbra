#![doc = include_str!("../README.md")]

mod decomposition;
mod error;
mod film;
mod image;
mod renderer;
mod reproduction;
mod work_result;

pub use decomposition::{Decomposition, PassIndex, TileBounds, TileIndex, WorkUnit};
pub use error::Error;
pub use film::{Acceptance, Film, FilmHash};
pub use image::{ExrWriter, Image, ImageWriter, write_atomically};
pub use renderer::{Cancellation, Rendered, Renderer};
pub use reproduction::{BuildIdentity, ReproductionRecord, Tree};
pub use work_result::{PixelSum, WorkResult};
