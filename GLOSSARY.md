# Umbra

Umbra is a CPU-only, multicore, LAN-distributed physically based renderer following pbrt-v4. This glossary fixes the words used for its rendering and distribution concepts.

Rendering terminology (Integrator, Sampler, Camera, Shape, Primitive, BSDF, Light, Medium, Spectrum, Texture, ...) follows _Physically Based Rendering: From Theory to Implementation_, 4th edition, unchanged. Only terms specific to Umbra, or pbrt-v4 terms whose meaning Umbra narrows or changes, are listed here.

## Language

### Jobs

**Render Job**:
A request to render one Scene through one camera into one image, from submission to the final image.
_Avoid_: Job, task, render, frame

**Render Job Options**:
The settings of a Render Job given at submission rather than in its Scene File, such as Tile size, Pixel Samples per Pass, seed and output path, including pbrt-v4-style overrides of the Scene File.
_Avoid_: Job settings, config, parameters

**Scene**:
The complete, validated description of what is rendered: geometry, materials, lights, media, camera and render settings, independent of the file format it was read from. Every Worker holds an identical replica.
_Avoid_: World, model

**Scene File**:
The pbrt-v4 `.pbrt` file, with the files it includes, from which the Coordinator builds a Scene.
_Avoid_: Scene description, input file, scene source

**Diagnostics**:
The complete list of reasons, each located in its Scene File, why a Scene File cannot be rendered. A Render Job with any Diagnostics is refused before any Work Unit exists.
_Avoid_: Parse errors, validation errors, warnings

**Reference Scene**:
A Scene from the pinned pbrt-v4 scene collection, used both to measure performance and to check correctness against pbrt-v4, with a fixed resolution, Pixel Samples per pixel, seed (seeds 0–15 for validation) and Tile size for each use. Once listed, a Reference Scene is never removed.
_Avoid_: Benchmark scene, test scene, test asset

**Reference Scene Variant**:
A Reference Scene with one declared, minimal edit to its Scene File, such as a different integrator, camera, sampler, filter, film, texture or light. The same edit is applied before pbrt-v4 renders it, so it exercises a feature no Reference Scene uses while pbrt-v4 stays the judge. It is listed, measured and validated like a Reference Scene.
_Avoid_: Modified scene, custom scene, test scene

**Reference Image**:
The converged pbrt-v4 rendering of a Reference Scene, with far more Pixel Samples per pixel than the Reference Scene fixes, against which the correctness of Umbra's rendering is judged.
_Avoid_: Ground truth, golden image, reference render

**Render Job Report**:
The structured record a Render Job leaves when it finishes: how long each phase took, how fast Pixel Samples were computed, what each Worker did and how the work was scheduled. Only some of its measurements are compared between runs; the rest explain them.
_Avoid_: Benchmark report, stats file, metrics dump

### Image decomposition

**Tile**:
A rectangular region of pixels of the image, independent of how many samples are taken in it.
_Avoid_: Bucket, block, chunk, region

**Pass**:
A contiguous range of Pixel Sample indices, shared by the whole Render Job; pass 2 means the same sample range in every pixel.
_Avoid_: Iteration, round, progression

**Pixel Sample**:
One radiance estimate for one pixel at one sample index; the pair (pixel, sample index) identifies it uniquely within a Render Job. "Sample" is never used unqualified: every kind is named (Pixel Sample, light sample, BSDF sample, camera sample), the latter following pbrt-v4.
_Avoid_: Sample (unqualified), ray, path, spp

**Work Unit**:
One Tile rendered for one Pass: the unit the Coordinator assigns to a Worker, and reassigns if that Worker fails.
_Avoid_: Task, job, chunk, assignment, tile pass

**Work Result**:
The unnormalised pixel contributions produced by exactly one Work Unit; at most one is accepted per Work Unit.
_Avoid_: Tile result, partial, fragment, partial film

**Film**:
The single sensor and pixel accumulator of a Render Job, from which the final image is produced; Work Results are merged into it.
_Avoid_: Framebuffer, canvas, image buffer, worker film

### Distribution

**Coordinator**:
The single process that accepts a Render Job, owns its Film, assigns Work Units to Workers and merges their Work Results.
_Avoid_: Master, server, leader, scheduler

**Worker**:
A process that holds a replica of the Scene and renders the Work Units assigned to it using all the cores of its NUMA node. A machine runs one Worker per NUMA node, so usually exactly one.
_Avoid_: Node, slave, client, agent, render thread

**Local Worker**:
A Worker that runs inside the Coordinator's own process, so that one machine can render a Render Job alone.
_Avoid_: Embedded worker, in-process node

**Worker Slot**:
One place in a Session where a Worker runs: one machine and one of its NUMA nodes. When its Worker fails, a new Worker takes the same slot, a bounded number of times.
_Avoid_: Worker instance, seat, node

**Session**:
One run of a Coordinator and the Workers started with it for a single Render Job; Workers do not outlive it, and a Worker that presents another Session's identifier is refused.
_Avoid_: Cluster, run, deployment

**Operator**:
The person who starts a Session from their own machine, which then hosts the Coordinator and receives the final image.
_Avoid_: User, admin, client

**Build Identity**:
The source commit a Coordinator or Worker binary was built from, whether its tree had uncommitted changes, and the CPU it was compiled for. A Render Job admits only Workers whose commit matches the Coordinator's.
_Avoid_: Version, build number, release

### Debugging

**Pixel Trace**:
The detailed record of how one Pixel Sample was computed, from camera sample to Film: its path vertices, the light and BSDF samples taken, their contributions and weights, and how the result was filtered and converted for the Film. It is never captured while a Render Job runs; it is obtained by computing that Pixel Sample again. Inspecting a pixel yields a set of Pixel Traces.
_Avoid_: Path log, debug ray, pixel dump

**Reproduction Record**:
What the final image carries so that any of its Pixel Samples can be computed again exactly: the Scene File path with a content hash of it, its includes and its assets, the seed, every Render Job option except the output path, and the Build Identity of the Umbra that rendered it.
_Avoid_: Job manifest, render metadata, provenance

**Anomalous Pixel Sample**:
A Pixel Sample whose radiance is not a number, infinite or negative. It is left out of the Film and reported, so its Pixel Trace can be inspected.
_Avoid_: Bad sample, NaN sample (a firefly is a valid, merely very bright, Pixel Sample)

### Development

**Aggressive Optimisation**:
A change made only for speed that departs from the readable, book-faithful implementation. It is admitted only with a benchmark showing an end-to-end gain, behind a simple contract, and tested against the readable reference it keeps.
_Avoid_: Optimisation (unqualified), fast path, hack

**Golden Film Hash**:
The committed hash of the Film a Reference Scene produces at minimal settings; a change to it means a change in rendered pixels.
_Avoid_: Golden image, snapshot, expected output
