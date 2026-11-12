//! Why a Scene cannot be built.

use std::error::Error;
use std::fmt;

use umbra_math::Point2i;

/// Why `SceneBuilder::build`, or adding to a Scene, failed.
#[derive(Debug, Clone, PartialEq, Eq)]
#[non_exhaustive]
pub enum SceneError {
    /// An arena would hold more objects than a `u32` handle can index
    /// (ADR 0008).
    TooManyObjects {
        /// The type of the objects.
        type_name: &'static str,
    },
    /// The film has no pixel.
    EmptyResolution {
        /// The resolution given.
        resolution: Point2i,
    },
    /// A filter radius is too small to tabulate the filter: under 1/32.
    FilterRadiusTooSmall,
}

impl fmt::Display for SceneError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::TooManyObjects { type_name } => {
                write!(
                    f,
                    "more than {} objects of type `{type_name}`",
                    u32::MAX - 1
                )
            }
            Self::EmptyResolution { resolution } => write!(
                f,
                "the film's resolution {} x {} has no pixel",
                resolution.x, resolution.y
            ),
            Self::FilterRadiusTooSmall => write!(f, "a filter radius is below 1/32"),
        }
    }
}

impl Error for SceneError {}
