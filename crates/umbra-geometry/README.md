# umbra-geometry

Where a ray hits: `Ray` and `Bounds`, the interactions a hit produces, the
Shapes behind their closed `ShapeKind` (ADR 0006), and the BVH node layout and
its traversal. An interaction names its Material, area Light and Medium by
`u32` ID (ADR 0008); turning it into a BSDF happens in `umbra-shading`.

It is a core crate: `#![no_std]` without `alloc` (ADR 0014, ADR 0015). It
depends on `umbra-math` and `umbra-sampling`, and `umbra-shading` and
`umbra-camera` depend on it (arc42 §5.1, §5.2). It covers PBR 4e chapters 6–7.

> **Status:** not implemented yet. Its first code arrives with
> [#79 Emissive spheres visible to the camera](https://github.com/sergioffpc/umbra/issues/79).
