# umbra-shading

What light does at a point: Textures, Materials, BxDFs, Lights and light
samplers, and Media, each interface behind its closed `*Kind` enum
(ADR 0006). This is where an interaction from `umbra-geometry` becomes a BSDF.

It is a core crate: `#![no_std]` without `alloc` (ADR 0014, ADR 0015). It
depends on `umbra-geometry` and `umbra-spectrum`, and `umbra-integrators`
depends on it (arc42 §5.1, §5.2). It covers PBR 4e chapters 9–12.

> **Status:** not implemented yet. Its first code arrives with
> [#79 Emissive spheres visible to the camera](https://github.com/sergioffpc/umbra/issues/79).
