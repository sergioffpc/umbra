//! What can go wrong cutting a Render Job into Work Units.

use thiserror::Error;

/// Why a Render Job cannot be cut into Work Units.
#[derive(Debug, Clone, PartialEq, Eq, Error)]
#[non_exhaustive]
pub enum Error {
    /// More Tiles than a `u32` counts.
    #[error("the image has more than 2^32 - 1 Tiles")]
    TooManyTiles,
    /// A Tile size larger than an image can be.
    #[error("the Tile size is above 2^31 - 1 pixels")]
    TileTooLarge,
}
