# umbra is a Hydra renderer: prims sync into each device's render data

This decision supersedes docs/adr/0006-scenes-enter-through-hydra.md. It
also revises docs/adr/0004-render-devices.md on where the geometry lives.

umbra renders USD stages as a Hydra renderer. A stage reaches umbra through
Hydra 2.0. A `UsdImagingStageSceneIndex`, followed by the `usdImaging` and
`hdsi` filter chain (purposes, visibility, light linking, render settings,
implicit surfaces), feeds an `HdRenderIndex` whose render delegate is
umbra's. Hydra resolves what USD composes (instancing, materials, lights,
the render settings) into prims a renderer consumes. The filters are
OpenUSD's own, and umbra does not reimplement them.

No Scene sits between Hydra and the devices. When a prim of the render
delegate syncs, it converts what it receives into umbra's types, laid out
for the devices: structures of arrays, aligned for upload. It hands the
result to every device the render uses, and each device builds its render
data from it. A device's render data is its acceleration structures
(Embree's BVHs on a CPU device, OptiX's GAS and IAS on a GPU) and the
buffers its kernels shade from. The integrator reads only render data and
holds no OpenUSD type. OpenUSD stays in the bridge, `src/ext/usd`, which is
the only code that holds OpenUSD's types, exceptions and RTTI.

The prims hand the devices:

- prototypes and instances, with instancing kept rather than flattened;
- materials, as neutral material networks
  (docs/adr/0008-materials-cross-as-neutral-networks.md);
- lights, which are physical (docs/adr/0010-lights-are-physical.md) and
  light everything (docs/adr/0009-lights-light-everything.md);
- the shutter, and the motion samples of the prims that move within it.

The units and conventions the devices receive are umbra's, whatever the
stage's:

- lengths are in meters, because the stage's `metersPerUnit` is applied;
- geometry is not rotated for `upAxis`; only the dome light's orientation
  uses it;
- triangles are wound consistently, because `leftHanded` geometry is
  flipped;
- asset paths (textures, `.oso` shaders) are resolved by Ar inside the
  bridge, against the layer each one comes from, so devices receive
  absolute file paths only.

## Hydra lives for the whole render

The stage, the render index and the prims stay alive until the process
ends. Each prim keeps its converted buffers as one copy in host memory.
CPU devices read that copy in place, and each GPU uploads it.

umbra's `HdRenderPass` runs the integrator. The integrator writes into
umbra's `HdRenderBuffer`s, one per render var, in host memory, where the
film of every device is merged.

## The CLI is the only host

umbra creates its render delegate directly, as an object, rather than as an
`HdRendererPlugin` found through a `plugInfo.json`. The delegate is not
meant for other hosts: a delegate built against another OpenUSD and another
C++ runtime could not load it anyway. umbra does not use `hdx`, whose tasks
draw and present on a GPU. OpenUSD builds `hdx` only with GL support
(`pxr/imaging/hdx/CMakeLists.txt`), and umbra's build of OpenUSD
(`third_party/config/OpenUSD.cmake`) leaves GL off.

A process renders a range of frames. For each frame, the CLI:

1. sets the time of the stage scene index;
2. runs `HdEngine::Execute` with umbra's task, which syncs the dirty prims
   and executes the render pass;
3. reads the render buffers and writes the render products.

Between frames, Hydra's change tracking syncs only the prims that changed.
The stage, the textures and the static geometry are loaded once. Each
device adds, updates or removes the render data of a prim: it refits or
rebuilds only the structures of the prims that changed.

## Considered options

- **A Scene between Hydra and the devices**, as ADR 0006 had it. The prims
  synced once into an immutable Scene, Hydra was torn down, and the devices
  built their render data from the Scene. That layout costs one more step
  at load. It also leaves nothing to sync incrementally, so every frame
  would need a process that loads the stage again.
- **The integrator reading Hydra's prims** for shading data. Device code
  cannot read Hydra's prims: a GPU would need its own copy anyway, and
  OpenUSD would reach the integrator.
- **A copy of the geometry per device**, one in every CPU device's NUMA node
  and one in every GPU, with nothing kept in the prims. Every read on a CPU
  would be node-local, but a machine with N sockets would hold the geometry
  N times in host memory.
- **A renderer plugin for usdview and DCCs.** A plugin needs `hdx` and GL,
  and a build against each host's OpenUSD and C++ runtime.
- **The render pass writing the render products.** That would save one copy
  of the film per frame. Instead, the CLI acts as the host and reads the
  render buffers, as any Hydra host does, which keeps the render pass free
  of files.

## Consequences

- The stage, the render index and the prims stay in memory during the
  render. That memory is the price of rendering a sequence without loading
  the stage again for every frame.
- On a machine with several sockets, the geometry of the prims is in the
  memory of one node, and CPU devices on the other nodes read it remotely.
  Each CPU device still renders from its own node's memory: its
  acceleration structures, its queues and its film.
- Render data is updatable per prim, so a device cannot build its
  structures once and treat them as frozen.
- Where ADRs 0007 to 0010 say what the Scene holds, the devices' render
  data now holds it.
- umbra cannot be used as a Hydra renderer in other hosts (usdview, a DCC).
  Its render pass and render buffers are the ones such a host would drive.
