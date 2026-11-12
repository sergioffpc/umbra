//! Per-type arenas (ADR 0008): one `Vec<T>` per concrete type, indexed by
//! typed `u32` handles.

use umbra_geometry::Handle;

use crate::error::Error;

/// The objects of one type in a Scene, which refer to one another by
/// [`Handle`]. Only a `SceneBuilder` pushes; a built Scene's arenas are
/// immutable, and per-sample code reads them as slices (ADR 0008, ADR 0014).
#[derive(Debug, Clone, PartialEq)]
pub struct Arena<T> {
    items: Vec<T>,
}

impl<T> Default for Arena<T> {
    fn default() -> Self {
        Self { items: Vec::new() }
    }
}

impl<T> Arena<T> {
    /// An empty arena.
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// Adds `item`, and mints its handle.
    ///
    /// # Errors
    ///
    /// [`Error::TooManyObjects`] if the arena already holds `u32::MAX`
    /// objects: a handle is a `u32` (ADR 0008).
    pub fn push(&mut self, item: T) -> Result<Handle<T>, Error> {
        let index = u32::try_from(self.items.len())
            .ok()
            .filter(|&i| i < u32::MAX)
            .ok_or_else(|| Error::TooManyObjects {
                type_name: core::any::type_name::<T>(),
            })?;
        self.items.push(item);
        Ok(Handle::new(index))
    }

    /// The object of `handle`, or `None` if it is not in this arena.
    #[must_use]
    pub fn get(&self, handle: Handle<T>) -> Option<&T> {
        self.items.get(usize::from(handle))
    }

    /// Every object, in the order of their handles: what a `SceneView`
    /// borrows.
    #[must_use]
    pub fn as_slice(&self) -> &[T] {
        &self.items
    }

    /// The number of objects.
    #[must_use]
    pub fn len(&self) -> usize {
        self.items.len()
    }

    /// Whether the arena holds no object.
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.items.is_empty()
    }
}
