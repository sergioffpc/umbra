# Hydra 2.0 render delegate contract

Research note for issue #14. It answers four questions:

- What must a Hydra 2.0 renderer implement?
- How do the production delegates for Arnold and RenderMan map that contract onto their renderers?
- Which parts of the contract set the boundary of Umbra's **Scene Ingest** and **Film/output** building blocks?
- How does a batch CLI drive the same path?

This note covers the contract only. It is not a design for Umbra's internals.

## Sources and versions

All claims trace to these primary sources, read on 2026-11-21:

| Ref | Source |
|---|---|
| [USD-hd] | OpenUSD `release` branch, version 0.26.8 (`cmake/defaults/Version.cmake`), headers under `pxr/imaging/hd/`: `renderDelegate.h`, `renderer.h`, `rendererPlugin.h`, `legacyRenderControlInterface.h`, `sceneIndex.h`, `sceneIndexObserver.h`, `sceneIndexPluginRegistry.h`, `sceneIndexAdapterSceneDelegate.h`, `dirtyBitsTranslator.h`, `changeTracker.h`, `renderPass.h`, `renderBuffer.h`, `aov.h`, `renderSettings.h`, `renderSettingsSchema.h`, `renderProductSchema.h`, `renderVarSchema.h`, `sceneGlobalsSchema.h`, `rendererCreateArgsSchema.h`, `renderCapabilitiesSchema.h`, `instancer.h`, `instancerTopologySchema.h`, `material.h`, `materialSchema.h` |
| [USD-guide] | OpenUSD docs, *Hydra 2.0 Getting Started Guide* (`openusd.org/release/api/_page__hydra__getting__started__guide.html`) |
| [USD-render] | OpenUSD docs, *UsdRender* overview (`openusd.org/release/api/usd_render_page_front.html`) and `UsdRenderVar` class reference |
| [usdrecord] | OpenUSD `pxr/usdImaging/usdAppUtils/frameRecorder.{h,cpp}` (the engine behind `usdrecord`) and `pxr/usdImaging/usdImaging/sceneIndices.h` |
| [HdPrman] | OpenUSD `third_party/renderman/plugin/hdPrman/`: `renderDelegate.{h,cpp}`, `renderParam.h`, `renderPass.h`, `renderSettings.h`, `framebuffer.h`, `rileyConversionSceneIndex.h`, `rileyPrimFactory.h`, `rileyPrimBase.h`, `plugInfo.json` |
| [HdArnold] | Autodesk `arnold-usd`, `master` branch, `libs/render_delegate/`: `render_delegate.{h,cpp}`, `render_pass.{h,cpp}`, `render_buffer.h`, `render_param.h` |
| [husk] | SideFX Houdini documentation, *husk* (`sidefx.com/docs/houdini/ref/utils/husk.html`) |

## Short answer

1. **The contract has two halves.**
   - The **scene half** is pure Hydra 2.0. A renderer observes a *terminal scene index* and receives four kinds of notice: `PrimsAdded`, `PrimsRemoved`, `PrimsDirtied` (with hierarchical data-source locators) and `PrimsRenamed`. It pulls typed data through Hydra schemas.
   - The **control half** (render passes, render buffers/AOVs, settings, convergence, pause/stop, commands) is still the Hydra 1.0 `HdRenderDelegate` surface. Hydra 2.0 only re-exposes it through `HdLegacyRenderControlInterface`.
   - Upstream states that "there is no pure Hydra 2.0 `HdRenderer` implementation yet". Its own guide still calls `HdRenderer` a stub. [USD-hd `renderer.h`; USD-guide]
2. **A practical Hydra 2.0 delegate today is a hybrid.**
   - It implements `HdRenderDelegate` and overrides the 2.0 hooks: `SetTerminalSceneIndex` registers an observer, and `Update` drains notices before `SyncAll`.
   - For anything it does not yet ingest natively, it falls back to emulated prim `Sync` with dirty bits.
   - Renderer-specific *scene index plugins* do the scene-to-renderer conditioning (motion blur, material filtering, mesh lights, procedurals) before the data reaches the delegate.
   - HdPrman follows this shape. HdArnold is still a pure 1.0 delegate that runs on upstream emulation. [HdPrman; HdArnold]
