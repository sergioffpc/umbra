//! Why a Scene cannot be built.

use thiserror::Error;
use umbra_math::Point2i;

/// Why `SceneBuilder::build`, or adding to a Scene, failed.
#[derive(Debug, Clone, PartialEq, Eq, Error)]
#[non_exhaustive]
pub enum Error {
    /// An arena would hold more objects than a `u32` handle can index
    /// (ADR 0008).
    #[error("more than {} objects of type `{type_name}`", u32::MAX - 1)]
    TooManyObjects {
        /// The type of the objects.
        type_name: &'static str,
    },
    /// The film has no pixel.
    #[error("the film's resolution {} x {} has no pixel", .resolution.x, .resolution.y)]
    EmptyResolution {
        /// The resolution given.
        resolution: Point2i,
    },
    /// A filter radius is too small to tabulate the filter: under 1/32.
    #[error("a filter radius is below 1/32")]
    FilterRadiusTooSmall,
}
