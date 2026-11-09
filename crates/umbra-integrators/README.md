# umbra-integrators

How one Pixel Sample is computed from start to end: the per-Pixel-Sample body
of each integrator, the `SceneView` it reads the Scene through (borrowed arena
slices, never `&Scene`), and the `Recorder` trait through which a Pixel Trace
watches the same code path that renders (ADR 0011).

It is a core crate: `#![no_std]` without `alloc` (ADR 0014, ADR 0015), and the
top of the core. It depends on `umbra-shading` and `umbra-camera`, and
`umbra-scene` and `umbra-render` depend on it (arc42 §5.1, §5.2). It covers
PBR 4e chapters 13–15.

> **Status:** not implemented yet. Its first code arrives with
> [#77 Black frame through Renderer and Film](https://github.com/sergioffpc/umbra/issues/77).
