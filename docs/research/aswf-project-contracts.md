# ASWF projects and the contracts they impose

Research for issue #13. Question: which Academy Software Foundation (ASWF)
projects are relevant to Umbra, a production spectral ray tracer, what role
each would play, and what interface or contract each imposes on the host
renderer. Licence and maturity noted. Contract/role level only; no
implementation advice.

Sources were read on 2026-11-21. Versions are the latest GitHub releases on
that date.

## Summary

| Project | ASWF stage | Licence | Latest release | Role in Umbra | Verdict |
|---|---|---|---|---|---|
| Open Shading Language (OSL) | Graduated | BSD-3-Clause | v1.15.8.0 | Pattern evaluation; emits closure trees | **In** (core) |
| MaterialX (+ OpenPBR) | Graduated (OpenPBR is a MaterialX subproject) | Apache-2.0 | MaterialX v1.39.5, OpenPBR v1.1.2 | Material description; compiled to OSL | **In** (core) |
| OpenImageIO (OIIO) | Graduated | Apache-2.0 | v3.2.2.0 | Texture sampling, image cache, image I/O | **In** (core; hard dependency of OSL) |
| OpenColorIO (OCIO) | Graduated | BSD-3-Clause | v2.6.0 | Colour-space conversion at input and output edges | **In** (core) |
| OpenEXR (+ Imath) | Graduated | BSD-3-Clause | OpenEXR v3.5.4, Imath v3.2.3 | Output format (multi-part, deep), math types | **In** (core) |
| OpenVDB (+ NanoVDB) | Graduated | Apache-2.0 (relicensed from MPL-2.0) | v14.0.0 | Sparse volume data | **In** (geometry) |
| OpenQMC | Sandbox | Apache-2.0 | v0.7.1 | Sample generation (QMC sequences, domains) | **In** (candidate; pre-1.0) |
| Open Path Guiding Library (OpenPGL) | Sandbox | Apache-2.0 | v0.7.1 | Path guiding field and distributions | **In** (candidate; pre-1.0) |
| OpenCue | Graduated | Apache-2.0 | v1.37.0 | Farm queue; drives Umbra from outside | **Integration only** |
| ACES | Incubating | (specification) | n/a | Colour encoding referenced by OCIO configs | **Indirect** (via OCIO config) |
| Digital Production Example Library (DPEL) | Graduated | — | n/a | Test scenes for validation | **Indirect** (test assets) |
| OpenUSD / Hydra | USD Working Group (USD itself is Pixar's, not an ASWF-hosted project) | — | — | Scene entry | Dependency note only; see its own ticket |
| OpenFX | Incubating | BSD-3-Clause | 1.5.1 | Compositing-host plugin API | **Out** |
| OpenTimelineIO, OpenAssetIO, Rez, rawtoaces, OpenAPV, Open RV, xStudio, Dailies Notes Assistant, RMTC | various | — | — | Editorial, asset, packaging, review, camera raw | **Out** |
| MoonRay | Sandbox | — | — | A complete renderer, i.e. a peer, not a building block | **Out** |

Stages come from the TAC project registry (`_data/projects.csv` in
[AcademySoftwareFoundation/tac](https://github.com/AcademySoftwareFoundation/tac/blob/main/_data/projects.csv));
stage meanings from the [TAC lifecycle](https://tac.aswf.io/process/lifecycle.html):
Sandbox is early stage, Incubation requires defined governance and an
OpenSSF "passing" badge, Graduated requires broad adoption and a "gold" badge.
Licences and releases come from the GitHub API for each repository
(`license.spdx_id`, `releases/latest`).

Every licence above is permissive (BSD-3-Clause or Apache-2.0).

## Dependency shape

- OSL requires OIIO (≥ 3.0), Imath (≥ 3.1), LLVM 14–23 with Clang libraries,
  and pugixml; OptiX/CUDA optional
  ([OSL INSTALL.md](https://github.com/AcademySoftwareFoundation/OpenShadingLanguage/blob/main/INSTALL.md)).
  OSL "uses OIIO both for its texture mapping functionality" and cannot drop
  OIIO entirely. OSL is a JIT built on LLVM, so the LLVM version pin in
  `third_party/versions.md` must stay inside OSL's tested range.
- OIIO requires Imath, OpenEXR (≥ 3.1) and OpenColorIO (≥ 2.3); TBB optional
  as its thread pool
  ([OIIO INSTALL.md](https://github.com/AcademySoftwareFoundation/OpenImageIO/blob/main/INSTALL.md)).
- MaterialX generates OSL source/networks; it has no runtime dependency on
  OSL, but its output targets OSL's standard closure set.
- Consequence: OSL, OIIO, OCIO, OpenEXR and Imath form one inseparable
  cluster. Choosing OSL selects the other four.

## Per-project contracts

### Open Shading Language

**Role.** Evaluates shader groups (patterns, layered networks) at shading
points and returns a *closure tree*. The map already settles that Umbra owns
the implementation of every closure.

**Contract imposed on the host** (from
[`oslexec.h`](https://github.com/AcademySoftwareFoundation/OpenShadingLanguage/blob/main/src/include/OSL/oslexec.h),
[`rendererservices.h`](https://github.com/AcademySoftwareFoundation/OpenShadingLanguage/blob/main/src/include/OSL/rendererservices.h),
[`oslclosure.h`](https://github.com/AcademySoftwareFoundation/OpenShadingLanguage/blob/main/src/include/OSL/oslclosure.h),
[`genclosure.h`](https://github.com/AcademySoftwareFoundation/OpenShadingLanguage/blob/main/src/include/OSL/genclosure.h),
[`stdosl.h`](https://github.com/AcademySoftwareFoundation/OpenShadingLanguage/blob/main/src/shaders/stdosl.h),
[`accum.h`](https://github.com/AcademySoftwareFoundation/OpenShadingLanguage/blob/main/src/include/OSL/accum.h)):

1. **ShadingSystem ownership.** The host constructs one `ShadingSystem`,
   handing it a `RendererServices`, a `TextureSystem` and an `ErrorHandler`.
   System options are set by name (`raytypes`, `lazylayers`, `greedyjit`,
   JIT target and optimisation flags).
2. **Ray types.** The host declares its ray-type names (`raytypes`
   attribute) and may tell the optimiser which are known on/off
   (`set_raytypes`). Shaders branch on these through `raytype()`, so the set
   of ray types becomes part of Umbra's shading vocabulary.
3. **Shader group construction.** The host builds groups with
   `ShaderGroupBegin` / `Parameter` / `Shader` / `ConnectShaders` /
   `ShaderGroupEnd` (or a text serialisation, which MaterialX emits). After
   optimisation only parameters hinted `interpolated` or `interactive` can be
   changed with `ReParameter`; that is the IPR edit path without
   recompilation. Other edits mean rebuilding and re-JITting the group.
4. **Execution.** The host fills a `ShaderGlobals` (position, normals, uv,
   derivatives, ray type, time, object/shader transforms) and calls
   `execute` per shade point (or `execute_init` / `execute_layer` /
   `execute_cleanup`). Per-thread state (`PerThreadInfo`, `ShadingContext`)
   must not be shared between threads. Batched SIMD execution (widths
   4/8/16) and OptiX execution are optional alternatives with their own
   services (`batched<WidthT>()`).
5. **RendererServices callbacks.** None is pure virtual; the host implements
   what it supports and reports optional features through `supports()`:
   - transforms: `get_matrix` / `get_inverse_matrix` for named spaces and
     object transforms, optionally time-varying (motion blur);
   - attributes and user data: `get_attribute`, `get_array_attribute`,
     `get_userdata` (primvars). `get_attribute` may be called with a null
     shading-globals pointer by the optimiser, speculatively, at compile
     time, so attribute lookups must be answerable without a shading point;
   - textures: `texture`, `texture3d`, `environment`, `get_texture_info`,
     texture handles, UDIM queries; by default delegated to the
     `TextureSystem`;
   - `trace` (shader-initiated rays) and `getmessage` (messages from a
     traced hit);
   - point clouds (`pointcloud_search` / `get` / `write`);
   - diagnostics (`errorfmt`, `warningfmt`, `printfmt`, `filefmt`).
6. **Closure registration.** The host registers every closure it accepts
   with `register_closure(name, id, params, prepare, setup)`, where `params`
   is a `ClosureParam` list (type, byte offset, optional keyword, size)
   describing the parameter block laid out after each closure component.
   Shaders can only emit closures that have been registered.
7. **Closure tree shape.** Output is a tree of `ClosureComponent` leaves
   (integer `id`, RGB weight `w`, parameter block) joined by `ClosureAdd`
   and `ClosureMul` (RGB weight) nodes. The header states the tree "really
   just boils down to a flat list of weighted closure primitive components";
   flattening and interpreting it is the host's job.
8. **Standard closure vocabulary.** `stdosl.h` declares the classic closures
   (`diffuse`, `microfacet`, `reflection`, `refraction`, `transparent`,
   `emission`, `background`, `holdout`, `debug`, …) and the MaterialX set:
   `oren_nayar_diffuse_bsdf`, `burley_diffuse_bsdf`, `dielectric_bsdf`,
   `conductor_bsdf`, `generalized_schlick_bsdf`, `translucent_bsdf`,
   `transparent_bsdf`, `subsurface_bssrdf`, `sheen_bsdf`, `chiang_hair_bsdf`,
   `uniform_edf`, `anisotropic_vdf`, `medium_vdf`, and the vertical-layering
   combinator `layer(top, base)`. Each MaterialX closure takes an optional
   `label` used for AOVs and light path expressions.
9. **Light path expressions.** OSL ships an LPE engine. The host registers
   custom event/scattering labels and LPE rules on a shared, read-only
   `AccumAutomata`; per path it drives an `Accumulator`
   (`begin`, `pushState` / `move` / `accum` / `popState`, `end`) and
   implements the `Aov::write` sink. The host must therefore report each
   path vertex as an (event, scattering, label) triple.

**Spectral implication.** OSL's `color` is a three-component type, closure
weights are RGB, and closure parameters such as `albedo`, `ior` (conductor)
and `extinction` are RGB. A spectral renderer must uplift these at the
closure-to-BSDF boundary, which matches the decision already recorded on the
map. Wavelength-dependent behaviour (dispersion, thin film) is visible to
OSL only through closure parameters, not through the shading globals.

### MaterialX and OpenPBR

**Role.** Look-development interchange. MaterialX documents (typically
carried in USD) describe node graphs; OpenPBR Surface is the reference
surface shading model.

**Contract.**
- MaterialX provides `OslShaderGenerator` (OSL source) and
  `OslNetworkShaderGenerator`, which "generates a command string that OSL can
  use to build a ShaderGroup"
  ([MaterialXGenOsl](https://github.com/AcademySoftwareFoundation/MaterialX/tree/main/source/MaterialXGenOsl)).
  Generation takes an element and a `GenContext`
  ([ShaderGeneration.md](https://github.com/AcademySoftwareFoundation/MaterialX/blob/main/documents/DeveloperGuide/ShaderGeneration.md)).
- The `genosl` implementations of pbrlib nodes call OSL closures by name:
  `oren_nayar_diffuse_bsdf`, `burley_diffuse_bsdf`, `translucent_bsdf`,
  `conductor_bsdf` (with thin-film keyword parameters), `layer`, `mix`, add
  and multiply, plus OSL wrappers for dielectric, generalized Schlick,
  subsurface, sheen and Chiang hair
  ([pbrlib_genosl_impl.mtlx](https://github.com/AcademySoftwareFoundation/MaterialX/blob/main/libraries/pbrlib/genosl/pbrlib_genosl_impl.mtlx)).
  So a host that consumes MaterialX through OSL must register and implement
  exactly the MaterialX closure set of `stdosl.h`, including `layer`
  semantics (vertical layering, not a weighted sum).
- OpenPBR is a specification plus a MaterialX reference graph
  (`reference/open_pbr_surface.mtlx`). Its colour parameters are in an
  assumed model colour space which, "following MaterialX", defaults to
  ACEScg. It explicitly allows spectral renderers to evaluate thin-film
  Fresnel "from first principles … according to the renderer's spectral
  pipeline", and specifies dispersion via Abbe number at Fraunhofer lines
  ([OpenPBR spec](https://academysoftwarefoundation.github.io/OpenPBR/)).
  The contract is therefore on the *meaning* of parameters, not on an API.
- Colour spaces on MaterialX inputs are resolved through a colour-management
  system (OCIO) and a declared working space; texture colour spaces are
  declared per input.

### OpenImageIO

**Role.** Filtered texture lookups for OSL and Umbra, a shared tile cache
bounding texture memory, and image file I/O (including OpenEXR writing).

**Contract** (from
[`texture.h`](https://github.com/AcademySoftwareFoundation/OpenImageIO/blob/main/src/include/OpenImageIO/texture.h)
and
[`imagecache.h`](https://github.com/AcademySoftwareFoundation/OpenImageIO/blob/main/src/include/OpenImageIO/imagecache.h)):
- `TextureSystem::create(shared, imagecache)` / `destroy`. A shared
  instance is process-global; a private one may own or share an
  `ImageCache`. OSL is handed the same `TextureSystem` instance.
- Global policy by named attribute: `max_memory_MB` (default 1024),
  `max_open_files` (default 100), `searchpath`, `autotile`, `automip`,
  `latlong_up`, `flip_t`, `stochastic`, etc.
- Lookups (`texture`, `texture3d`, `environment`) require screen-space
  derivatives (`dsdx`, `dtdx`, `dsdy`, `dtdy`, or `dRdx/dRdy`, `dPdx/y/z`)
  for filtering; zeros are allowed but disable antialiasing. So ray
  differentials (or an equivalent footprint) are part of what the host must
  supply. Per-lookup options (`TextureOpt`) carry wrap modes, MIP mode,
  interpolation, blur/width, fill, `missingcolor`, a random value for
  stochastic filtering, and a colour transform id.
- Per-thread state (`Perthread`) must never be shared between concurrent
  threads. `TextureHandle` lets the host pre-resolve file names.
- UDIM: `is_udim`, `resolve_udim`, `inventory_udim`.
- IPR: `ImageCache::invalidate(filename, force)` and `invalidate_all` drop
  cached tiles so changed files are re-read; tiles already held keep old
  pixels until released.
- Result channels are whatever the file holds (RGB for colour); spectral
  uplifting of texture colour is the host's responsibility.

### OpenColorIO

**Role.** Colour-space conversion: textures and colour parameters into the
rendering space at the input edge; film/AOV encoding and display transforms
at the output edge.

**Contract** (from the
[developer guide](https://opencolorio.readthedocs.io/en/latest/guides/developing/developing.html)
and
[config authoring overview](https://opencolorio.readthedocs.io/en/latest/guides/authoring/overview.html)):
- The host obtains a `Config` (from the `OCIO` environment variable via
  `GetCurrentConfig`, an explicit file, or a built-in config such as
  `studio-config-latest`). The config, not Umbra, is the authority on colour
  spaces.
- Roles are the stable names a host should use: `scene_linear`,
  `reference`, `data`, `color_picking`, `color_timing`, `compositing_log`,
  `texture_paint`, `matte_paint`, `default`, and the interchange roles
  `aces_interchange` (ACES2065-1) and `cie_xyz_d65_interchange` (CIE XYZ,
  D65). There is no standard `rendering` role.
- Conversions go through `getProcessor` (source→destination, or
  display/view/direction) then a CPU processor (`apply`) or GPU processor
  (shader code extraction). Building processors is comparatively expensive
  and can block, so it belongs to render setup; applying a CPU processor is
  usable from multiple threads. Dynamic properties (e.g. exposure) change
  without rebuilding.
- OCIO is RGB-only. For a spectral renderer the bridge is
  `cie_xyz_d65_interchange`: spectra integrate to XYZ, then OCIO maps XYZ to
  any configured space. The reverse (RGB → spectrum) is outside OCIO.

### OpenEXR and Imath

**Role.** Primary output format for the film and AOVs (including deep);
Imath supplies the vector/matrix/half types shared across the cluster.

**Contract** (from the
[technical introduction](https://openexr.com/en/latest/TechnicalIntroduction.html)
and
[standard attributes](https://openexr.com/en/latest/StandardAttributes.html)):
- Multi-part files: each part has its own header and a type (scanline,
  tiled, deep scanline, deep tiled), allowing one file to hold beauty, AOVs
  and deep data.
- Channel naming: dot-separated layers (`light1.specular.R`); `R`,`G`,`B`,`A`
  are premultiplied colour; non-colour data should avoid `R`/`G`/`B` names
  so it is not colour-managed; `Z`/`ZBack` give sample front/back depth
  (volumetric when `ZBack > Z`); deep parts reserve names such as `RA`,
  `GA`, `BA`, `id`.
- Deep data: variable samples per pixel; `deepImageState` declares whether
  samples are sorted and non-overlapping (not verified by the library).
- Colour identification: `colorInteropID` (new in v3.4) supersedes the
  unreliable `chromaticities`; `whiteLuminance` relates RGB to absolute XYZ.
  Camera metadata: `worldToCamera`, `worldToNDC`, `multiView` for stereo.
- Pixel types `HALF`, `FLOAT`, `UINT` (ids); lossless and lossy
  compressors, several lossy ones unsupported for deep.

### OpenVDB and NanoVDB

**Role.** Sparse volume grids (density, temperature, velocity) referenced
from USD volume prims.

**Contract.** OpenVDB is a hierarchical sparse grid library and `.vdb` file
format ([README](https://github.com/AcademySoftwareFoundation/openvdb));
NanoVDB is a "pointer-less" linearised form occupying "a contiguous block of
memory", with fixed topology but mutable values and extra per-node metadata
(min/max, bounding boxes) "to speed up ray tracing"; it runs on CPU and GPU
([NanoVDB FAQ](https://www.openvdb.org/documentation/doxygen/NanoVDB_FAQ.html)).
The contract on Umbra is the grid data model (named typed grids, index-to-
world transform, sparse tree, value/background semantics); the choice of
OpenVDB vs NanoVDB for traversal is an implementation matter. Grid values are
physical quantities, not RGB, so they need no uplifting; any colour grids
would.

### OpenQMC

**Role.** Deterministic low-discrepancy sample generation for every
stochastic decision (pixel, lens, time, wavelength, light, BSDF).

**Contract** ([README](https://github.com/AcademySoftwareFoundation/openqmc)):
- Sampler families: PMJ, Sobol, lattice, each with a blue-noise variant
  (`PmjSampler`, `PmjBnSampler`, `SobolSampler`, …). Samplers are tiny
  (8–16 bytes), trivially copyable, constructed from pixel coordinates, a
  further index/seed argument and a sample index.
- Domain tree: `newDomain(key)` derives dependent or independent domains;
  `newDomainSplit`, `newDomainDistrib`, `newDomainChain` cover fixed-rate
  splitting and non-constant rates; `drawSample<N>` returns up to four
  dimensions per domain. Correct use of the tree is what the README ties to
  "bias-free results", so the integrator's structure has to be expressed as
  a domain tree with stable keys.
- PMJ samplers need a shared table initialised once (`initialiseCache`);
  Sobol and lattice do not.
- C++17, header-only, no dependencies; CPU SIMD and GPU targets.
- Relevance to distribution: since samples are a pure function of
  (pixel, index, domain key path), workers on different nodes can compute
  disjoint sample-index subsets of the same pixel deterministically.
- Maturity: Sandbox, v0.7.1 (2025-08), pre-1.0.

### Open Path Guiding Library (OpenPGL)

**Role.** Learned spatio-directional guiding distributions for surface and
volume sampling.

**Contract** ([README](https://github.com/RenderKit/openpgl)): a `Device`
(CPU SIMD backends), a `Field` holding the learned approximation, a
thread-safe `SampleStorage` that collects radiance samples, and
`SurfaceSamplingDistribution` / `VolumeSamplingDistribution` queried per
vertex for sampling and PDF evaluation (optionally product with a BSDF
cosine or a Henyey–Greenstein lobe). The host must emit a sample per path
vertex with radiance back-propagated from the path end, and update the field
between progressions. Its radiance type is RGB (`pgl_spectrum`), so a
spectral renderer must reduce per-wavelength contributions to that form.
The field is learned from samples, so in a multi-node frame the training
data or the field must be shared for workers to converge on the same
guiding. Maturity: Sandbox; pre-1.0 and the README warns the API may change.
The repository lives under RenderKit (Intel), not the ASWF GitHub
organisation.

### OpenCue

**Role.** Farm queue that schedules render jobs; per the map, it integrates
from outside.

**Contract** ([glossary](https://docs.opencue.io/docs/concepts/glossary/)):
a job contains layers; each layer has a frame range and a command; each frame
is one command run by RQD on a host proc, with results reported to Cuebot.
The only demand on Umbra is to be a well-behaved batch command: driven by
arguments (scene, frame), reporting success or failure through its process
outcome, and fitting in a proc's resource reservation. No library linkage.

## Not adopted

- **OpenFX**: image-effect plugin API for compositing hosts; Umbra is not a
  compositing host.
- **OpenTimelineIO, Rez, rawtoaces, OpenAPV, Open RV, xStudio, Dailies
  Notes Assistant, RMTC**: editorial, packaging, camera raw, codec and review
  tools outside a renderer's boundary.
- **OpenAssetIO**: asset-reference resolution; if needed it would sit behind
  USD asset resolution, so it belongs to the USD ticket.
- **ACES**: not a library dependency; reaches Umbra through OCIO configs
  (`aces_interchange`, ACEScg as the MaterialX/OpenPBR default space).
- **DPEL**: useful as validation scenes, not as a runtime component.
- **MoonRay**: a complete renderer; not a component Umbra would consume.
- **OpenUSD / Hydra**: Umbra's only scene entry (settled on the map). The
  ASWF hosts a USD Working Group, not the USD project itself. USD's
  MaterialX support (`UsdShade` + `usdMtlx`) is what delivers MaterialX
  documents to Umbra, and USD links OpenVDB/OIIO/OCIO/MaterialX itself, so
  versions must be shared across the stack (the VFX Reference Platform
  coordinates these). Covered by its own ticket.

## Cross-cutting observations for the contract catalog

1. **The RGB boundary.** OSL closures, MaterialX parameters, OIIO texels,
   OCIO and OpenPGL are all RGB. OpenPBR permits spectral evaluation of
   thin film and dispersion. Every ASWF interface Umbra consumes therefore
   enters as RGB in a declared colour space and leaves as XYZ/RGB through
   OCIO; the spectral domain is entirely internal to Umbra.
2. **Callbacks into Umbra.** OSL is the only project that calls back into
   the renderer (`RendererServices`). Through it, the Scene (transforms,
   attributes, primvars), Texturing, and Ray Tracing (`trace`) building
   blocks are exposed to shaders, and the optimiser may query attributes
   without a shading point.
3. **Shared singletons.** OIIO's `TextureSystem`/`ImageCache` and OSL's
   `ShadingSystem` are process-wide and thread-safe with per-thread state.
   In the distributed model each worker node owns its own instances; none of
   these libraries is distributed itself.
4. **IPR hooks.** OSL `ReParameter` (only for parameters hinted
   interactive/interpolated) and OIIO `invalidate` are the upstream
   mechanisms for incremental edits; anything else means rebuilding the
   shader group.
5. **Vocabulary inherited.** Ray-type names, closure names/labels, LPE event
   and scattering labels, OCIO role names, and EXR channel/layer naming
   conventions all become part of Umbra's public vocabulary.
6. **Toolchain coupling.** OSL JIT pins an LLVM range (14–23 at v1.15.8);
   the OSL/OIIO/OCIO/OpenEXR/Imath cluster must be versioned together.
