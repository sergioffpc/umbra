//! Typed index handles into a Scene's arenas (ADR 0008).

use core::cmp::Ordering;
use core::fmt;
use core::hash::{Hash, Hasher};
use core::marker::PhantomData;

/// The index of a `T` in its Scene's arena of `T`s: a typed `u32`, so a
/// handle to one type cannot index another's arena. Objects refer to one
/// another by handle, never by reference, `Box` or `Arc` (ADR 0008).
///
/// A handle is meaningful only for the Scene whose builder minted it; that
/// is not checked.
#[repr(transparent)]
pub struct Handle<T> {
    index: u32,
    target: PhantomData<fn() -> T>,
}

impl<T> Handle<T> {
    /// The handle of the `T` at `index` of its arena.
    #[inline]
    #[must_use]
    pub const fn new(index: u32) -> Self {
        Self {
            index,
            target: PhantomData,
        }
    }

    /// The index into the arena.
    #[inline]
    #[must_use]
    pub const fn index(self) -> u32 {
        self.index
    }
}

// Implemented by hand: deriving them would require `T` to implement them
// too, and a handle is a `u32` whatever it points to.
impl<T> Clone for Handle<T> {
    fn clone(&self) -> Self {
        *self
    }
}

impl<T> Copy for Handle<T> {}

impl<T> PartialEq for Handle<T> {
    fn eq(&self, other: &Self) -> bool {
        self.index == other.index
    }
}

impl<T> Eq for Handle<T> {}

impl<T> PartialOrd for Handle<T> {
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        Some(self.cmp(other))
    }
}

impl<T> Ord for Handle<T> {
    fn cmp(&self, other: &Self) -> Ordering {
        self.index.cmp(&other.index)
    }
}

impl<T> Hash for Handle<T> {
    fn hash<H: Hasher>(&self, state: &mut H) {
        self.index.hash(state);
    }
}

impl<T> fmt::Debug for Handle<T> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "Handle({})", self.index)
    }
}

impl<T> From<Handle<T>> for usize {
    fn from(handle: Handle<T>) -> Self {
        usize::try_from(handle.index).expect("a u32 index fits usize on every target")
    }
}