3. **Umbra's boundaries follow from this.**
   - **Scene Ingest** is everything from the terminal-scene-index observer inward. Its upstream edge is Umbra's registered scene-index plugins plus the renderer's declared resolution preferences.
   - **Film/output** is defined by the `renderSettings` / `renderProduct` / `renderVar` schemas and by the `HdRenderBuffer` + `HdRenderPassAovBinding` + `IsConverged` contract. Non-raster outputs go through *delegate render products*.
4. **A batch CLI uses the same path as the interactive one.**
   - It builds the USD stage scene index chain, creates the renderer, selects a render settings prim, and calls render in a loop until `IsConverged`. Then it writes the products.
   - Across frames it pushes time changes as scene deltas, not rebuilds. `usdrecord` does exactly this loop. `husk` adds settings partitioning per camera, snapshots, and delegate products. [usdrecord; husk]

## 1. Lifecycle and discovery

- **Discovery.** A renderer is a `HdRendererPlugin`, found at runtime through the Plug system (`plugInfo.json`). Hydra instantiates it once per library. [USD-hd `rendererPlugin.h`]
- **Hydra 2.0 creation.** The plugin exposes three calls:
  - `IsSupported(rendererCreateArgs, reasonWhyNot)`: a runtime capability check. The create args carry `gpuEnabled` and `drivers`/`hgi`.
  - `GetSceneIndexCreateArgs()`: arguments the *application* passes to its scene indices so they can be configured for this renderer. Examples from the header: whether motion-blur samples must be pre-computed, and the material render-context preference order.
  - `CreateRenderer(sceneIndex, rendererCreateArgs)`: if the plugin does not override `_CreateRenderer`, this falls back to wrapping a 1.0 `HdRenderDelegate` in "back-end" emulation (`HdRenderDelegateAdapterRenderer`).

  [USD-hd `rendererPlugin.h`, `rendererCreateArgsSchema.h`; USD-guide]
- **Hydra 1.0 creation (still the live path).** `CreateRenderDelegate(settingsMap)` takes an initial `HdRenderSettingsMap`. `DeleteRenderDelegate` releases it. [USD-hd `rendererPlugin.h`]
- **Capabilities.** `GetCapabilities()` returns a data source that conforms to `HdRenderCapabilitiesSchema`. Today the schema holds only `motionBlur`. HdPrman sets `motionBlur = true`. [USD-hd `renderCapabilitiesSchema.h`; HdPrman `renderDelegate.cpp`]

## 2. Scene half: consuming the scene index

### 2.1 The graph a renderer sees

A Hydra 2.0 application builds the following chain [USD-guide, "A Hydra 2.0 Application"]:

```
UsdImagingStageSceneIndex → USD filters (instancing, draw mode, …)
  → HdMergingSceneIndex                     (scenegraph / renderer demarcation)
  → app-specific + renderer-specific plugin scene indices (ordered by phase)
  → [optional caching scene index]
  → terminal scene index → renderer (observer)
```

- Renderer-specific filters are `HdSceneIndexPlugin` subclasses. Each is registered with `RegisterSceneIndexForRenderer(rendererDisplayName, pluginId, inputArgs, insertionPhase, insertionOrder)` and loaded via the `loadWithRenderer` key in `plugInfo.json`. Ordering can also come from JSON `tags`/`ordering`. [USD-hd `sceneIndexPluginRegistry.h`; HdPrman `plugInfo.json`]
- HdPrman registers about 20 such plugins. They cover world offset, motion blur, velocity motion resolving, material filtering (`matfilt*`), MaterialX, mesh-light and portal-light resolving, render-settings filtering, render-pass prune/visibility, tet-mesh conversion, pinned-curve expansion, implicit surfaces, and the dependency scene indices. [HdPrman file list, `plugInfo.json`]

