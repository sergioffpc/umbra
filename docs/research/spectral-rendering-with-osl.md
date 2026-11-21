# Spectral rendering with OSL closures

Research for issue #15. Question: how is spectral rendering reconciled with OSL, whose patterns compute colour as RGB triples? This note collects the facts the **spectral-representation contract** will need. It covers closure evaluation in a spectral integrator, spectral uplifting at the closure→BSDF boundary and how it interacts with OpenColorIO working spaces, wavelength sampling, and how Manuka separates shading from light transport.

Every claim cites its primary source (see [Sources](#sources)). Statements marked **Implication** are inferences for Umbra's contracts, not facts from the sources.

## 1. What OSL gives a renderer

### 1.1 `color` is a three-component RGB value

- The OSL `color` type is "used to represent 3-component (RGB) spectral reflectivities and light energies" [OSL-types]. Even though the spec says "spectral", the representation is fixed at three components.
- Colours built in another named space (`"hsv"`, `"hsl"`, `"YIQ"`, `"XYZ"`, `"xyY"`) go through an "implied transformation to `"rgb"` space". `"rgb"` is "the coordinate system that all colors start out in, and in which the renderer expects to find colors that are set by your shader" [OSL-types].
- The renderer chooses what `"rgb"` means. The `ShadingSystem` attribute `string colorspace` names the RGB colour space (default `"Rec709"`) [OSL-exec]. The built-in table includes Rec709, sRGB, NTSC, EBU, PAL, SECAM, SMPTE, HDTV, CIE, AdobeRGB, XYZ, ACES2065-1 and ACEScg [OSL-opcolor]. `transformc` with a space name that is not built in goes through an OCIO path (`ColorSystem::ocio_transform`) [OSL-opcolor].
- Spectral helpers return RGB, not spectra. `blackbody(T)` returns a `color` in W/m² [OSL-stdlib]. `wavelength_color(λ)` returns "an RGB color corresponding as closely as possible to the perceived color of a pure spectral color" [OSL-stdlib]. `luminance` uses fixed Rec.709 weights (0.2126, 0.7152, 0.0722) [OSL-stdlib].

**Implication:** Every coloured quantity that leaves an OSL shader is an RGB triple in the renderer-chosen `"rgb"` space. No spectral information crosses the OSL boundary unless Umbra adds its own closures or parameters. Umbra must set `"rgb"` to the same space as its OCIO rendering space, or `color("XYZ", …)` and `transformc` in shaders will disagree with the uplifting tables.

### 1.2 Closures are opaque and the renderer evaluates them

- Surface and volume shaders "compute a *closure*, which is a symbolic representation describing the appearance of the surface, that may be more fully evaluated later" [OSL-bigpicture]. "The shader itself cannot examine the numeric values of the `closure color`" [OSL-stdlib].
- The renderer's integrators "combine the color closures computed by surfaces and volumes with the light sources" [OSL-bigpicture]. Closures can be *evaluated*, *sampled*, *integrated* and *recomputed* by the renderer [OSL-bigpicture].
- The standard material closures are "a minimal but useful set … that you can expect any renderer implementation to provide". They are synchronised with the MaterialX physically based nodes. Renderers may add closures and extra token/value parameters [OSL-stdlib].
- In memory, a closure is a tree. Leaves are `ClosureComponent`s (closure id, a `Vec3 w` weight and parameter data). Internal nodes are `MUL` (a `Color3 weight` times a sub-closure) or `ADD` (two sub-closures). The header notes that the tree "really just boils down to a flat list of weighted closure primitive components" [OSL-closure-h].
- Allowed closure arithmetic: scaling by `float` or `color`, addition, and unary negation [OSL-types].

**Implication:** OSL already fixes the boundary that issue #11 places at "closure→BSDF". The shader produces a flat list of (RGB weight, closure id, parameters). Umbra turns each entry into a spectral BSDF/EDF/VDF. Flattening the tree gives each leaf one accumulated RGB weight, the product of the `MUL` weights on its path. Uplifting that product is not the same as multiplying uplifted factors, because the uplifting maps are nonlinear (§3.2). The contract must state which of the two Umbra does. Flattening first is cheaper and is the only option once the tree is gone.

### 1.3 The RGB inputs of the standard closures

Each standard closure has its own RGB parameters [OSL-stdlib]. They fall into different physical classes, and each class needs a different uplifting rule:

| Class | OSL parameters (examples) | Physical range |
|---|---|---|
| Reflectance / albedo | `oren_nayar_diffuse_bsdf.albedo`, `burley_diffuse_bsdf.albedo`, `sheen_bsdf.albedo`, `translucent_bsdf.albedo`, `anisotropic_vdf.albedo`, `medium_vdf.albedo` | [0, 1] per wavelength |
| Fresnel tints / Schlick endpoints | `dielectric_bsdf.reflection_tint` / `transmission_tint`, `generalized_schlick_bsdf.f0` / `f90` / tints | [0, 1] (multipliers) |
| Complex IOR | `conductor_bsdf.ior`, `conductor_bsdf.extinction` (or from `artistic_ior(reflectivity, edge_tint)`) | unbounded optical constants, not colours |
| Scalar IOR | `dielectric_bsdf.ior`, `medium_vdf.ior` (float) | float, so no dispersion through the standard interface |
| Extinction / transmission colour | `anisotropic_vdf.extinction`, `medium_vdf.transmission_color` (+ `transmission_depth`) | extinction is unbounded; transmission colour is [0, 1] |
| Emission | `uniform_edf.emittance` (radiance, W·sr⁻¹·m⁻²) | unbounded |
| Closure weights | `ClosureMul.weight`, `ClosureComponent.w` | in practice usually [0, 1]; unbounded in general (and may be negative) |
| AOV / signalling | `debug(outputname)` weight, `holdout()` weight | not light transport, so stays RGB/float |

**Implication:** The contract needs at least three uplift classes: *bounded reflectance*, *unbounded emission/illuminant*, and *unbounded non-colour physical constants* (complex IOR, extinction coefficients). Uplifting the last class with a reflectance model is not physically meaningful. The contract has to name a policy for it, for example per-channel fitting or a measured-spectrum override through renderer-specific closure parameters. OSL allows renderer-specific closures and extra token/value parameters [OSL-stdlib], so Umbra can accept true spectral data, such as tabulated IOR or measured emission, without breaking standard shaders.

## 2. MaterialX colour management at the boundary

- Every `color3` value and the RGB part of every `color4` value is "presumed to be specified in the 'working color space'". The working colour space is set by the root `colorspace` attribute, and any scope can override it. "Implementations are expected to translate those color values into the working color space before performing computations" [MTLX-spec].
- The document working space "may differ from the rendering color space in which all color computations will be performed". "MaterialX does not specify *how* or *when* color values should be transformed: that is up to the host application" [MTLX-spec].
- The current colour-space names come from the ASWF Color Interop Forum recommendation, for example `lin_ap1_scene` (ACEScg, AP1, D60) and `lin_rec709_scene` (Rec.709, D65). Older names such as `lin_rec709` and `acescg` are deprecated but still supported [MTLX-spec].
- `data` (alias `none`) means "no color space conversion should be applied" [MTLX-spec].

**Implication:** In the MaterialX → OSL → closure chain, all pattern RGB has already been converted into one rendering space by the time it reaches a closure. That rendering space is the input space of the uplifting step. `data`-tagged inputs (normals, roughness maps, masks) are never uplifted. The Shading contract has to carry the "is colour vs is data" distinction as far as the closure parameter. For OSL standard closures the parameter type answers this (`color` vs `float`/`normal`). A `color`-typed parameter fed from `data` is ambiguous and needs a rule.

## 3. Spectral uplifting

### 3.1 The problem and its constraints

- RGB-to-spectrum uplifting is ill-posed. Each tristimulus value corresponds to an infinite-dimensional set of metameric spectra [JH19].
- Reflectance spectra are bounded by 1 at every wavelength. Saturated, bright chromaticities need peaky spectra, so brightness must drop. Some inputs therefore have no energy-conserving reflectance spectrum, and some form of gamut mapping is required [Manuka §3.2, JH19 §2]. Brighter saturated colours would need fluorescence to reproduce. Manuka chose not to implement fluorescence [Manuka §3.2].
- The constraints Jakob & Hanika list for an uplifting method are round-trip identity where possible, smoothness in λ and in the input colour, very cheap evaluation, inlinable code, no added memory traffic, and SIMD-friendliness [JH19 §1].

### 3.2 Jakob & Hanika 2019 (sigmoid-polynomial model)

- Model: f(λ) = S(c₀λ² + c₁λ + c₂), with S(x) = ½ + x / (2√(1+x²)). Three coefficients per texel give "storage requirements … identical to standard RGB textures". Evaluation takes "as few as six floating point operations" and is "trivially vectorizable" (SSE/AVX/AVX-512) [JH19 abstract, §3].
- The output is intrinsically energy-conserving (in [0, 1]), with "zero error on the full sRGB gamut". It supports large-gamut spaces. Precomputed tables are supplied for sRGB/Rec.709, Rec.2020, ProPhoto RGB and ACES2065-1 [JH19 §2, §3].
- Coefficients are optimised offline (a Levenberg-Marquardt solver) and tabulated per RGB colour space in a 3D grid. Three 64³ cubes total 9 MiB, with brightness slices spaced by smoothstep(smoothstep(i/63)). Lookup is trilinear. The tables are read only "once at load time to transform RGB texels into model parameters", so rendering pays no extra memory traffic [JH19 §3.1–3.2].
- Interpolating coefficients is not the same as interpolating RGB, because S is nonlinear. The paper concludes that "within limits" interpolating coefficients is reasonable [JH19 §4].
- Scope limit: the method "does not apply to light source spectra" and does not cover iridescence or similar oscillatory spectra [JH19 §1 footnote].

### 3.3 Meng et al. 2015 and Manuka's approach

- Meng et al. precompute spectra on a 2D xy-chromaticity discretisation of the spectral locus. Spectra are later rescaled for brightness [JH19 §2 on MSHD15]. Manuka cites Meng et al. for strategies that produce energy-conserving spectra with close chromaticity [Manuka §3.2].
- Manuka's own upsampler is "an optimisation strategy similar to … Smits [1999]", with these properties [Manuka §3.2]:
  - **Emission spectra may exceed 1.** Instead of energy conservation, the constraint is "that a D65 input spectrum round-trips without error".
  - **The input colour space is the texture's own space**, "instead of using fixed sRGB primaries".
  - **For reflectances, the white point is CIE Illuminant E**, so R = G = B maps to a uniform (flat) reflectance spectrum. In the paper's words, "the white point is governed by the illumination, which is not part of the reflectance spectrum".
  - **Equivalence is judged in camera RGB** using the measured spectral response of the principal-photography camera, not the CIE XYZ matching functions. For image-based lighting, "the responsivities of the camera that was used to take the picture" are used.
- Manuka also accepts full binned spectra as input. RGB textures are used to save memory [Manuka §3.2].

**Implications for the contract:**

1. Uplifting is a function of *(RGB triple, source colour space, quantity class)*, not of RGB alone. Both Manuka and JH19 tie the result to a specific RGB colour space.
2. Reflectance and emission need different uplifts. Reflectance is bounded and uses Illuminant E white. Emission is unbounded and must reproduce the illuminant white, for example D65. The JH19 sigmoid model alone covers reflectance only.
3. When uplifting happens is an architectural choice. It can happen at texture load (JH19: convert texels to coefficients once) or at closure construction (convert the leaf's RGB weights and parameters once per shading point). In both cases the per-wavelength cost at BSDF evaluation stays a few flops.
4. A *colorimetric observer* is a contract parameter: CIE 1931 2° XYZ by default, or measured camera responsivities for plate matching as in Manuka. It is used both for uplifting equivalence and for the film's spectrum-to-RGB projection.

## 4. Colour formation: spectrum to output RGB

- The standard route projects the spectrum onto the CIE 1931 2° colour-matching functions x̄, ȳ, z̄ and then applies a 3×3 matrix to the target space (sRGB, ACES, …). "Most contemporary definitions of colour spaces employ xyz coordinates in this central role of pivot space" [Manuka §3.2].
- For VFX plate integration, Manuka uses measured camera spectral responsivities, converted into colour-matching functions, so that renders reproduce the camera's metamerism rather than a human observer's [Manuka §3.2]. The light transport stage uses the traced wavelength to apply "the spectral camera sensitivity of the sensor" [Manuka §1].
- Manuka uses colour management throughout and photometric quantities for lighting UI, so a light's colour can change without changing its perceived brightness [Manuka §3.2].
- OCIO has no spectral concept. Its roles relevant here are [OCIO-types, OCIO-docs]:
  - `scene_linear`: the scene-referred linear space.
  - `rendering`: the space "to be used by CGI renderers … the primaries also matter since they influence the resulting color, especially in areas of indirect illumination".
  - `aces_interchange`: ACES2065-1, for scene-referred config-to-config conversion.
  - `cie_xyz_d65_interchange`: CIE XYZ with a D65 neutral axis, for display-referred conversion.
  - `data`: no colour transforms.

**Implications:**

- OCIO sits only at the two RGB edges of a spectral pipeline. On input, it resolves every authored colour into the space from which uplifting happens (the `rendering` role is the natural anchor). On output, the film accumulates XYZ, or observer-space tristimulus, and converts it to the output space through OCIO, for example via `cie_xyz_d65_interchange` or `aces_interchange`.
- The OCIO header comment about primaries affecting indirect illumination describes an RGB-renderer artefact. Spectral transport removes that dependence, apart from the choice of uplift tables [Manuka §3.2: "a different working space will yield different results" in RGB rendering].
- The uplift tables must exist for each supported rendering space, or inputs must first be converted by OCIO into one space that has tables, such as ACES2065-1 or Rec.709 for JH19's shipped tables. White-point handling (Illuminant E for reflectance vs the space's white for emission) has to be explicit, because the OCIO spaces involved carry D60 or D65 whites.

## 5. Wavelength sampling: hero wavelength

- Manuka traces "a single, randomly selected wavelength for each path" for the best importance sampling. It controls colour noise with hero wavelength spectral sampling and computes the throughput of the extra wavelengths with SIMD "at little additional cost" [Manuka §3.2].
- Hero wavelength [HWSS14 §3]:
  - The hero wavelength λₕ is sampled per path exactly as in a single-wavelength renderer, so it needs no extra random numbers. **All directional sampling decisions use λₕ only.**
  - C − 1 additional wavelengths are placed deterministically at equal spacing: rⱼ(λₕ) = ((λₕ − λmin + (j/C)·λ̄) mod λ̄) + λmin, with λ̄ = λmax − λmin, j = 1 … C.
  - Contributions are combined with spectral MIS using the balance heuristic, w_s(X, λ) = p_s(X, λ) / Σₖ pₖ(X, λ). Each rotation is treated as a separate technique. This requires **evaluating the sampling pdf for all C wavelengths**, not just the hero.
  - It is "not sufficient to just replace all single wavelength references … with SSE-compatible 4-vectors". The BSDF code must return pdfs per wavelength and divide by the pdf that actually generated the sample.
  - The engineering choice was **C = 4**, one SSE lane per wavelength. The method can be recompiled for multiples of 4, but 4 "proved to be sufficient in practice" [HWSS14 §3.4].
  - **Dispersion through Dirac interfaces:** paths possible only for the hero wavelength keep only the hero channel and blank the others. The method then "gracefully regresse[s] to single wavelength behaviour" for those paths and still removes colour noise elsewhere [HWSS14 §3.3, §5].
  - The paper covers coupling with NEE, BDPT, photon maps (a photon carries 4 wavelengths, tested against the query's hero), environment maps and participating media [HWSS14 §4].
- Manuka relies on pdf evaluation for every technique (it computes MIS weights after path construction from a cached vertex graph). It notes that spectral tracking [Kutz et al. 2017] is "fundamentally incompatible with the hero wavelength sampling scheme because spectral tracking cannot provide pdf evaluations". Hero-wavelength sampling matters most "in media with chromatic extinction, such as skin" [Manuka §3.3, §5 volumes].

**Implications for the contract:**

- The spectral sample type is a fixed-width vector of wavelengths plus a per-lane value: C = 4 per HWSS14, or 8 to fill AVX2 lanes on x86-64-v3, as a tuning choice. Lane 0 is the hero.
- The BSDF contract per wavelength lane must expose: evaluate f(ωi, ωo, λ⃗), sample(ωo, λₕ, u), and **pdf(ωi, ωo, λ⃗) for every lane**. It must also expose a "wavelength-dependent Dirac" flag (dispersion) that collapses the sample to the hero lane.
- Uplifted quantities must be evaluable at arbitrary continuous λ, because wavelengths are sampled continuously. A fixed binned representation does not meet this need.
- Volume/medium closures must offer per-wavelength pdfs for distance sampling. That excludes estimators without pdfs, such as spectral tracking, under hero MIS.

## 6. Manuka: separating shading from light transport

- Manuka's three architectural cornerstones are **shade before hit**, **spectral rendering** and **deferred splatting** [Manuka §1].
- **Shade before hit:** RSL shaders are not run at ray hits. All geometry is pre-tessellated and pre-shaded in the front end, so "we can efficiently order shading computations to support near-optimal texture locality, vectorisation, and parallelism" [Manuka §1]. "Shading in our system does not include bsdf evaluation" [Manuka abstract].
- **The shader output is BSDF inputs, not a closure:** "While in classic rsl the result of running a surface shader is the final color … and in osl is a closure, in Manuka the result of a shader is a data aggregate containing the bsdf inputs, their weights, and the layering setup". This aggregate is compressed and stored per vertex, separately from ray-tracing geometry [Manuka §3.1].
- At hit time, BSDF *inputs* are bilinearly interpolated from the grid vertices, and the BSDF itself is evaluated exactly at the hit point ("Phong shaded, based on parameters that are Gouraud interpolated") [Manuka §3.1].
- Shaders are batch-executed over whole grids. Grids are merged up to texture seams for locality. Shaders are JIT-compiled from LLVM bytecode at startup for SSE/AVX. The same scheme covers hair (line segments) and volumes (voxels) [Manuka §3.1].
- **Path-first, deferred MIS/splatting:** sampling techniques only build paths into an acyclic vertex graph that caches BSDF/pdf evaluations. MIS weights and splatting are computed afterwards, so techniques need not know about each other [Manuka §3.3].
- OSL supports batched SIMD execution (`BatchedShadingSystem`, targets such as `b8_AVX2`, `b16_AVX512`) [OSL-exec]. The renderer supplies shader globals, so OSL execution is not tied to ray hits.

**Implications:**

- Where uplifting happens follows from the shading model. In a shade-before-hit design, the natural point is *when the closure list is converted to the stored BSDF-input aggregate* (once per vertex). In shade-on-hit, it is once per hit. Either way, the light-transport side sees only spectral-ready BSDF inputs (for example, three JH19 coefficients per reflectance), never RGB. This keeps the transport loop independent of OCIO.
- With the JH19 representation, a stored "spectral reflectance" is the same size as an RGB triple. So a Manuka-style per-vertex cache costs nothing extra for being spectral, and coefficients can be interpolated (within limits) [JH19 §4].
- Shading and transport communicate only through a value type (the BSDF-input aggregate or the flattened closure list). That is the seam the spectral-representation contract should define.

## 7. Facts the spectral-representation contract needs (checklist)

1. **Input boundary:** OSL/MaterialX deliver RGB in one renderer-chosen space. OSL's `"rgb"` must equal the OCIO `rendering` space [OSL-types, OSL-exec, MTLX-spec, OCIO-types].
2. **Boundary object:** a flat list of (closure id, RGB weight, typed parameters) [OSL-closure-h].
3. **Uplift classes:** bounded reflectance (energy-conserving, Illuminant E white); unbounded emission (round-trips the illuminant white); physical constants such as complex IOR and extinction (separate policy, or a measured-spectrum override); data, which is never uplifted [Manuka §3.2, JH19, OSL-stdlib].
4. **Uplift inputs:** RGB triple, source colour space, quantity class and observer [Manuka §3.2, JH19].
5. **Spectral representation:** continuous in λ, cheap to evaluate per lane, SIMD-friendly. For reflectance, three coefficients per value (the JH19 sigmoid polynomial) [JH19].
6. **Wavelength sample:** a fixed-width lane vector with a hero lane. Pdfs are available per lane. A dispersion collapse goes to the hero lane [HWSS14].
7. **Output boundary:** project onto the observer (CIE 1931 2°, or a camera responsivity), then convert XYZ/observer RGB to the output space with OCIO via an interchange role [Manuka §3.2, OCIO-types].
8. **Shading/transport seam:** shading produces spectral-ready BSDF inputs and never evaluates BSDFs. Transport evaluates BSDFs per wavelength lane [Manuka §3.1].
9. **Spectral data paths:** renderer-specific closure parameters or closures can carry real spectra, such as measured IOR, emission or tabulated data, beside the RGB path [OSL-stdlib, Manuka §3.2 "full spectra from binned data"].
10. **Known gaps:** fluorescence (not in Manuka, not expressible in OSL standard closures) and dispersion through the standard `dielectric_bsdf`, whose `ior` is a float [Manuka §3.2, OSL-stdlib].

## Sources

- **[OSL-types]** OpenShadingLanguage, *Language Specification — Data types* (`color`, Closures). `src/doc/datatypes.md`, AcademySoftwareFoundation/OpenShadingLanguage, `main`.
- **[OSL-bigpicture]** OpenShadingLanguage, *The Big Picture* (closures, integrators). `src/doc/bigpicture.md`.
- **[OSL-stdlib]** OpenShadingLanguage, *Standard Library* (Color functions; Material Closures). `src/doc/stdlib.md`.
- **[OSL-closure-h]** OpenShadingLanguage, `src/include/OSL/oslclosure.h` (`ClosureColor`, `ClosureComponent`, `ClosureMul`, `ClosureAdd`).
- **[OSL-exec]** OpenShadingLanguage, `src/include/OSL/oslexec.h` (`colorspace` attribute; batched targets).
- **[OSL-opcolor]** OpenShadingLanguage, `src/liboslexec/opcolor.cpp` (built-in colour systems; OCIO transform path).
- **[MTLX-spec]** MaterialX Specification, *Color Spaces and Color Management Systems*. `documents/Specification/MaterialX.Specification.md`, AcademySoftwareFoundation/MaterialX.
- **[OCIO-types]** OpenColorIO, `include/OpenColorIO/OpenColorTypes.h` (role constants and their documentation).
- **[OCIO-docs]** OpenColorIO documentation, *Config authoring overview*, <https://opencolorio.readthedocs.io/en/latest/guides/authoring/overview.html>.
- **[Manuka]** L. Fascione, J. Hanika, M. Leone, M. Droske, J. Schwarzhaupt, T. Davidovič, A. Weidlich, J. Meng. "Manuka: A Batch-Shading Architecture for Spectral Path Tracing in Movie Production." *ACM Transactions on Graphics* 37(3), Article 31, 2018. doi:10.1145/3182161. Preprint: <https://jo.dreggn.org/home/2018_manuka.pdf>.
- **[HWSS14]** A. Wilkie, S. Nawaz, M. Droske, A. Weidlich, J. Hanika. "Hero Wavelength Spectral Sampling." *Computer Graphics Forum* 33(4) (Proc. EGSR), 2014. <https://cgg.mff.cuni.cz/~wilkie/Website/EGSR_14_files/WNDWH14HWSS.pdf>.
- **[JH19]** W. Jakob, J. Hanika. "A Low-Dimensional Function Space for Efficient Spectral Upsampling." *Computer Graphics Forum* 38(2) (Proc. Eurographics), 2019. <https://rgl.s3.eu-central-1.amazonaws.com/media/papers/Jakob2019Spectral_3.pdf>.
- **[MSHD15]** (cited through Manuka and JH19) J. Meng, F. Simon, J. Hanika, C. Dachsbacher. "Physically Meaningful Rendering using Tristimulus Colours." *Computer Graphics Forum* 34(4) (Proc. EGSR), 2015.
