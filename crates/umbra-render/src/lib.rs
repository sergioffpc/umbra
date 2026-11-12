#![doc = include_str!("../README.md")]

mod decomposition;
mod film;
mod renderer;
mod work_result;

pub use decomposition::{Decomposition, PassIndex, RenderError, TileBounds, TileIndex, WorkUnit};
pub use film::{Acceptance, Film, FilmHash};
pub use renderer::{Cancellation, Rendered, Renderer};
pub use work_result::{PixelSum, WorkResult};