**Consequence:** much renderer-specific scene conditioning happens *upstream of the delegate*, as scene-index filters. The delegate does not do it.

### 2.2 Observer notices: the change-tracking contract

[USD-hd `sceneIndex.h`, `sceneIndexObserver.h`; USD-guide]

- **`PrimsAdded(path, type)`.** The prim may already exist. In that case treat the notice as a resync or a type change.
- **`PrimsRemoved(path)`.** Hierarchical: removing a path removes its whole subtree.
- **`PrimsDirtied(path, locatorSet)`.**
  - Not hierarchical on the path: children are unaffected.
  - Hierarchical on the locator: dirtying `primvars` dirties `primvars/color`.
  - The renderer must drop any cached data-source handles under the locators and pull them again.
- **`PrimsRenamed(old, new)`.** Covers renames and reparenting of a subtree. A helper converts it to remove + add.
- **Initial population is not pushed.** An observer only gets notices sent after it registers. The consumer must traverse the existing scene itself with `GetChildPrimPaths` from the root, then `GetPrim`.
- **Threading.**
  - `GetPrim` and `GetChildPrimPaths` are expected to be thread-safe.
  - Notices may be delivered on a single thread, and observers must not assume re-entrancy.
  - Adding or removing an observer is not thread-safe.
- **Data model.** A prim is `(type, container data source)`. Typed access goes through `HdSchema` wrappers such as mesh topology, primvars, xform, material, instancer topology, render settings and scene globals. These schemas, not C++ structs, are the agreed format between scene and renderer.
- **Dependency forwarding.** Relationship-borne invalidation is not automatic. Example: a mesh must re-tessellate when its bound material changes. A filter declares the link through `HdDependenciesSchema`, and `HdDependencyForwardingSceneIndex` propagates the dirty notice. HdPrman ships dependency plugins for this. [USD-guide; HdPrman]

### 2.3 Three ways to consume, and which is current

[USD-guide; USD-hd `renderDelegate.h`, `sceneIndexAdapterSceneDelegate.h`, `dirtyBitsTranslator.h`]

| Mode | What the renderer implements | Status |
|---|---|---|
| **Emulated 1.0** | `HdRprim`/`HdSprim`/`HdBprim` subclasses with `Sync(sceneDelegate, renderParam, dirtyBits)`. `HdSceneIndexAdapterSceneDelegate` turns notices into dirty bits through `HdDirtyBitsTranslator`. | Supported and widely used (HdArnold). Invalidation is limited to what dirty bits can express, and sync is limited to the "type-sorted-by-tier, thread-per-prim model of `SyncAll`". |
| **Hybrid 2.0** | Same delegate, plus overrides of `SetTerminalSceneIndex` (register an observer; the index may not be fully populated yet) and `Update()` (called at the start of `SyncAll` to process notices directly). Prims may also read `GetTerminalSceneIndex()->GetPrim(id)` inside `Sync`. | Upstream's recommended migration path. HdPrman uses it. |
| **Pure 2.0 `HdRenderer`** | A scene-index observer, typically built on `HdsiPrimManagingSceneIndexObserver::PrimBase`, plus `HdLegacyRenderControlInterface` for control. | API is a stub. Upstream: "no pure Hydra 2.0 HdRenderer implementation yet". |

- **Dirty bits.** The 1.0 dirty-bit vocabulary is still the shared language of emulation:
  - Rprim bits: points, primvar, materialId, topology, transform, visibility, normals, doubleSided, subdivTags, widths, instancer, instanceIndex, volumeField, categories, and others.
  - Material bits: params, resource, surface, displacement, volume.
  - Render-settings bits: products, namespaced settings, camera, frame number, color space, sampling interval, and others.
  - Custom bits are available from `1<<24` to `1<<30`.

  [USD-hd `changeTracker.h`, `material.h`, `renderSettings.h`]
