# Umbra

Umbra is a physically based path tracer. This glossary fixes the language of its domain.

## Devices

**Device**:
What a render runs on: a CPU NUMA node (`cpu0`, `cpu1`, …) or a GPU
(`gpu0`, `gpu1`, …). In the hot path, it reads only its own memory
(docs/adr/0004-render-devices.md,
docs/adr/0013-devices-render-from-their-own-memory.md).

**Render data**:
What a device builds and renders from: its acceleration structures, and
the buffers its kernels shade from, in its own memory
(docs/adr/0012-umbra-is-a-hydra-renderer.md).
_Avoid:_ scene

**Host copy**:
What a prim of the render delegate converts its sync into, in umbra's
types, and holds until the end of the commit
(docs/adr/0013-devices-render-from-their-own-memory.md).
_Avoid:_ scene, render data

**Change set**:
The prims added, updated and removed since the last commit, by id, with
what changed in each (docs/adr/0013-devices-render-from-their-own-memory.md).

**Commit**:
The hand-off of the change set to every device, between the sync of the
prims and the render pass
(docs/adr/0013-devices-render-from-their-own-memory.md).

**Frame parameters**:
What a device receives for each frame besides render data: the shutter,
the active camera and the render buffers to fill
(docs/adr/0013-devices-render-from-their-own-memory.md).

**Tile**:
A block of 64×64 pixels of a render product's data window, smaller on its
right and bottom edges: the unit of work devices take, in batches, from
the counter they share while the product renders
(docs/adr/0014-frames-render-in-tiles-from-one-counter.md).

**Film**:
Where a device renders its tiles, in its own memory: float32 values of
every render var over a render product's data window, from which the device
copies its tiles into the render buffers once the product's tiles are all
taken (docs/adr/0014-frames-render-in-tiles-from-one-counter.md).

## What umbra renders

**Prototype**:
Geometry a device holds once, for every instance of it; a mesh that is not
instanced is a prototype with one instance
(docs/adr/0012-umbra-is-a-hydra-renderer.md).

**Instance**:
A placement of a prototype: its transform, and its own primvars when it
has them (docs/adr/0012-umbra-is-a-hydra-renderer.md).

**Light**:
A UsdLux light, as a device holds it: its shape (sphere, rect, disk, cylinder,
distant or dome) in meters, given by its parameters, its emission, as an
ACEScg color and an optional blackbody temperature, and its transform. It
only emits, lights every instance and has no artistic control
(docs/adr/0010-lights-are-physical.md).
_Avoid:_ light for a mesh light (an emissive mesh)

**Tilt factor**:
What scales one instance of a Light's emission when its IES profile has
TILT data (IESNA LM-63): the profile's multiplying factor at the
inclination of that instance at the frame, the angle between the profile's
nadir, in world space, and the stage's down direction. It is 1 when the
profile has no TILT data, and applies whether or not the profile is
normalized.

**Emissive mesh**:
A mesh whose surface emits light: its emission authored by UsdLux's
MeshLightAPI (a mesh light) or by its material. Its shape is its triangles,
and it reflects light as well as it emits: it is geometry, not a Light.

**Emitter**:
Anything that emits light: a Light or an emissive mesh.

**Source face**:
The polygon of a mesh a triangle comes from, as an index into the mesh's
original faces: per-face texturing (Ptex) maps a hit through it.

**Material network**:
A material as it crosses from Hydra: nodes that are MaterialX nodedef ids or
`.oso` shaders, with their parameters and connections, and no shader code
(docs/adr/0008-materials-cross-as-neutral-networks.md).

**Fallback material**:
An 18% gray diffuse: the material of a prim whose material does not
translate, or that has none and no `displayColor`
(docs/adr/0008-materials-cross-as-neutral-networks.md).

## Rendering

**Render settings**:
The `UsdRender` prim a render uses: its camera, resolution and render
products (docs/adr/0012-umbra-is-a-hydra-renderer.md).

**Render product**:
An image file a render writes, holding render vars
(docs/adr/0012-umbra-is-a-hydra-renderer.md).

**Render var**:
One quantity per pixel a render product holds, such as the color (`color`)
(docs/adr/0012-umbra-is-a-hydra-renderer.md).
_Avoid:_ AOV

**Shutter**:
The interval of time a frame integrates, from the camera's `shutter:open` to
its `shutter:close`, relative to the frame; none when the render settings
disable motion blur (docs/adr/0012-umbra-is-a-hydra-renderer.md).

**Motion sample**:
The value of a transform or of a mesh's points at one of the equally spaced
times within the shutter, for a prim that moves
(docs/adr/0012-umbra-is-a-hydra-renderer.md).

## Color

**Reference color space**:
ACEScg: the color space of every color value in render data, and of every
texel once the renderer reads it
(docs/adr/0007-spectral-rendering-from-acescg.md).

**Spectral uplift**:
The conversion of an ACEScg color into a spectrum the renderer samples per
wavelength, scaled by its spectral kind: reflectance, unbounded or
illuminant (docs/adr/0007-spectral-rendering-from-acescg.md).
