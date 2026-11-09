# umbra-camera

How a Pixel Sample becomes a ray, and radiance becomes sensor RGB: the Cameras
behind their closed `CameraKind` (ADR 0006), the Filters, the pixel sensor,
and camera sample generation.

It is a core crate: `#![no_std]` without `alloc` (ADR 0014, ADR 0015). It
depends on `umbra-geometry` and `umbra-spectrum`, and `umbra-integrators`
depends on it (arc42 §5.1, §5.2). It covers PBR 4e chapter 5 and §8.8.

> **Status:** not implemented yet. Its first code arrives with
> [#77 Black frame through Renderer and Film](https://github.com/sergioffpc/umbra/issues/77).