- **Parallel sync.** `IsParallelSyncEnabled(primType)` lets a delegate opt into multi-threaded `Sync` per prim type. HdPrman enables it only for types it has reviewed. [USD-hd; HdPrman `renderDelegate.cpp`]

### 2.4 Prim vocabulary and factories

[USD-hd `renderDelegate.h`]

- **Prim classes.** The delegate declares the prim types it supports: Rprims (geometry), Sprims (state: camera, lights, material, coordSys, …) and Bprims (buffers: renderBuffer, renderSettings, volume fields).
  - It provides factories for each class, plus `CreateFallbackSprim`/`CreateFallbackBprim` for dangling references.
  - It provides `CreateInstancer`/`DestroyInstancer`.
- **`HdRenderParam`.** An opaque, delegate-owned handle passed to every prim `Sync`. It also takes thread-safe `Set/Get/HasArbitraryValue` calls from clients.
- **`CommitResources(tracker)`.** Called after all syncs and before execution, so the delegate can finalize memory (buffers, textures, acceleration structures).
- **Render settings are a Bprim on purpose.** They sync before Sprims and Rprims, so their opinions can steer how those prims sync. [USD-hd `renderSettings.h`]

### 2.5 Instancing

[USD-hd `instancer.h`, `instancerTopologySchema.h`]

- **Data.** An instancer holds `prototypes` (paths, which may be subtrees), `instanceIndices` per prototype, an optional `mask`, and `instanceLocations`. Instance-rate data (primvars such as `hydra:instanceTranslations`, and material bindings) lives on the instancer.
- **Nesting.** Instancers can nest. A prototype draws once for each tuple of indices across the nesting levels.
- **Two origins.**
  - *Explicit* (point instancer): data expansion.
  - *Implicit* (USD native instancing): data coalescing. `instanceLocations` keeps the original paths.
- **1.0 view.** Hydra 1.0 stores the relation in reverse: each Rprim names its instancer.

**For a ray tracer:** the contract hands over prototypes plus per-instance transforms and primvars, not flattened copies. That maps directly to two-level (instance-of-BVH) acceleration.

### 2.6 Material networks

[USD-hd `materialSchema.h`, `material.h`, `renderDelegate.h`; HdArnold; HdPrman]

- **Per-context networks.** A material prim carries one network per *render context*: the universal `""` context, `ri`, `glslfx`, `mtlx`, `arnold`, and so on.
  - The delegate sets the preference order with `GetMaterialRenderContexts()`.
  - It sets which binding to resolve with `GetMaterialBindingPurpose()`: `preview` by default, `full` for production.
  - It declares supported shading systems or source types with `GetShadingSystems()`. `GetShaderSourceTypes()` is deprecated.
- **Network shape.** A network is nodes (path, identifier, parameters) plus relationships (`inputId/inputName → outputId/outputName`), with terminals for surface, displacement and volume.
- **Filtering.** Optional primvar filtering (`IsPrimvarFilteringNeeded`) lets the scene side send only the primvars that bound materials need.
- **Resolution tables in the delegates:**

  | | Render contexts | Binding purpose |
  |---|---|---|
  | HdArnold | `{arnold, mtlx}` | `full` |
  | HdPrman | `{ri, mtlx}` when built with MaterialX | — |

- **MaterialX.** HdPrman converts MaterialX and filters networks with scene-index plugins (`matfiltMaterialX`, `matfiltResolveVstructs`, `matfiltConvertPreviewMaterial`) *before* the delegate sees them.

## 3. Control half: passes, buffers, AOVs, settings

### 3.1 Render pass and progressive rendering

[USD-hd `renderPass.h`, `renderDelegate.h`; usdrecord]

- **Render pass.** `CreateRenderPass(index, collection)` returns an `HdRenderPass`, which has two phases:
  - `Sync` tracks collection changes.
  - `_Execute(renderPassState, renderTags)` renders.

  The render-pass state carries camera, framing and AOV bindings.
