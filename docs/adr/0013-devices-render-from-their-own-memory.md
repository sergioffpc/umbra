# Devices render from their own memory, fed a change set at each commit

This decision revises docs/adr/0012-umbra-is-a-hydra-renderer.md on where
the geometry lives, docs/adr/0004-render-devices.md on what a CPU device
shares, and docs/adr/0008-materials-cross-as-neutral-networks.md on where
shaders compile.

Every device renders as if it were a machine of its own. A device's hot
path reads only its own memory: its acceleration structures, its geometry,
its compiled shaders, its textures, its queues and its film. On a CPU
device, that memory is its NUMA node's. On a GPU, it is the GPU's. Devices
touch shared data at two points only: at the commit, when they receive
what changed, and at the end of a frame, when their films are merged into
the render buffers.

## Sync, then commit

Hydra syncs the prims in parallel, then calls the render delegate's
`CommitResources` on one thread, then runs the render pass. umbra splits its
work along those phases:

1. In `Sync`, a prim converts what it receives into its host copy, in
   umbra's types, and is recorded as added or updated. `Destroy*` records
   it as removed. No device reads anything during the sync.
2. In `CommitResources`, the delegate hands every device the frame's change
   set: the prims added, updated and removed. Each device copies what the
   change set gives it into its own memory, a CPU device on its node and a
   GPU by upload, and builds or updates its render data from it. Each
   device processes the change set as it chooses: a CPU device in parallel
   on its arena, a GPU with its uploads batched. When the commit returns,
   every upload has completed.
3. The render pass reads only the devices' own memory.

The host copy is a staging buffer. The prim is its only owner, from the
sync to the end of the commit, and then frees it. Devices read it only
during the commit, and never read the memory of a removed prim: they drop
what they built from it. Only the devices' copies persist, one in each
device's memory. A desktop with one NUMA node and no GPU holds the geometry
once.

A prim is named by an id that the delegate assigns: a dense integer per
kind (meshes, instancers, lights, materials, cameras, textures), reused once
freed. The ids are the same on every device. The change set uses them, and
so do the cross-references of render data, such as an instance's prototype
and material. Each device keeps its structures in tables indexed by id, and
gives nothing back.

An updated prim carries umbra's change flags, which the bridge maps from
Hydra's dirty bits, so dirty bits stay in `src/ext/usd`. A mesh has five:

- `kPlacement`: its instances, or their transforms, changed;
- `kPoints`: its points changed, and its topology did not;
- `kTopology`: its triangles changed;
- `kPrimvars`: its normals or another primvar changed;
- `kMaterials`: its material, or a material of one of its subsets, changed.

A light, a material or a camera is only changed. A device applies a partial
update to its own copy, which keeps the rest of the prim.

The shutter, the active camera and the render buffers to fill are not in the
change set. The render pass gives them to the devices as frame parameters.
The render settings stay on the host, where the CLI and the render pass
read them to create the render buffers and write the render products.

When a commit fails on any device (memory runs out, OptiX rejects a module,
a build fails), the render stops with an error that names the device and
the cause.

## Acceleration structures

Each device builds one acceleration structure per prototype, and one top
level that holds the instances: Embree's geometry BVHs and instance scene on
a CPU device, the GASes and the IAS on a GPU. Devices never share them.
Instancing has one level: nested instancers are flattened into instances of
the prototypes they place, with their transforms composed, and the
prototypes' geometry stays shared.

What a device rebuilds follows the change flags:

- `kTopology` rebuilds the prototype's structure.
- `kPoints` refits it, and rebuilds it after a fixed number of consecutive
  refits, since every refit loosens its bounds. That number is a constant
  of umbra's, not an option.
- `kPrimvars` and `kMaterials` rebuild no structure, only the buffers that
  shading reads, and a GPU's shader binding table.
- The top level is rebuilt at every commit that changes an instance.

A GPU builds a GAS without `OPTIX_BUILD_FLAG_ALLOW_UPDATE`, which slows
tracing, and adds it at the first rebuild of a prototype that received
`kPoints`. Static geometry never pays for it.

## Layout

