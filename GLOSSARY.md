# Umbra

Umbra is a physically based path tracer. This glossary fixes the language of its domain.

## Scenes

**Scene**:
What umbra renders: the render settings, the geometry, the materials and
the lights of one frame, in umbra's own types, synced from a USD stage
through Hydra and immutable once loaded
(docs/adr/0006-scenes-enter-through-hydra.md).
_Avoid:_ stage (the USD side), render index (Hydra's)

**Prototype**:
Geometry the Scene holds once, for every instance of it; a mesh that is not
instanced is a prototype with one instance
(docs/adr/0006-scenes-enter-through-hydra.md).

**Instance**:
A placement of a prototype: its transform, and its own primvars when it
has them (docs/adr/0006-scenes-enter-through-hydra.md).

**Source face**:
The polygon of a mesh a triangle comes from, as an index into the mesh's
original faces: per-face texturing (Ptex) maps a hit through it.

**Material network**:
A material as the Scene holds it: nodes that are MaterialX nodedef ids or
`.oso` shaders, with their parameters and connections, and no shader code
(docs/adr/0008-materials-cross-as-neutral-networks.md).

**Fallback material**:
An 18% gray diffuse: the material of a prim whose material does not
translate, or that has none and no `displayColor`
(docs/adr/0008-materials-cross-as-neutral-networks.md).

## Rendering

**Render settings**:
The `UsdRender` prim a render uses: its camera, resolution and render
products (docs/adr/0006-scenes-enter-through-hydra.md).

**Render product**:
An image file a render writes, holding render vars
(docs/adr/0006-scenes-enter-through-hydra.md).

**Render var**:
One quantity per pixel a render product holds, such as the color (`color`)
(docs/adr/0006-scenes-enter-through-hydra.md).
_Avoid:_ AOV

**Shutter**:
The interval of time a frame integrates, from the camera's `shutter:open` to
its `shutter:close`, relative to the frame
(docs/adr/0006-scenes-enter-through-hydra.md).

**Motion sample**:
The value of a transform or of a mesh's points at one of the equally spaced
times within the shutter, for a prim that moves
(docs/adr/0006-scenes-enter-through-hydra.md).

## Color

**Reference color space**:
ACEScg: the color space of every color value in the Scene, and of every
texel once the renderer reads it
(docs/adr/0007-spectral-rendering-from-acescg.md).

**Spectral uplift**:
The conversion of an ACEScg color into a spectrum the renderer samples per
wavelength, scaled by its spectral kind: reflectance, unbounded or
illuminant (docs/adr/0007-spectral-rendering-from-acescg.md).