- **Progressive contract.** `IsConverged()` defaults to `true`. A progressive renderer returns `false` while it is still adding samples. The application calls `Execute` again and again until it returns `true`.
  - `usdrecord` calls render, sleeps 10 ms (growing by 5 ms up to a 100 ms cap) while the pass is not converged, then writes.
  - HdArnold's pass says it "is continuously executed, until IsConverged returns true".
- **Background render control.** Each capability is advertised, and the defaults are no-ops:
  - `IsPauseSupported`, `Pause`, `Resume`, `IsPaused`.
  - `IsStopSupported`, `Stop(blocking)`, `Restart`, `IsStopped`.
- **Commands.** `GetCommandDescriptors` and `InvokeCommand` provide renderer-specific commands, for example "save checkpoint" or "reload texture".
- **Statistics.** `GetRenderStats()` returns an open dictionary. It is the progress/telemetry channel.

### 3.2 Render buffers and AOV bindings

[USD-hd `renderBuffer.h`, `aov.h`]

- **`HdRenderBuffer` (a Bprim).** It provides:
  - `Allocate(dims, format, multiSampled)`, width/height/depth, and `GetFormat`.
  - `Map`/`Unmap`/`IsMapped`.
  - `Resolve()`: makes reads see the latest writes, which allows deferred accumulation or filtering.
  - `IsConverged()`.
  - Optionally `GetResource()`, which returns a backing GPU texture.
- **`HdRenderPassAovBinding`.**
  - `aovName`: a token from `HdAovTokens`. Prefixes such as `primvars:` and `lpe:` are parsed by `HdParsedAovToken`.
  - `renderBuffer`: a pointer or path.
  - `clearValue`: an empty value means no clear.
  - `aovSettings`: a free dictionary, for example pixel filter options.
- **Buffer lifetime.** Hydra never owns the buffer. It assumes the buffer stays alive "until the end of the renderpass, or whenever the buffer is marked converged, whichever is later".
- **AOV defaults.** `GetDefaultAovDescriptor(name)` gives the delegate's preferred format, multisampling and clear value for each AOV. A default-constructed descriptor carries `HdFormatInvalid`, so an AOV the delegate does not describe comes back with no usable format.

### 3.3 Render settings: three overlapping mechanisms

[USD-hd `renderSettings.h`, `renderDelegate.h`, schemas]

1. **`HdRenderSettingsMap`**: a token→value dictionary given at construction and updated later through `SetRenderSetting`. `GetRenderSettingsVersion` bumps on each change.
2. **`GetRenderSettingDescriptors()`**: settings the delegate exports to UI.
3. **The render settings prim** (`HdRenderSettings` Bprim, `renderSettings` schema). This is the direction upstream says it is moving towards. It carries:
   - `active`, `camera`, `renderProducts`, `includedPurposes`, `materialBindingPurposes`.
   - `renderingColorSpace`.
   - `unionedSamplingInterval`: the frame-relative motion-blur window, defined as the union of the camera shutter intervals.
   - `disableMotionBlur`, `disableDepthOfField`.
   - `namespacedSettings`: filtered by the delegate's `GetRenderSettingsNamespaces()`, for example `ri`.

**Render products and render vars**, flattened as in `UsdRenderSpec` [USD-hd `renderSettings.h`; USD-render]:

- **Product:** `productPath`, `type` (e.g. `raster`), `name`, `resolution`, `renderVars[]`, `cameraPath`, `pixelAspectRatio`, `aspectRatioConformPolicy`, `apertureSize`, `dataWindowNDC`, per-product `disableMotionBlur`/`disableDepthOfField`, and `namespacedSettings`.
- **Render var (AOV):** `varPath`, `dataType`, `sourceName`, `sourceType` (UsdRender schema values: `raw`, `primvar`, `lpe`, `intrinsic`), and `namespacedSettings`.
- **Data window.** `dataWindowNDC` handles crop, tiling and overscan.
  - Coordinates are relative to the policy-adjusted aperture, with (0,0) at the bottom-left.
  - Rasterization rule: a pixel is included if its centre lies inside the window. The minimum edge is inclusive and the maximum edge exclusive, so adjacent windows tile with no gaps or overlaps. [USD-render]
