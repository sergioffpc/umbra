# Feature sets of Manuka, Arnold and RenderMan

Research for issue #12. It feeds the quality goals and requirements of Umbra's
arc42 §1 (map: #11).

**Question.** What feature set do Wētā FX Manuka, Autodesk Arnold and Pixar
RenderMan (RIS and XPU) expose, according to public sources? The answer is a
feature matrix grouped by area.

**Date of survey:** 2026-11-21. Arnold is surveyed up to core 7.5.4.0 and
RenderMan up to 27 (with 27.3 daily-build notes). Manuka is surveyed up to the
public REAC 2023 talk.

## Sources

Each claim in the matrix cites one of these keys. All are first-party: papers and
talks by each renderer's own engineers, or the vendor's official documentation.

| Key | Source |
|---|---|
| **M18** | Fascione et al., *Manuka: A Batch-Shading Architecture for Spectral Path Tracing in Movie Production*, ACM TOG 37(3), 2018. <https://doi.org/10.1145/3182161> (author copy: <https://jo.dreggn.org/home/2018_manuka.pdf>) |
| **M23** | van der Steen & Cannell (Wētā FX), *Manuka and Gazebo rendering architectures*, Rendering Engine Architecture Conference 2023. <https://enginearchitecture.org/downloads/reac2023_manuka_web_version.pdf> |
| **PL20** | Langlands & Fascione (Wētā Digital), *PhysLight: An End-to-End Pipeline for Scene-Referred Lighting*, SIGGRAPH 2020 Talks. <https://history.siggraph.org/wp-content/uploads/2022/08/2020-Talks-Langlands_PhysLight-An-End-to-End-Pipeline-for-Scene-Referred-Lighting.pdf> |
| **A18** | Georgiev et al., *Arnold: A Brute-Force Production Path Tracer*, ACM TOG 37(3), 2018. <https://doi.org/10.1145/3182160> |
| **AUG** | Arnold User Guide (Autodesk Help): What's New, release notes 7.x, Lights, Cameras, Light Path Expression AOVs, Cryptomatte, Denoising, Imagers, Caustics pages. <https://help.autodesk.com/view/ARNOL/ENU/> |
| **R18** | Christensen et al., *RenderMan: An Advanced Path-Tracing Architecture for Movie Rendering*, ACM TOG 37(3), 2018. <https://doi.org/10.1145/3182162> (author copy: <https://research.pixar.com/docs/2018.SiggraphPapers.CFSWSKFKRBRBL.pdf>) |
| **RTS** | RenderMan technical specifications. <https://renderman.pixar.com/tech-specs> |
| **R27** | *Pixar Animation Studios releases RenderMan 27*. <https://renderman.pixar.com/news/pixar-animation-studios-releases-renderman-27> |
| **RDOC** | RenderMan 26/27 documentation (rmanwiki): Geometry, Implicit Surfaces, NURBS, Projection Plugins, Lens Distortion, Tilt-Shift, Light Filters, Color management, RenderMan 27.3 notes. <https://rmanwiki-27.pixar.com/space/REN27> |

### Caveat on Manuka

Manuka is an in-house renderer with no public manual. Its public record is the
2018 TOG paper, a few SIGGRAPH talks and courses, and the REAC 2023 talk. An entry
of "n/d" (not documented) for Manuka means only that it is absent from public
sources. It does **not** mean the feature is missing. Arnold and RenderMan have
full public manuals, so an "n/d" there is stronger evidence of absence.

## Legend

- **Yes**: documented as supported.
- **Partial**: supported with a documented restriction (named in the cell).
- **No**: documented as unsupported, or documented as deliberately excluded.
- **n/d**: not found in the sources above.

RenderMan cells refer to both RIS and XPU unless the cell names one. RenderMan 27
makes XPU the final-frame architecture and puts RIS into maintenance [R27].

## Feature matrix

### Geometry

| Feature | Manuka | Arnold | RenderMan |
|---|---|---|---|
| Polygon meshes | Yes [M18 §3] | Yes; quads stored natively to save memory [A18 §3.1, §3.5.2] | Yes [R18 §5; RDOC Geometry] |
| Subdivision surfaces | Yes, by Reyes-style split-and-dice [M18 §3] | Yes: Catmull-Clark and linear, adaptive, OpenSubdiv-compatible, creases [A18 §3.1.1] | Yes: Catmull-Clark and Loop, with sharp and semi-sharp creases [R18 §5]; OpenSubdiv [RTS] |
| Displacement | Yes: micropolygon displacement with a crack-welding pass [M18 §3.4] | Yes, plus autobump [A18 §3.1.2] | Yes: watertight displacement, editable interactively [RTS] |
| NURBS / bicubic patches | Yes: NURBS and Bézier patches [M18 §3.4] | n/d | Partial: RIS only, trimmed NURBS; XPU "does not and will not" [RDOC NURBS] |
| Curves (hair, fur) | Yes: diced into line segments; hair level of detail [M18 §3, §3.4] | Yes: Bézier, Catmull-Rom and others; minimum pixel width [A18 §3.2] | Yes: flat ribbons and round curves, curvelet BVH [R18 §6.3.2] |
| Points / particles | Yes: points diced to spheres and discs [M18 §3.4] | Yes [A18 §3.2]; OpenVDB points loading (7.3.7) [AUG] | Yes (`points:scale` primvar, 27.3) [RDOC 27.3] |
| Implicit surfaces | Yes [M18 §3] | Yes: implicit surface solver [A18 §3.3] | Partial: RiBlobby, RIS only; XPU unsupported [RDOC Implicit Surfaces] |
| 3D Gaussian splats | n/d | Yes: from PLY files, relightable (7.5.2) [AUG] | n/d |
| Instancing | Yes: billions of instances, arbitrarily nested; procedurals emit and can be instanced; optional de-instancing on ingest [M23] | Yes: multi-level, with per-instance UV and colour overrides [A18 §3.5.3] | Yes: with per-instance material [R18 §6.3.1] |
| Procedurals (render-time amplification) | Yes: RI procedural plugins and tessellation plugins for level-of-detail procedurals [M18 §3]; hair, trees and cloth amplified at ingest [M23] | Yes: procedural nodes, nesting, `.ass` files referenced as procedurals [A18 §3.5.4] | Yes: render procedurals and RIB archives [RDOC Geometry in Katana] |
| Transformation motion blur | Yes: transformation blur [M18 §3.4] | Yes [A18 §4.4] | Yes [R18 §6.5] |
| Deformation motion blur | Yes: motion BVH [M18 §3.4] | Yes [A18 §4.4.1] | Yes [R18 §6.5] |
| Tessellation strategy | Everything is pre-tessellated into micropolygons, view-adaptive by shading rate, before transport. An optional oracle pass culls occluded geometry [M18 §3.4, §5] | Each node is processed up front: subdivision, displacement, then a per-node BVH [A18 §2.2, §3.1] | On demand when rays reach a patch, with multi-resolution tessellation [R18 §5] |

### Shading

| Feature | Manuka | Arnold | RenderMan |
|---|---|---|---|
| Shading language | RSL 2.0, JIT-compiled through LLVM (the in-house "Hyperion" compiler). OSL is not directly supported [M18 §3; M23] | C++ shaders and OSL, mixable in one network; OSL closures [A18 §4.6] | C++ pattern plug-ins, OSL and SeExpr [R18 §4] |
| MaterialX | n/d | Yes: MaterialX 1.38.10-OpenPBR (7.4.0) [AUG] | Partial: MaterialX Lama in XPU (early access at 27, completed in 27.3); general MaterialX still in development [R27; RDOC 27.3] |
| OpenPBR Surface | n/d | Yes: `openpbr_surface`, native C++ and OSL (7.3.3, 7.3.5) [AUG] | n/d |
| Material model | 187 hard-coded BxDF lobes, layered by a user-controllable, direction-dependent stack [M18 §3.4; M23] | `standard_surface`, `openpbr_surface`, `standard_hair`, toon [AUG] | Bxdf plug-ins: PxrSurface, PxrDisney and the ILM Lama layered materials [R18 §4; RTS] |
| When shading runs | **Shade before hit.** Patterns run on micropolygon vertices before transport. The BSDF is evaluated at the hit from interpolated inputs [M18 §3] | **Shade on hit,** one ray at a time, no ray packets [A18 §2.3] | **Shade groups.** Hit points that share a material are shaded in batches of up to a few hundred [R18 §3] |
| Subsurface scattering | Yes: fully path-traced (Dwivedi sampling), plus diffusion profiles [M18 §3.4] | Yes: random-walk volumetric SSS, plus diffusion [A18 §5.4] | Yes: dipole, Burley normalised diffusion, and path-traced [R18 §11; RTS] |
| Hair / fibre BSDF | Yes: near- and far-field fibre models with eccentricity; hair LOD [M18 §3.4]; double-cylinder models [M23] | Yes: `standard_hair` with approximate, accurate and adaptive modes (7.4.4) [A18 §5.5; AUG] | Yes: physical and artistic hair [RTS] |
| Wavelength effects (dispersion, iridescence, interference) | Yes, native: dispersion, interference, iridescence, diffraction, chromatic extinction [M18 §3] | Partial: dispersion, blackbody, melanin and sky by converting spectra to RGB [A18 §6.1]; thin film (7.4.4) [AUG] | Partial: dispersion [RTS]; iridescence in Lama (LamaIridescence) [RDOC 27.3] |
| Glints, cloth and yarn | Yes [M23] | n/d | n/d |
| Non-photorealistic / stylised | n/d | Yes: toon shader (GPU from 7.4.1), tone zones (7.5.2) [AUG] | Yes: Stylized Looks [RTS; R27] |
| Textures | Grid-coherent shading through a texture cache [M18 §3] | Mip-mapped, tiled, cached; UV, UDIM and projective mappings [A18 §4.5] | Mip-mapped with ray differentials; Ptex and UDIM [R18 §5; RTS] |
| Baking | Partial: the shaded micropolygon store can be exported with per-vertex albedo [M23] | Yes: UV-camera baking and a geometry baking API [AUG] | Yes: texture and light baking [RTS] |

### Lights

| Feature | Manuka | Arnold | RenderMan |
|---|---|---|---|
| Emitter model | Lights are geometry with emissive lobes, so lights can be textured and displaced; scales to millions of emitting micropolygons [M18 §3.4] | Analytic light nodes plus mesh lights [AUG Lights] | Analytic lights plus emissive geometry [RTS] |
| Light types | Any emissive geometry [M18] | Point, spot, distant, quad, disk, cylinder, skydome, mesh, photometric [AUG Lights] | Analytic area lights, dome, daylight simulator, mesh lights (full XPU support in 27) [RTS; R27] |
| IES profiles | n/d | Yes: photometric light [AUG Lights] | Yes [RTS] |
| Portals | n/d | Yes: light portal [AUG Lights] | Yes: PxrPortalLight [RTS; RDOC] |
| Light filters | Yes: gobos [M18 §4] | Yes (listed on the Lights page) [AUG Lights] | Yes: barn door, cookie, rod, ramp and others, combinable [RDOC Light Filters] |
| Many-light sampling | Light hierarchy over emitters, with learned visibility per node [M18 §3.4] | Light BVH with culling [A18 §4.3]; Global Light Sampling, glossiness-aware, also in volumes (7.2.1 onward) [AUG] | Light selection that accounts for the bxdf [R18 §5]; global light sampling [RTS] |
| Light linking and light groups | Partial: fixed light-path selectors [M18 §4] | Yes: light linking, light-group AOVs [AUG] | Yes: light groups through LPEs [R18 §8] |
| Per-light controls (visibility, bounces) | n/d | Yes: per-ray-type visibility and max bounces per light [AUG Lights] | Yes: per-light integrator control (PxrUnified) [RTS] |
| Physical units / scene-referred lighting | Yes: PhysLight, using measured camera spectral sensitivity and physical units [PL20] | Partial: exposure and normalise controls [AUG Lights] | Partial: colour temperature [RTS] |
| Lights as code | Partial: RSL illuminance loops, a legacy back door [M18 §4] | n/d | Yes: light plug-ins [R18 §4] |

### Light transport

| Feature | Manuka | Arnold | RenderMan |
|---|---|---|---|
| Unidirectional path tracing with NEE and MIS | Yes, the most used technique [M18 §3.4] | Yes, the only technique [A18 §4.1] | Yes: PxrPathTracer [R18 §11] |
| Bidirectional path tracing | Yes [M18 §3] | **No**, by design [AUG Caustics] | Yes: PxrUnified, VCM [R18 §12; RTS] |
| VCM / photon mapping | Yes, but rarely used because of bias and cost [M18 §3.4] | **No** [AUG Caustics] | Yes: VCM; experimental UPBP [R18 §12] |
| Manifold next-event estimation (caustics) | Yes [M18 §3.4] | No | Yes: PxrUnified [RTS] |
| Path guiding | Yes [M18 §3.4] | n/d. The 2018 paper names it as under investigation [A18 §4] | Yes: PxrUnified light-path guiding [RTS]; volume guiding in PxrPathTracer (27.3) [RDOC 27.3] |
| Metropolis / MCMC | No: removed for temporal stability [M18 §2] | No | Experimental only [R18 §12] |
| Path regularisation, clamping, caustic suppression | Partial: roughness can grow with bounce count [M18 §4] | Yes: roughness clamping and indirect clamping [A18 §4.2] | Yes: caustic toggles; firefly sample filters [R18 §11, §4] |
| Adaptive sampling | Yes: relative variance at several image scales [M18 §3.4] | Yes [A18; AUG] | Yes [R18 §7.2] |
| Sample sequences | Hero-wavelength stratification [M18 §3.4] | Correlated multi-jittered (CMJ) [A18 §5.1] | Progressive multi-jittered and Owen-scrambled Sobol' [R18 §7.1] |
| Custom light-transport plug-ins | No (built-in modules) [M18] | No (shaders may integrate light) [A18 §4.6] | Yes: integrator plug-ins [R18 §4] |
| GPU execution | No: an early GPU path was dropped [M18 §2] | Yes: OptiX 8 GPU renderer (7.2.5) [AUG] | Yes: XPU (CPU+GPU), multi-GPU, scene must fit each GPU [R27; RTS] |

### Volumes

| Feature | Manuka | Arnold | RenderMan |
|---|---|---|---|
| OpenVDB input | n/d (own structure, similar in spirit to Wrenninge 2016) [M18 §3.4] | Yes; mip-mapped grids (7.4.5) [A18 §3.3; AUG] | Yes, plus Maya fluids [RTS] |
| Volume representation | Diced into anisotropic microvoxels, shaded before transport [M18 §3.4] | Volume plug-ins that report valid-data intervals [A18 §3.3] | Volume primitive, with interior integrators [R18 §4] |
| Distance sampling and tracking | Regular tracking (3D DDA), chosen because hero-wavelength sampling needs pdfs; equiangular-like scatter sampling [M18 §3.4] | Decoupled ray marching; equiangular sampling [A18 §5.3] | Interior integrators handle scattering [R18 §4] |
| Overlapping volumes | Yes: summed coefficients, priority-based CSG [M18 §3.4] | n/d | Yes: interior volume aggregates (XPU 27) [R27] |
| Volume motion blur | Yes [M18 §3.4] | Yes: Eulerian [A18 §4.4.2] | n/d |
| Chromatic (wavelength-dependent) extinction | Yes, e.g. for skin [M18 §3.4] | No (RGB) | No (RGB) |

### Cameras

| Feature | Manuka | Arnold | RenderMan |
|---|---|---|---|
| Perspective with depth of field | Yes [M18] | Yes [AUG Cameras] | Yes [R18 §6.5] |
| Aperture blades / bokeh | n/d | Yes: blade count, curvature, rotation [AUG Cameras] | Yes: depends on the projection plug-in [RDOC Projection Plugins] |
| Non-perspective projections | Yes: fisheye, lat-long [M18 §3.4] | Yes: orthographic, fisheye, cylindrical, spherical, VR [AUG Cameras] | Yes: orthographic, sphere, cylinder, torus, Panini, omnidirectional stereo [RDOC] |
| Lens distortion | n/d | Yes: UV remap image [AUG Cameras] | Yes: radial, asymmetric, anamorphic [RDOC Lens Distortion] |
| Rolling shutter | n/d | Yes [AUG Cameras] | Yes [RTS; RDOC] |
| Custom shutter curve | n/d | Yes [AUG Cameras] | n/d |
| Tilt-shift, chromatic aberration, vignetting | n/d | Partial: vignetting through a filter map [AUG Cameras] | Yes: PxrCamera [RTS; RDOC] |
| Several cameras in one render (stereo) | Yes: path reconnection to several sensor points [M18 §3] | n/d | n/d |
| Spectral camera response | Yes: measured sensitivity curves of the plate camera [M18 §3; PL20] | No | No |
| Custom projection plug-ins | n/d | n/d | Yes: RixProjection [RDOC] |

### Outputs and AOVs

| Feature | Manuka | Arnold | RenderMan |
|---|---|---|---|
| Arbitrary AOVs | Yes, computed on the full path after sampling [M18 §3] | Yes; 30+ per frame is common [A18 §6.2] | Yes, programmable [R18 §8; RTS] |
| Light path expressions | **No**: a fixed set of selectors only [M18 §4] | Yes, including light-group syntax [AUG LPE AOVs] | Yes [R18 §8; RTS] |
| Deep EXR | Yes: deep buffers [M18 §4] | Yes, with deep compression [A18 §6.2] | Yes; XPU deep workflows since 27 [RTS; R27] |
| Cryptomatte | n/d | Yes: native, GPU-capable (7.3.5) [AUG] | Yes; in XPU since 27.3 [RTS; RDOC 27.3] |
| Holdouts and mattes | Yes: holdouts, shadow casters, camera visibility [M18 §4] | n/d | Yes [R27] |
| Checkpoint / resume | Yes: frame buffer, seeds and cached tessellation [M18 §4] | Partial: time-limited batch render writes the partial frame; interactive pause and resume (7.5.4) [AUG] | Yes: checkpoints, time-limited exits, restart; in XPU since 27 [R18 §7.3; R27] |
| Built-in denoising | **No**: left to post tools [M18 §3.4] | Yes: noice, OIDN and OptiX imagers [AUG] | Yes: Disney ML denoiser (interactive in XPU), OptiX [R18 §13; RTS] |
| Post-process stage in the renderer | n/d | Yes: imagers (tonemap, light mixer, lens effects, ONNX inference, shader imager) [AUG] | Yes: display and sample filters, including OSL ones [R18 §4; R27] |
| Firefly / outlier rejection | Yes: at splatting [M18 §3] | Yes: clamping [A18 §4.2] | Yes: sample filters [R18 §4] |

### Colour

| Feature | Manuka | Arnold | RenderMan |
|---|---|---|---|
| Transport | **Spectral**, hero-wavelength sampling [M18 §3] | **RGB**, with a configurable rendering-space gamut [A18 §6.1] | **RGB**: bxdfs return RGB values [R18 §4] |
| RGB-to-spectrum uplifting | Yes: optimisation per input colour space, illuminant E for reflectance [M18 §3] | Not applicable | Not applicable |
| Fluorescence | No: deliberately not implemented [M18 §3] | No | n/d |
| OpenColorIO | Colour management throughout, with photometric controls for lighters [M18 §3] | Yes: SynColor and OCIO [A18 §6.1] | Yes: OCIO configs, ACEScg recommended [RDOC] |
| Output colour formation | Projection through the plate camera's spectral response, which matches its metamerism [M18 §3; PL20] | Colour-space conversion [A18 §6.1] | Colour-space conversion [RDOC] |

### Interactivity

| Feature | Manuka | Arnold | RenderMan |
|---|---|---|---|
| Interactive (IPR) rendering | Partial: a separate "Manuka Interactive" mode for lookdev and lighting, with second-scale frame times, a layered BVH and on-the-fly edits. Sub-second preview is a separate renderer, Gazebo [M23] | Yes: designed for it; any scene edit [A18 §2] | Yes: any edit, including integrators and plug-ins, through the Riley API [R18 §9] |
| Time to first pixel | Slow by design (shade before hit). Mitigated by caching the shaded-micropolygon store [M18 §3; M23] | Fast: no precomputation [A18 §2] | Fast: progressive sampling [R18 §7] |
| Multi-frame reuse | Yes: one shaded store reused over several frames [M23] | n/d | n/d |

### Distribution

| Feature | Manuka | Arnold | RenderMan |
|---|---|---|---|
| One frame split across several nodes | n/d | n/d | n/d |
| Farm and cloud integration | Cloud rendering (Avatar: The Way of Water); shading and the pre-shaded store can be distributed [M23] | Flow Render cloud service (tech preview, 7.5.1) [AUG] | Tractor job queue [RTS] |
| Many-core scaling | Yes [M18 §3] | Yes [A18 §2.3] | Yes [RTS] |

### Extensibility and scene input

| Feature | Manuka | Arnold | RenderMan |
|---|---|---|---|
| Scene input | RIB with a RenderMan Interface frontend; Wētā's Atlas; increasingly USD [M18 §3; M23] | `.ass` files and the C API; USD through a Hydra delegate and husk [AUG] | RIB and the Riley API; USD through Hydra (hdPrman) [R18 §9; RTS] |
| Plug-in surface | RI filter, procedural, shadeop and display (dspy) plug-ins; tessellation plug-ins. BxDFs are not pluggable [M18 §3; M23] | Node API for shaders, procedurals, volumes, drivers and imagers (public imager API since 7.3.1) [A18 §4.6; AUG] | Plug-ins for bxdfs, integrators, patterns, displacement, lights, light filters, projections, sample filters and display filters [R18 §4] |

## Where the three renderers diverge

1. **Colour transport.** Manuka is the only spectral renderer. It uses
   hero-wavelength sampling, RGB-to-spectrum uplifting and output through the
   plate camera's spectral response. Arnold and RenderMan transport RGB and
   approximate a few spectral effects. Arnold's own paper concludes that
   "physically accurate results require spectral rendering" [A18 §6.1].
2. **When shading runs.** Manuka shades before hit, which favours memory locality
   at the cost of a slow first pixel. Arnold shades each hit one ray at a time.
   RenderMan shades hits in batches grouped by material.
3. **Range of light-transport algorithms.** Arnold commits to unidirectional path
   tracing by design: no BDPT, no photons, no MNEE, and no documented path guiding [A18 §4.1; AUG Caustics].
   Manuka and RenderMan both ship BDPT, VCM, MNEE and path guiding. RenderMan
   additionally makes the integrator a user plug-in.
4. **Shading languages.** Arnold and RenderMan run OSL and MaterialX (OpenPBR in
   Arnold, Lama in RenderMan). Manuka runs RSL through its own LLVM compiler and
   does not support OSL directly.
5. **Light path expressions.** Arnold and RenderMan support general LPEs. Manuka
   exposes only a fixed set of selectors.
6. **Lights.** Manuka treats every light as emissive geometry and scales to
   millions of emitters. Arnold and RenderMan centre on analytic lights, with
   filters, portals and IES profiles.
7. **Denoising and post-processing.** Arnold and RenderMan integrate denoisers and
   in-renderer post stages (imagers, display filters). Manuka deliberately leaves
   denoising to post.
8. **GPU.** Arnold (OptiX) and RenderMan (XPU) execute on GPUs. Manuka runs on
   the CPU only.
9. **Interactivity.** Arnold and RenderMan each run batch and interactive
   rendering in one engine. Wētā splits the work across Manuka batch, Manuka
   Interactive and the separate Gazebo renderer.
10. **Multi-node rendering of one frame.** None of the three documents splitting
    a single frame across several nodes. Each relies on farm or cloud scheduling
    of whole frames.

## Notes for Umbra's quality goals (arc42 §1)

- Combining spectral transport (Manuka) with OSL, MaterialX and OpenPBR shading
  (Arnold and RenderMan) has no public precedent among the three renderers.
  Manuka shows the spectral pipeline can work at production scale. The other two
  show that OSL and MaterialX are the expected shading interface.
- A parity target is the **union** of the matrix, not the intersection. Its
  hardest parts are:
  - the light-transport breadth of Manuka and RenderMan (BDPT, VCM, MNEE, path
    guiding);
  - general LPEs and deep output with Cryptomatte;
  - checkpoint and resume;
  - many-light sampling;
  - chromatic media, which needs pdf-capable volume tracking under hero-wavelength
    sampling [M18 §3.4].
- Single-frame multi-node distribution, which the map has already settled, is
  beyond anything these three document publicly. It is a differentiator, not a
  parity item.
