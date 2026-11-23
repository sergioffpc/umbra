# References

Public sources for ray tracing and physically based rendering research, covering BVHs,
OptiX/CUDA, many-light sampling, path tracing, and film/denoising. Each link was opened against
the primary source (the venue, studio, lab or repository). Entries marked *(unverified)* could
not be confirmed on the verification date below.

**ACM Digital Library access.** ACM's own pages say ACM became a fully open-access publisher on
1 January 2026. Every ACM journal, conference and magazine article, back archive included, can
now be read free on the *Basic* edition of the DL. *Premium* is the paid tier with extra tools,
and ACM Books are excluded. Sources: [acm.org/publications/openaccess](https://www.acm.org/publications/openaccess),
[ACM bulletin, Jan 2026](https://www.acm.org/articles/bulletins/2026/january/acm-open-access).
The acm.org and dl.acm.org pages refuse automated clients (HTTP 403), so this was confirmed from
ACM's pages as indexed by search, and the DOIs below were confirmed through Crossref.

## 1. Conferences and journals

| Name | URL | What's there |
|---|---|---|
| ACM Transactions on Graphics (SIGGRAPH / SIGGRAPH Asia papers) | https://dl.acm.org/journal/tog | TOG issues; SIGGRAPH and SIGGRAPH Asia technical papers appear as TOG issues. Free via DL Basic since 2026 *(page blocks automated fetch)* |
| SIGGRAPH 2026 | https://s2026.siggraph.org/ | Current conference site: program, courses, talks |
| SIGGRAPH Asia 2026 | https://asia.siggraph.org/2026/ | Current SIGGRAPH Asia conference site |
| ACM SIGGRAPH (YouTube) | https://www.youtube.com/@ACMSIGGRAPH | Official channel with talk and paper videos |
| EGSR 2026 | https://egsr2026.inria.fr/ | 37th Eurographics Symposium on Rendering (Bordeaux, 1–3 July 2026). Note: `egsr.eu` is an unrelated German site, not the symposium |
| Eurographics Association | https://www.eg.org/wp/ | Parent association for EG, EGSR and HPG (co-sponsor) |
| Eurographics Digital Library | https://diglib.eg.org/ | DSpace repository: Computer Graphics Forum (EG and EGSR papers), STARs, and proceedings of EG symposia and workshops |
| High-Performance Graphics | https://www.highperformancegraphics.org/ | Redirects to the current edition (2027); past editions at `/2026/` etc. Ray tracing hardware/BVH venue |
| I3D | https://i3dsymposium.org/ | ACM Symposium on Interactive 3D Graphics and Games (redirects to the current year) |
| JCGT | https://jcgt.org/ | Journal of Computer Graphics Techniques: open access, practical techniques, often with code |
| arXiv cs.GR | https://arxiv.org/list/cs.GR/recent | Graphics preprints |
| Ke-Sen Huang's paper pages | https://kesen.realtimerendering.com/ | Per-conference paper lists (SIGGRAPH through 2026, EGSR, HPG, I3D…) with author PDF links |

## 2. Courses and talks

| Name | URL | What's there |
|---|---|---|
| Physically Based Shading (SIGGRAPH course) | https://blog.selfshadow.com/publications/ | Index of PBS course pages 2012–2025 (latest: [`s2025-shading-course`](https://blog.selfshadow.com/publications/s2025-shading-course/)) with slides and notes |
| Path Tracing in Production (SIGGRAPH course) | https://jo.dreggn.org/path-tracing-in-production/ | Course pages for 2017, 2018 and 2019 (notes and slides) |
| Advances in Real-Time Rendering in Games | https://advances.realtimerendering.com/ | Every year's course page, slides included (latest: s2025) |
| Open Problems in Real-Time Rendering | https://openproblems.realtimerendering.com/ | Course pages and slides |
| GDC Vault | https://gdcvault.com/ | GDC talk slides and videos (some need a paid account) |
| HPG (YouTube) | https://www.youtube.com/@highperformancegraphics | HPG talks; the HPG 2026 site also links its livestream recordings |
| EGSR 2026 (YouTube) | https://www.youtube.com/channel/UCWPrsFDWZFQLDJ6L1DeCFmQ | Session recordings, linked from the EGSR 2026 site. Each year has its own channel, e.g. [@egsr2024](https://www.youtube.com/@egsr2024) |
| Production rendering design talk (Pharr) | https://history.siggraph.org/wp-content/uploads/2022/03/2018-04_Pharr_Design-and-Implementation.pdf | Talk notes on "Design and Implementation of Modern Production Renderers" |

## 3. Studios and industry research

| Name | URL | What's there |
|---|---|---|
| Pixar research library | https://research.pixar.com/ | Pixar papers, talks, courses and technical memos (the page needs JavaScript). The old `graphics.pixar.com/library/` now redirects to the Pixar technology-libraries index |
| Walt Disney Animation Studios | https://disneyanimation.com/publications/ | Hyperion-era papers and talks |
| Disney Research Studios | https://studios.disneyresearch.com/publications/ | Publications: rendering, denoising, and more |
| DreamWorks Animation research | https://www.dreamworks.com/research | `research.dreamworks.com` redirects here. The page needs JavaScript; paper PDFs are still served from `research.dreamworks.com/wp-content/…` *(listing content unverified)* |
| OpenMoonRay | https://openmoonray.org/ | MoonRay project site; docs at https://docs.openmoonray.org/ |
| Wētā FX key publications | https://www.wetafx.co.nz/research-and-tech/key-publications | Manuka and other publications (moved from `/publications/`) |
| Sony Pictures Imageworks | https://www.imageworks.com/technology/publications | Talks and papers, OSL included; open-source projects at `/technology/opensource` |
| Autodesk Arnold research | https://blogs.autodesk.com/media-and-entertainment/2024/01/04/autodesk-arnold-research-papers/ | 17 Arnold papers, 2011–2020 (MIS, many-light/volume sampling, BVH traversal). The old `arnoldrenderer.com/research` redirects here |
| NVIDIA Research | https://research.nvidia.com/publications | All NVIDIA Research publications |
| NVIDIA Real-Time Graphics Research | https://research.nvidia.com/labs/rtr/ | Rendering lab: ReSTIR, path tracing, denoising, neural rendering |
| NVIDIA OptiX | https://developer.nvidia.com/rtx/ray-tracing/optix | OptiX SDK and documentation |
| Intel Embree | https://www.embree.org/ | CPU ray tracing kernels, docs and papers |
| Intel Open Image Denoise | https://www.openimagedenoise.org/ | Denoiser library and docs |
| Intel Open PGL | http://www.openpgl.org/ | Redirects to the GitHub repo (no HTTPS on the domain) |
| AMD GPUOpen publications | https://gpuopen.com/learn/publications/ | AMD research papers 2011–2026 (HIPRT, light sampling, radiance caching) |
| EA SEED | https://www.ea.com/seed/publications | SEED papers and talks |
| Activision Research | https://research.activision.com/publications | Call of Duty rendering talks and papers |
| Ubisoft La Forge | https://www.ubisoft.com/en-us/studio/laforge/publications | La Forge publications (page needs JavaScript) |

**ACM TOG special issue on production rendering.** *ACM Transactions on Graphics* 37(3), 2018 ([TOC](https://dl.acm.org/toc/tog/2018/37/3)); Matt Pharr was guest editor. The DOIs were confirmed through Crossref:

| Article | DOI |
|---|---|
| Guest Editor's Introduction (Pharr), [free PDF](https://pharr.org/matt/assets/tog_intro.pdf) | [10.1145/3212511](https://doi.org/10.1145/3212511) |
| Sony Pictures Imageworks Arnold | [10.1145/3180495](https://doi.org/10.1145/3180495) |
| RenderMan: An Advanced Path-Tracing Architecture for Movie Rendering | [10.1145/3182162](https://doi.org/10.1145/3182162) |
| Manuka: A Batch-Shading Architecture for Spectral Path Tracing in Movie Production | [10.1145/3182161](https://doi.org/10.1145/3182161) |
| Arnold: A Brute-Force Production Path Tracer | [10.1145/3182160](https://doi.org/10.1145/3182160) |
| The Design and Evolution of Disney's Hyperion Renderer | [10.1145/3182159](https://doi.org/10.1145/3182159) |

## 4. Academic groups

| Name | URL | What's there |
|---|---|---|
| EPFL RGL (Wenzel Jakob) | https://rgl.epfl.ch/publications | Publications: Mitsuba, Dr.Jit, differentiable rendering, appearance |
| KIT Computer Graphics (Dachsbacher) | https://cg.ivd.kit.edu/english/publications.php | Publications: many-light rendering, sampling, GPU techniques |
| Charles University, Computer Graphics Group | https://cgg.mff.cuni.cz/publications/ | Publications: path guiding, light transport, participating media |
| Dartmouth Visual Computing Lab (Jarosz) | https://cs.dartmouth.edu/~wjarosz/ | Publications: ReSTIR, volumes, sampling *(unverified: Dartmouth served an outage page on the verification date)* |
| UCSD (Ravi Ramamoorthi) | https://cseweb.ucsd.edu/~ravir/ | Homepage and publications: sampling, real-time and neural rendering |
| Cornell Program of Computer Graphics | https://bowers.cornell.edu/computer-graphics | The old `graphics.cornell.edu` redirects here. See also [Steve Marschner](https://www.cs.cornell.edu/~srm/) |
| Utah Graphics Lab | https://graphics.cs.utah.edu/ | Lab site with research projects |
| Cem Yuksel (Utah) | https://www.cemyuksel.com/research/ | Research and papers: hair, sampling, real-time rendering |
| Saarland University Computer Graphics Lab (Slusallek) | https://graphics.cg.uni-saarland.de/ | Lab site: ray tracing and rendering research (the DFKI pages block automated fetch, so they are omitted) |
| TU Wien Rendering course | https://www.cg.tuwien.ac.at/courses/Rendering/VU/ | Course VU 193.183 (path tracing and more), with an [older lecture playlist](https://www.youtube.com/watch?v=5sY_hoh_IDc&list=PLmIqTlJ6KsE2yXzeq02hqCDpOdtj6n6A9) |

## 5. Open-access books and test scenes

| Name | URL | What's there |
|---|---|---|
| PBRT 4th edition | https://pbr-book.org/4ed/contents | Full text free online (3rd edition also at https://pbr-book.org/) |
| Ray Tracing Gems | https://link.springer.com/book/10.1007/978-1-4842-4427-2 | Free PDF from Apress/Springer; the book page is at https://www.realtimerendering.com/raytracinggems/ |
| Ray Tracing Gems II | https://link.springer.com/book/10.1007/978-1-4842-7185-8 | Open-access PDF (CC BY-NC-ND 4.0) |
| Ray Tracing in One Weekend series | https://raytracing.github.io/ | The three free books with source |
| Real-Time Rendering resources | https://www.realtimerendering.com/ | Ray tracing resources, book bibliography, portal |
| Bitterli rendering resources | https://benedikt-bitterli.me/resources/ | 30+ test scenes in Tungsten, Mitsuba 3 and pbrt-v4 formats (CC0 / CC BY mostly) |
| NVIDIA ORCA | https://developer.nvidia.com/orca | Open Research Content Archive: Bistro, Emerald Square, Sun Temple, Zero-Day, SpeedTree (marked "legacy", unsupported) |
| Disney Moana Island scene | https://disneyanimation.com/resources/moana-island-scene/ | Production-scale scene: base, animation, USD, pbrt and pbrt-v4 packages |

## 6. Open-source renderers and defining papers

| Name | Repo | Defining paper / notes |
|---|---|---|
| Mitsuba 3 | https://github.com/mitsuba-renderer/mitsuba3 | No standalone paper. Built on Dr.Jit ([10.1145/3528223.3530099](https://doi.org/10.1145/3528223.3530099), TOG 41(4) 2022), with Mitsuba 2 as predecessor ([10.1145/3355089.3356498](https://doi.org/10.1145/3355089.3356498), TOG 38(6) 2019). Site: https://www.mitsuba-renderer.org/ |
| pbrt-v4 | https://github.com/mmp/pbrt-v4 | The PBRT book, 4th edition (https://pbr-book.org/) |
| Falcor | https://github.com/NVIDIAGameWorks/Falcor | No paper; README asks for citation as "The Falcor Rendering Framework" (Kallweit et al., 2022) |
| MoonRay | https://github.com/dreamworksanimation/openmoonray | Redirects to `github.com/OpenMoonRay/openmoonray`. Paper: "Vectorized Production Path Tracing", HPG 2017 ([10.1145/3105762.3105768](https://doi.org/10.1145/3105762.3105768)) |
| Cycles | https://github.com/blender/cycles | Official mirror; site https://www.cycles-renderer.org/. No defining paper |
| Tungsten | https://github.com/tunabrain/tungsten | Bitterli's research renderer; no defining paper |
| Embree | https://github.com/RenderKit/embree | "Embree: A Kernel Framework for Efficient CPU Ray Tracing", TOG 33(4) 2014 ([10.1145/2601097.2601199](https://doi.org/10.1145/2601097.2601199)) |
| Open PGL | https://github.com/OpenPathGuidingLibrary/openpgl | Moved from `RenderKit/openpgl`. No library paper; it implements path guiding in the line of Müller et al., "Practical Path Guiding", CGF 36(4) 2017 ([10.1111/cgf.13227](https://doi.org/10.1111/cgf.13227)) |
| Open Image Denoise | https://github.com/RenderKit/oidn | No defining paper |
| OSL | https://github.com/AcademySoftwareFoundation/OpenShadingLanguage | "Open Shading Language", SIGGRAPH 2010 Talks ([10.1145/1837026.1837070](https://doi.org/10.1145/1837026.1837070)); see also SPI Arnold above |
| MaterialX | https://github.com/AcademySoftwareFoundation/MaterialX | Specification at https://materialx.org/; no defining paper |
| OptiX (closed source, SDK) | https://developer.nvidia.com/rtx/ray-tracing/optix | "OptiX: A General Purpose Ray Tracing Engine", TOG 29(4) 2010 ([10.1145/1778765.1778803](https://doi.org/10.1145/1778765.1778803)) |

Many-light sampling papers (DOIs confirmed through Crossref):

- Light trees: Conty Estévez & Kulla, "Importance Sampling of Many Lights with Adaptive Tree Splitting", PACMCGIT 1(2) 2018, [10.1145/3233305](https://doi.org/10.1145/3233305).
- Many-light resampling: Bitterli et al., "Spatiotemporal Reservoir Resampling" (ReSTIR), TOG 39(4) 2020, [10.1145/3386569.3392481](https://doi.org/10.1145/3386569.3392481).

---

Verified on 2026-11-23.
