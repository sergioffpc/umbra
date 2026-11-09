# umbra-scene

The owning Scene: its per-type arenas with typed `u32` handles (ADR 0008),
the `SceneBuilder` that fills them, the deterministic BVH construction, and
the Render Job Options. `SceneBuilder` is the seam that makes a Scene
independent of the file format it was read from: the parser is one adapter,
and tests that build Scenes directly are the other.

It is a `std` crate, outside the core. It depends on `umbra-integrators`, with
the core's `serde` feature on so that a Scene can be replicated to the Workers,
and `umbra-parser` and `umbra-render` depend on it (arc42 §5.1). Its contract
is sketched in arc42 §5.3.

> **Status:** not implemented yet. Its first code arrives with
> [#77 Black frame through Renderer and Film](https://github.com/sergioffpc/umbra/issues/77).