- **Coarse invalidation.** `GetAndResetHasDirtyProducts()` reports any change to the products, because invalidation is not fine-grained.

**Scene globals** are a `sceneGlobals` container on the root prim [USD-hd `sceneGlobalsSchema.h`]:

- `activeRenderSettingsPrim`, `activeRenderPassPrim`, `primaryCameraPrim`.
- `startTimeCode`, `endTimeCode`, `timeCodesPerSecond`.
- `currentFrame`: changing the frame is a scene notice, not a control call.
- `sceneStateId`.

## 4. How the production delegates map the contract

### 4.1 RenderMan (HdPrman)

[HdPrman]

- **API level.** A 1.0 `HdRenderDelegate` with the 2.0 hooks.
  - `SetTerminalSceneIndex` builds a set of "riley scene indices" over the terminal index when compiled with `HDPRMAN_USE_SCENE_INDEX_OBSERVER`.
  - `HdPrman_RileyConversionSceneIndex` converts Hydra prims into `riley:*` prims (geometryPrototype, geometryInstance, material, renderView, renderOutput, display, integrator, globals, …). `HdPrman_RileyPrimFactory` is an `HdsiPrimManagingSceneIndexObserver::PrimFactoryBase`. It turns those prims into `Riley::Create/Modify/Delete*` calls.
  - This path is marked experimental and incomplete: today it converts only spheres, uses the fallback material, and ignores instancers. Production still runs through emulated prim sync.
- **Scene conditioning.** It is done in about 20 renderer-specific scene-index plugins, ordered by phase tags in `plugInfo.json` (see §2.1).
- **Render loop.** `HdPrman_RenderParam` owns a render thread that calls into Riley.
  - Scene edits first stop the render. `HdPrman_RileyPrimBase` "does necessary things (such as stopping the render) so that calls to, e.g., Riley::Create are safe".
  - Pause and stop/restart are supported.
- **Interactive vs batch.** The `enableInteractive` render setting picks the mode (default `true`).
  - Interactive: AOVs come from Hydra `aovBindings`. The renderer writes into an in-process `HdPrmanFramebuffer` through a display driver, and `HdRenderBuffer` reads from it. The framebuffer also remaps id AOVs back to Hydra `primId`/`instanceId`.
  - Batch: displays are created from the render-settings map or prim. `HdPrman_RenderSettings::DriveRenderPass` updates camera, render view and options, then calls `riley->Render`. It is documented as "batch (non-interactive)" only.
- **Settings vocabulary.** `ri:` namespace, plus host-provided keys: `delegateRenderProducts`, `batchCommandLine`, `houdini:frame`, `ri:checkpoint:interval`, `ri:Ri:PixelFilterName`, `shutter:open/close`, `instantaneousShutter`, `dataWindowNDC`, `aspectRatioConformPolicy`, `resolution`.
- **Parallel sync.** Enabled for reviewed prim types only.

### 4.2 Arnold (HdArnold)

[HdArnold]

- **API level.** A pure Hydra 1.0 delegate: it does not override `SetTerminalSceneIndex`/`Update`. Under a 2.0 application it runs inside upstream emulation.
- **Supported prims.**
  - Rprims: mesh, volume, points, basisCurves, a custom procedural, and particleField.
  - Sprims: camera, coordSys, material, light filter, all UsdLux light types, mesh light, plus `ArnoldOptions` and `GeometryLight`.
  - Bprims: renderBuffer, openvdbAsset, renderSettings.
- **Scene mapping.** Each Hydra prim becomes Arnold nodes in a per-delegate `AtUniverse` with an `AtRenderSession`. `HdArnoldRenderParam` drives Arnold's progressive render:
  - `UpdateRender` starts or continues the render.
  - `Interrupt(needsRestart)` runs on every scene or settings edit.
  - `Pause`/`Resume` keep accumulated samples. `Stop`/`Restart` discard them.
  - The status enum is `Converging`, `Converged`, `Aborted`.
