# Umbra

Umbra is a multicore, LAN-distributed physically based renderer following pbrt-v4. This glossary fixes the words used for its rendering and distribution concepts.

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

**Reference Scene**:
A Scene from the pinned pbrt-v4 scene collection that a milestone must render, and whose render the owner checks by eye. `umbra bench` measures it at a fixed resolution, Pixel Samples per pixel, seed and Tile size. Once listed, a Reference Scene is never removed.
_Avoid_: Benchmark scene, test scene, test asset

**Reference Scene Variant**:
A Reference Scene with one declared, minimal edit to its Scene File, such as a different integrator, camera, sampler, filter, film, texture or light, so that it exercises a feature no Reference Scene uses. pbrt-v4 renders it with the same edit when its speed is compared. It is listed, measured and checked like a Reference Scene.
_Avoid_: Modified scene, custom scene, test scene

**Render Job Report**:
The record a Render Job prints when it ends, finished or cancelled: how long each phase took, how fast Pixel Samples were computed, what each Worker did and how the work was scheduled. Only some of its measurements are compared between runs; the rest explain them.
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
A long-running process that joins the Render Jobs of its Cluster one at a time. While it is in a Render Job, it holds a replica of the Scene and renders the Work Units assigned to it on its Render Device. Between Render Jobs it holds no Scene. A machine runs one Worker per Render Device: one per NUMA node, plus one per GPU.
_Avoid_: Node, slave, client, agent, render thread

**Asset Cache**:
The directory on a Worker's host where Workers keep the assets they have received, each named by its content hash and bounded in size, so that a later Render Job sends a Worker only the assets it lacks. It holds asset bytes, never a Scene.
_Avoid_: Scene cache, local store, asset store

**Render Device**:
The hardware one Worker renders on: either all the cores of one NUMA node, or one GPU.
_Avoid_: Backend, accelerator, compute unit

**Local Worker**:
A Worker that the Coordinator starts on its own machine, so that one machine can render a Render Job alone: the one for the CPU runs inside the Coordinator's process, and the one for each GPU in a child process of it.
_Avoid_: Embedded worker, in-process node

**Worker Slot**:
One machine and one of its Render Devices, as a Worker reports them when it joins a Render Job. The Coordinator counts the failures of successive Workers in the same slot and refuses the slot after three in a row.
_Avoid_: Worker instance, seat, node

**Cluster**:
The Coordinators and Workers that share a Cluster Name. A Coordinator gives work only to Workers of its own Cluster, and an idle Worker joins whichever Coordinator of its Cluster announces a Render Job.
_Avoid_: Session, pool, farm, swarm

**Cluster Name**:
The name that defines a Cluster, given to every Coordinator and Worker in it, and `default` unless set. It guards against mistakes of operation, not attacks: it is a label, not a secret.
_Avoid_: Cookie, session identifier, cluster key, namespace

**Operator**:
The person who runs Render Jobs from their own machine, which then hosts the Coordinator and receives the final image, and who keeps the Workers of their Cluster running.
_Avoid_: User, admin, client

**Build Identity**:
The source commit a Coordinator or Worker binary was built from, and whether its tree had uncommitted changes. A Render Job admits only Workers whose commit matches the Coordinator's.
_Avoid_: Version, build number, release

### Debugging

**Pixel Trace**:
The detailed record of how one Pixel Sample was computed, from camera sample to Film: its path vertices, the light and BSDF samples taken, their contributions and weights, and how the result was filtered and converted for the Film. It is never captured while a Render Job runs; it is obtained by computing that Pixel Sample again. Inspecting a pixel yields a set of Pixel Traces.
_Avoid_: Path log, debug ray, pixel dump

**Reproduction Record**:
What the Render Job Report carries so that any of the image's Pixel Samples can be computed again, exactly on the machine that rendered it: the Scene File path with a content hash of it, its includes and its assets, the seed, every Render Job option except the output path, and the Build Identity of the Umbra that rendered it.
_Avoid_: Job manifest, render metadata, provenance

**Anomalous Pixel Sample**:
A Pixel Sample whose radiance is not a number, infinite or negative. It is left out of the Film and reported, so its Pixel Trace can be inspected.
_Avoid_: Bad sample, NaN sample (a firefly is a valid, merely very bright, Pixel Sample)

### Development

**Aggressive Optimisation**:
A change made only for speed that departs from the readable, book-faithful implementation. It is admitted only with a benchmark showing an end-to-end gain, behind a simple contract, and tested against the readable reference it keeps.
_Avoid_: Optimisation (unqualified), fast path, hack