Each attribute of a prim is one array. Points are `float3` elements at a
16-byte stride, and indices three `uint32` per triangle. Each motion sample
has its own array of points. Every buffer is aligned to 16 bytes. Embree
takes these arrays as shared buffers and OptiX as vertex and index buffers,
with no conversion.

## Materials and textures

A material's host copy is its material network. At the commit, the host
compiles each material that changed once for each kind of device the render
uses: MaterialX's GenOsl, then OSL, into a batched shader group for CPU
devices and into PTX for GPUs, with the layout of its parameters. The change
set carries the compiled shader. Two GPUs share one PTX.

Each CPU device reads textures through its own OpenImageIO `TextureSystem`,
whose tile cache is in its node's memory. For GPUs, the host reads, at the
commit, each texture that a compiled material references for the first time:
its mip pyramid, in the file's format and color space. The change set
carries those texels, under the texture's id, and each GPU uploads them into
its own memory. A texture that several materials share is read once. The
conversion of texels to ACEScg happens as they are sampled
(docs/adr/0007-spectral-rendering-from-acescg.md), on either kind of device.

## The GPU module

The change set crosses into the GPU module as a view of plain data: spans
that point into the host copies, ids and flags, with no ownership. CPU
devices receive the same view. `ModuleApi` gains functions to create a
device on a GPU, to destroy it, and to commit a change set to it.
`kModuleAbiVersion` changes whenever a type of the view changes. The module
holds CUDA and OptiX only: neither OpenUSD, MaterialX, OSL nor
OpenImageIO.

## Considered options

- **One host copy that every device reads**, as ADR 0012 had it. A CPU
  device on another NUMA node would read the geometry remotely while
  shading, and a host copy would persist beside each GPU's upload.
- **A persistent host copy beside the devices' copies.** Nothing reads it
  after the commit, and a desktop would hold its geometry twice.
- **One `TextureSystem` shared by every CPU device.** Its tile cache would
  live on one node, and the others would read it remotely in every texture
  lookup.
- **Prims calling the devices from `Sync`.** Every device would have to be
  thread-safe against Hydra's parallel sync, and a GPU would receive many
  small uploads, interleaved with the sync.
- **Ownership shared with reference counts.** The order of Hydra's phases
  already tells when memory may be freed.
- **Handles that each device returns.** Every cross-reference in render data
  would need translating per device, and handles would cross the GPU
  module's ABI in both directions.
- **Hydra's dirty bits, or "the whole prim changed", in the change set.**
  The first would bring OpenUSD to the devices. The second would leave
  every device to diff buffers or to rebuild everything.
- **One array per component** (`x…`, `y…`, `z…`). Neither Embree nor OptiX
  reads it, so every device would convert it.
- **Nested instancing kept as nested structures.** A forest of instanced
  trees would hold fewer instances, but every level slows every trace.
- **Each device compiling materials from their networks.** The GPU module
  would hold MaterialX and OSL, and two GPUs would compile each material
  twice.
- **Owning C++ containers across the GPU module's ABI.** The module could
  keep or free umbra's memory by mistake.
- **A device dropped from the render when its commit fails.** Its ids would
  fall out of step for the rest of a sequence, and the render would ignore
  the devices `--devices` named.

## Consequences

- With N CPU devices and some GPUs, host memory holds the geometry N times,
  and each GPU holds it once. On a machine with several sockets, that is
  the price of a hot path with no remote reads.
- With P prototypes, a render on `cpu0` and `gpu0` builds P + 1
  acceleration structures on each. Instances are entries of the top level,
  not structures of their own.
- On a GPU, an instance that moves within the shutter needs an
  `OptixMatrixMotionTransform` between the IAS and its GAS. That adds a
  level to the traversable graph (depth 3) for those instances. Embree
  takes the motion of instances directly.
- Each CPU device's texture cache holds the tiles it reads, so a tile that
  several nodes sample is in memory once per node, and its file is read
  once per node.
- GPUs load whole textures, with no streaming of tiles.
- Splitting work between devices cannot rely on per-sample synchronization
  across them, such as a shared atomic counter. Devices take work in large
  blocks, outside the hot path.
- Every commit copies what changed once per device, whatever its kind.