- **AOVs.** In `_Execute` the render pass compares `aovBindings`. It creates one Arnold output per binding: a filter, optional primvar writer/reader shaders for `primvars:` AOVs, and a Hydra driver that writes into `HdArnoldRenderBuffer`. Each buffer's converged flag follows the render status.
- **Delegate render products.** When `husk` sets `delegateRenderProducts`, the pass instead creates *native* Arnold drivers (`driver_exr`, `driver_deepexr`, `driver_no_op`, …). Driver parameters are read from `arnold:{driverType}:` namespaced settings.
  - Arnold then writes the files itself, for example deep EXR.
  - The code takes care that the data window it applies matches husk's to the pixel.
- **Render settings source.** `renderSettingsSrc` selects between the Hydra render settings prim and host-provided settings.

### 4.3 What the two have in common

- Both treat the renderer as **owning a private, renderer-native scene**: a Riley scene or an Arnold universe. Hydra prims are translated into it, and edits are applied with *stop → modify → restart*.
- Both use **two output paths**:
  - Hydra render buffers, for viewports and husk raster products.
  - Renderer-native writers ("delegate render products"), for deep, multi-part or checkpoint outputs.
- Neither implements a pure 2.0 `HdRenderer`.

## 5. How a batch CLI drives the same path

**`usdrecord` (OpenUSD)** [usdrecord]

1. Opens the stage.
2. Picks a renderer plugin (empty id gives the default, with GPU allowed or not).
3. Sets the active render settings prim and render pass prim, the camera, the purposes, and `color` as the AOV.
4. Loops `Render` until `IsConverged`.
5. Writes the image only if the render settings products did not already produce files.

The scene indices come from `UsdImagingCreateSceneIndices`. It is the same engine (`UsdImagingGLEngine`) as the interactive viewer.

**`husk` (Houdini)** [husk]

- **Delegate choice.** The `--delegate` option, then the `husk:delegate` render setting, then an environment default. `--delegate-options` passes options to the delegate.
- **Settings selection.** `--settings` (patterns allowed), or the render pass `renderSource`. Each settings prim renders on its own.
- **Partitioning.** Before rendering, husk "partitions the render products". Because "the delegate can only render a single camera at a time", products that use different cameras become separate render tasks.
- **Multi-frame.** It "uses USD delta operations ... The delegate is only told about scene data that changes frame-to-frame". `--restart-delegate n` forces a rebuild every *n* frames.
- **Delegate render products** represent non-raster outputs (deep images, checkpoints, photon maps). `--disable-delegate-products` turns them off.
  - If no raster product exists, husk makes a dummy `husk:null_raster` product, because "some delegates may throw errors without any raster products".
- **Snapshots.** `--snapshot sec` or `SIGUSR1` maps the current buffers and writes a partial image while the render is still converging.
- **Host settings** reach the delegate through the settings map: `houdini:fps`, `houdini:frame`, `batchCommandLine`, `delegateRenderProducts`.

**Pattern:** a batch CLI is a Hydra application with a fixed loop:

```
build scene chain → create renderer → select settings
→ for each (settings × camera partition × frame):
    set frame (scene delta) → render until converged (snapshots optional)
    → map buffers and write raster products  ∥  delegate writes its own products
```

## 6. Implications for Umbra's building blocks

These are suggestions to feed the arc42 map in #11, not decisions.

**Scene Ingest boundary (upstream edge)**

- *Input contract:* the terminal scene index (Hydra schemas plus the add/remove/dirty/rename notices) and the `sceneGlobals` root container.
- Scene Ingest must also handle the initial traversal, because existing prims are not pushed to a new observer.
- *Umbra-owned upstream:* a set of Umbra scene-index plugins, registered for Umbra's renderer display name. This is the natural home for these jobs, done as filters rather than inside the delegate:
  - motion-sample gathering over `unionedSamplingInterval`;
  - MaterialX→OSL network preparation;
  - mesh-light resolution;
  - procedural expansion;
  - dependency forwarding.
