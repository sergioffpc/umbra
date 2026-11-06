# Umbra

Umbra is a CPU-only, multicore, LAN-distributed physically based renderer following pbrt-v4. This glossary fixes the words used for its rendering and distribution concepts.

Rendering terminology (Integrator, Sampler, Camera, Shape, Primitive, BSDF, Light, Medium, Spectrum, Texture, ...) follows *Physically Based Rendering: From Theory to Implementation*, 4th edition, unchanged. Only terms specific to Umbra, or pbrt-v4 terms whose meaning Umbra narrows or changes, are listed here.

## Language

### Jobs

**Render Job**:
A request to render one Scene through one camera into one image, from submission to the final image.
_Avoid_: Job, task, render, frame

**Scene**:
The complete, validated description of what is rendered: geometry, materials, lights, media, camera and render settings, independent of the file format it was read from. Every Worker holds an identical replica.
_Avoid_: World, model

**Scene File**:
The pbrt-v4 `.pbrt` file, with the files it includes, from which the Coordinator builds a Scene.
_Avoid_: Scene description, input file, scene source

**Reference Scene**:
A Scene from the pinned pbrt-v4 scene collection, with fixed resolution, Pixel Samples per pixel, seed and Tile size, used to measure performance. Once listed, a Reference Scene is never removed.
_Avoid_: Benchmark scene, test scene, test asset

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
A process on one machine that holds a replica of the Scene and renders the Work Units assigned to it using all of that machine's cores.
_Avoid_: Node, slave, client, agent, render thread

### Debugging

**Pixel Trace**:
The detailed record of how one Pixel Sample was computed, from camera sample to Film: its path vertices, the light and BSDF samples taken, their contributions and weights, and how the result was filtered and converted for the Film. It is never captured while a Render Job runs; it is obtained by computing that Pixel Sample again. Inspecting a pixel yields a set of Pixel Traces.
_Avoid_: Path log, debug ray, pixel dump

**Reproduction Record**:
What the final image carries so that any of its Pixel Samples can be computed again exactly: the Scene File and its content, the seed, the Render Job options and the version of Umbra that rendered it.
_Avoid_: Job manifest, render metadata, provenance

**Anomalous Pixel Sample**:
A Pixel Sample whose radiance is not a number, infinite or negative. It is left out of the Film and reported, so its Pixel Trace can be inspected.
_Avoid_: Bad sample, NaN sample (a firefly is a valid, merely very bright, Pixel Sample)
