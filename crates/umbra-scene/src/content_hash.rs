//! The content hash of what a Scene was read from (arc42 §8.9).

use std::fmt;

/// A SHA-256 hash of the files a Scene was read from: a Scene File, its
/// includes and its assets. The Reproduction Record carries it, so that
/// `umbra trace` can tell whether the files changed since the render.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct ContentHash(pub [u8; 32]);

impl fmt::Display for ContentHash {
    /// Lowercase hexadecimal, 64 digits.
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        self.0.iter().try_for_each(|b| write!(f, "{b:02x}"))
    }
}
