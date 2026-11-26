# Every light lights everything: no light or shadow linking

USD lets a light restrict what it affects with two collections of
`UsdLuxLightAPI`: `lightLink`, the prims it illuminates, and `shadowLink`,
the prims that cast its shadows. Both are artistic controls with no physical
counterpart: in the physical world no light skips a surface, and no surface
lets one light through while blocking another.

umbra aims to render as physically as it can, so it does not honor them:
every light illuminates every instance, and every instance casts shadows from
every light. The Scene holds no linking: instances carry no light or shadow
mask, and lights no category.

A stage that authors linking is still rendered, and the difference is not
silent: at the end of `Syncing scene`, one warning reports the lights whose
`lightLink` or `shadowLink` is not the default collection of every prim,
with their count and the first. `hdsi`'s light linking scene index stays in
the Hydra chain to tell those collections apart: it gives the default
collection no category, and any other one a category of its own.

## Consequences

- The integrator tests no light mask, and shadow rays filter no hit: the
  BVHs, Embree's and OptiX's, need no per-instance mask for linking.
- A stage lit with linking for another renderer (Karma, RenderMan, Arnold)
  renders differently in umbra: lights reach what the artist excluded, and
  shadows fall where the artist removed them.
