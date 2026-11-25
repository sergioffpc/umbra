# Umbra

Umbra is a physically based path tracer. This glossary fixes the language of its domain.

## Scenes

**Scene**:
What umbra renders: the camera, the render settings, the geometry, the
materials and the lights of one frame, in umbra's own types, synced from a
USD stage through Hydra and immutable once loaded
(docs/adr/0006-scenes-enter-through-hydra.md).
_Avoid:_ stage (the USD side), render index (Hydra's)

**Prototype**:
Geometry the Scene holds once, for every instance of it; a mesh that is not
instanced is a prototype with one instance
(docs/adr/0006-scenes-enter-through-hydra.md).

**Instance**:
A placement of a prototype: its transform, and its own material, primvars
and light-link and shadow-link masks when it has them
(docs/adr/0006-scenes-enter-through-hydra.md).

**Material network**:
A material as the Scene holds it: nodes that are MaterialX nodedef ids or
`.oso` shaders, with their parameters and connections, and no shader code
(docs/adr/0008-materials-cross-as-neutral-networks.md).

**Fallback material**:
The material of a prim whose material does not translate or compile, or
that has none: an 18% grey diffuse
(docs/adr/0008-materials-cross-as-neutral-networks.md).

**Light link**:
Which lights illuminate an instance: a mask of the light-link categories of
the lights (docs/adr/0006-scenes-enter-through-hydra.md).

**Shadow link**:
Which lights an instance casts shadows from: a mask of the shadow-link
categories of the lights (docs/adr/0006-scenes-enter-through-hydra.md).

## Rendering

**Render settings**:
The USD prim that says how a frame is rendered: its camera, resolution and
products (docs/adr/0006-scenes-enter-through-hydra.md).

**Render product**:
An image a render writes, a file, with the render vars it holds
(docs/adr/0006-scenes-enter-through-hydra.md).

**Render var**:
One quantity a render product holds, such as the color (`color`) or the
depth of the first hit (docs/adr/0006-scenes-enter-through-hydra.md).
_Avoid:_ AOV

**Shutter**:
The interval of time a frame integrates, from the camera's `shutter:open` to
its `shutter:close`, relative to the frame
(docs/adr/0006-scenes-enter-through-hydra.md).

**Motion sample**:
The value of a transform or of a mesh's points at one time within the
shutter; a prim that moves has several, equally spaced
(docs/adr/0006-scenes-enter-through-hydra.md).

## Color

**Reference color space**:
The color space of every color in the Scene: ACEScg
(docs/adr/0007-spectral-rendering-from-acescg.md).

**Spectral uplift**:
The conversion of an ACEScg color, by its spectral kind (reflectance,
unbounded, illuminant), into a spectrum the renderer samples per wavelength
(docs/adr/0007-spectral-rendering-from-acescg.md).
