# Subdivision surfaces are diced in the sync, by their size on screen

A mesh whose `subdivisionScheme` is `catmullClark`, `loop` or `bilinear`
stands for its limit surface. umbra renders that surface as triangles,
which it refines from the cage on the host, in the mesh's Hydra sync. The
host copy holds those triangles. Devices receive them as they receive any
mesh's, so Embree and OptiX need no new primitive, and the render pass
never evaluates a patch
(docs/adr/0013-devices-render-from-their-own-memory.md).

## Refinement

OpenSubdiv builds a topology refiner from the cage. The cage's
`subdivisionScheme`, `interpolateBoundary`,
`faceVaryingLinearInterpolation`, `triangleSubdivisionRule`, creases,
corners and holes map onto its options and its topology. Its `Bfr`
surfaces are then evaluated and tessellated face by face:

- Each edge of the cage is cut into as many segments as its length on
  screen needs. Faces that share an edge share its points, so the
  surface is watertight.
- A face's points, normals and primvars are evaluated on its limit
  surface: vertex primvars and normals through the same stencils as the
  points, varying primvars linearly, and face-varying primvars through
  their own channel.
- A face whose triangles still have an edge longer than the rate is cut
  further, for a few rounds at most.

Normals and face-varying primvars are given per triangle corner, so that
a crease shades sharp. Each triangle keeps its source face, from which
Ptex can map a hit once umbra reads Ptex textures.

## The dicing rate

The dicing rate is `umbra:dicingRate` on the render settings, in pixels,
1 by default, and nothing overrides it per mesh. An edge's length on
screen is its length, in world space, over its distance from the eye, no
closer than the near plane, scaled by the pixels per radian of the
largest image that sees through the settings' camera. For an orthographic
camera, it is the length scaled by the pixels per meter. Foreshortening is
ignored, so that a surface seen at a grazing angle, or in a reflection, is
diced as finely as one that faces the camera.

A mesh is diced once for all of its instances. Its length on screen is
measured on its instance closest to the camera over the shutter: of every
motion sample of every instance's transform and every position of the
camera, the pair whose distance from the eye to the instance's bounds is
the least. No instance then shows edges longer than the rate, and the
farther ones are finer than they need.

## When a mesh is refined again

A subdivided mesh is refined again when its topology, subdivision tags,
points, primvars, sidedness or placement change, and when its view does:
the settings' camera, its samples over the shutter, the images that see
through it, or the dicing rate. Each refinement commits the mesh with
`kTopology`, `kPrimvars` and `kMaterials`, and `kPlacement` when its
placement changed too.

The render task chooses the view in its sync, which Hydra runs after the
cameras' and before the rprims'. When the view differs from the last
frame's, the delegate refines every subdivided mesh itself, in parallel,
unless Hydra is about to sync it for a change that refines it anyway.
Hydra cannot be asked to sync them: under scene index emulation, its
change tracker takes no invalidation while it syncs. With nothing moving,
no mesh is refined again and nothing is committed.

## Considered options

- **Refining on the devices**, as patches that Embree and OptiX
  tessellate or intersect. Each device would need its own path for
  subdivision surfaces, and the GPU's would evaluate them in its hot path.
- **Refining once for the whole frame range**, at the camera's closest
  approach. A camera that dollies in would dice the first frames far
  finer than they need.
- **Refining at the first frame only.** A camera that dollies in would
  show the facets of the cage's refinement at the last frames.
- **Refining uniformly, to a fixed level.** The level that suits a close
  mesh wastes memory on a distant one, and the reverse shows facets.
- **Measuring each instance apart**, and dicing a prototype per instance.
  Instances would no longer share their geometry
  (docs/adr/0012-umbra-is-a-hydra-renderer.md).

## Consequences

- A moving camera commits every subdivided mesh with `kTopology` at every
  frame, and devices rebuild their acceleration structures.
- A mesh close to the camera can hold millions of triangles. Each edge is
  cut into 1024 segments at most.
- A mesh far from its closest instance's camera, such as a crowd's last
  instances, is diced finer than it needs.
