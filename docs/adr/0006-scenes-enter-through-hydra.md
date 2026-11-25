# Scenes enter through Hydra, which only syncs them into umbra's Scene

umbra renders USD scenes. A stage reaches umbra through Hydra 2.0: a
`UsdImagingStageSceneIndex`, followed by the `usdImaging` and `hdsi` filter
chain (purposes, visibility, light linking, render settings), feeds an
`HdRenderIndex` whose render delegate is umbra's. Hydra resolves what USD
composes (instancing, materials, lights, the render settings) into prims a
renderer consumes, and the filters are OpenUSD's own: umbra does not
reimplement them.

Hydra stops at the Scene. The render delegate's prims sync their data into
umbra's Scene, the description of what is rendered; one task of umbra's runs
in `HdEngine::Execute` and only syncs, and the delegate's `CommitResources`
closes the Scene. The integrator reads the Scene and never sees Hydra:
there is no `HdRenderPass` and there are no `HdRenderBuffer`s.

umbra creates its render delegate directly, as an object, rather than as an
`HdRendererPlugin` found through a `plugInfo.json`. The delegate is not meant
for other hosts: one built against another OpenUSD and another C++ runtime
could not load it anyway. umbra does not use `hdx`, whose tasks draw and
present on a GPU: OpenUSD builds it only with GL support
(`pxr/imaging/hdx/CMakeLists.txt`), which the deps layer leaves off.

The delegate lives in the OpenUSD bridge, `src/ext/usd`, behind
`umbra::ext::usd::LoadScene`: as for every dependency, one bridge holds
OpenUSD's types, exceptions and RTTI. The Scene lives in `src/scene` and
holds no `pxr` type.

The Scene is immutable once loaded: umbra renders one frame per process, and
the scene does not change during a render. Its units and conventions are
umbra's, whatever the stage's:

- lengths are in meters: the stage's `metersPerUnit` is applied;
- it is not rotated for `upAxis`: only the dome light's orientation uses it;
- triangles are wound consistently: `leftHanded` geometry is flipped.

Asset paths (textures, shaders, volumes) are resolved by Ar inside the
bridge, where the layer each one comes from anchors it; the Scene holds
absolute file paths only.

## Consequences

- The integrator, the BVHs and the GPU module depend on the Scene alone, so
  none of them includes an OpenUSD header, and the Scene can be built in
  tests without a stage.
- Hydra's change tracking is used once, to load: a scene that changes
  during a render, as in an interactive viewer, is not supported.
- A filter umbra needs that OpenUSD lacks is a scene index of umbra's,
  inserted into the chain in the bridge.
- umbra cannot be used as a Hydra renderer in other hosts (usdview, a DCC).