- *Renderer declarations* that shape upstream resolution:
  - `GetMaterialRenderContexts`, for example `{mtlx, ""}` or an Umbra context;
  - `GetMaterialBindingPurpose = full`;
  - `GetShadingSystems` (OSL / MaterialX);
  - `GetRenderSettingsNamespaces`, for example `umbra`;
  - `GetCapabilities` (`motionBlur`);
  - `GetSceneIndexCreateArgs`.
- *Output contract:* deltas into Umbra's render-optimised internal scene, with stop→apply→restart semantics, like Riley and Arnold.
- *Instancing* arrives as prototypes plus instance-rate data. Keep it that way for two-level acceleration.
- *Recommended consumption mode:* hybrid 2.0, the HdPrman shape. Observe the terminal index natively and process notices in `Update()`. Keep the 1.0 prim factories only as thin shells where the application still requires them, for example render buffers and render settings Bprims.
  - This keeps the dirty-bit model out of Umbra's core, and it is the direction upstream is taking.
  - Watch for the risk that `HdRenderer` stabilises. The `HdLegacyRenderControlInterface` mirrors the 1.0 control surface method for method, so the control half should port cleanly.

**Film/output boundary**

- *Configuration input* is the render settings prim (products, vars, camera, data window, color space, namespaced settings), not ad-hoc settings-map keys. Keep the settings map only for host keys (`houdini:*`, `delegateRenderProducts`, …).
- *Raster output* is the `HdRenderBuffer` contract: allocate per binding, `Resolve` before reads, `Map` for readback, `IsConverged` per buffer.
  - `Resolve` is the hook where Umbra's spectral accumulation buffer is converted to the requested output format and color space.
  - In the distributed case, the coordinator's merged film is what backs the buffer, so a worker never answers `Map` directly.
- *AOV vocabulary:* RenderVar `sourceType` ∈ {`raw`, `primvar`, `lpe`, `intrinsic`}, and Hydra AOV tokens with `lpe:`/`primvars:` prefixes. This puts LPE support in the Film/output contract.
- *Non-raster outputs* (deep EXR, cryptomatte, checkpoints) go through **delegate render products**: Umbra writes them itself, and Hydra buffers are not involved.
- *Data window* follows the UsdRender rasterization rule (pixel centre, min edge inclusive, max edge exclusive). Distributed tile/region assignment can reuse that rule so that tiles never overlap.

**Control / progress (cross-cutting)**

- `IsConverged`, Pause/Resume, Stop/Restart, `InvokeCommand` and `GetRenderStats` are the whole progress and control surface Hydra offers. Checkpoint, snapshot and denoise triggers fit as commands or render settings.
- *Frame changes* reach the renderer as scene deltas: a `currentFrame` dirty, then time-varying data dirtied. Batch and interactive therefore share one ingest path. husk's `--restart-delegate` is only an escape hatch.

**Batch CLI**

- An Umbra CLI should be a thin Hydra application, `usdrecord`-shaped, that adds husk's behaviours:
  - partition products per camera;
  - loop frames as deltas;
  - snapshot by mapping buffers;
  - honour delegate products.
- It needs no private scene path. The same renderer plugin also runs inside husk itself.

## 7. Open points

- **`HdRenderer` API.** Its final shape (render settings "in band" as prims, capabilities feeding the scene indices) is announced but not published. Re-check this note against future OpenUSD releases before freezing the Scene Ingest contract.
- **Distributed rendering.** Hydra's contract assumes one process holds the render buffers. How workers get the scene is outside Hydra and belongs to Umbra's distribution design:
  - each worker runs its own scene-index chain from the same stage, or
  - the coordinator streams Scene Ingest deltas.
- **Spectral AOVs.** `HdFormat` is RGB(A)-oriented. Whether spectral or multi-channel AOVs can travel through `HdRenderBuffer`, or only through delegate render products, still needs to be checked against the `HdFormat` enumeration.
