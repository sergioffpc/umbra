#![doc = include_str!("../README.md")]

mod atomic_file;
mod decomposition;
mod error;
mod film;
mod image;
mod rendered;
mod reproduction;
mod work_result;

pub use atomic_file::write_atomically;
pub use decomposition::{Decomposition, PassIndex, TileBounds, TileIndex, WorkUnit};
pub use error::Error;
pub use film::{Acceptance, Film, FilmHash};
pub use image::Image;
pub use rendered::{Cancellation, Meter, Rendered};
pub use reproduction::{BuildIdentity, ReproductionRecord, Tree};
pub use work_result::{PixelSum, WorkResult};
