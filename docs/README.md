# Documentation

Umbra's design is written down in four places, each with one job:

- [`GLOSSARY.md`](../GLOSSARY.md): the domain vocabulary. Every other document uses its terms.
- [`architecture/`](architecture/): the architecture as it stands, in the twelve arc42 sections listed below. Section 5 describes how the crates fit together.
- [`adr/`](adr/): the hard-to-reverse decisions, one file each, in the order they were taken. Section 9 indexes them.
- `crates/<name>/README.md`: each crate from the inside, as it is built: its modules, its own design decisions, what to know when using it, its performance and its tests. It is also the crate's rustdoc documentation. See [Crate READMEs](../CONTRIBUTING.md#crate-readmes).

When an ADR and an arc42 section disagree, the later ADR is the decision and the section is the error to fix.

## Architecture (arc42)

1. [Introduction and goals](architecture/01-introduction-and-goals.md): requirements, quality goals, stakeholders
2. [Architecture constraints](architecture/02-architecture-constraints.md)
3. [Context and scope](architecture/03-context-and-scope.md): what Umbra talks to, and what is out of scope
4. [Solution strategy](architecture/04-solution-strategy.md): the milestone roadmap and the fundamental decisions
5. [Building block view](architecture/05-building-block-view.md): the crates and their interfaces
6. [Runtime view](architecture/06-runtime-view.md): a Render Job from start-up to image, Workers joining, failures and cancellation
7. [Deployment view](architecture/07-deployment-view.md): the target machines, release and development builds, running Workers and Render Jobs
8. [Crosscutting concepts](architecture/08-crosscutting-concepts.md): numerics, determinism, scheduling, wire protocol, tuning, image output
9. [Architecture decisions](architecture/09-architecture-decisions.md): the ADR index
10. [Quality requirements](architecture/10-quality-requirements.md): the measurable quality scenarios
11. [Risks and technical debt](architecture/11-risks-and-technical-debt.md)
12. [Glossary](architecture/12-glossary.md): points to `GLOSSARY.md`

## Background

Interactive explainers live next to the crate they explain, in its `docs/` folder. Open them in a browser. Start with the project-wide one:

- [`umbra-guide.html`](umbra-guide.html): the map of the whole project. What each crate is for and how much of it is built, a Render Job from command to image, one Pixel Sample through the core on the CPU and the GPU, the rules every crate follows, Workers and Clusters, the milestones, and a search over every guide section, ADR, arc42 section and glossary term. It links to the crate guides below instead of repeating them.

The crate guides:

- [`umbra-math/docs/math-guide.html`](../crates/umbra-math/docs/math-guide.html): `f32` rounding, error bounds, intervals, compensated arithmetic, normals, frames, octahedral vectors, the perspective projection, and what the C library's functions cost on each benchmark machine and the GPU.
- [`umbra-camera/docs/camera-guide.html`](../crates/umbra-camera/docs/camera-guide.html): how a Pixel Sample becomes a ray and radiance becomes sensor RGB: coordinate spaces, the pinhole and thin-lens camera, importance-sampled pixel filters, the camera sample and the pixel sensor, with diagrams for readers new to the maths.
- [`umbra-sampling/docs/sampling-guide.html`](../crates/umbra-sampling/docs/sampling-guide.html): Monte Carlo, hashing and PCG32, low-discrepancy and Halton sampling, the inversion method, piecewise-constant distributions and the warps to discs, spheres, triangles and lobes.
- [`umbra-spectrum/docs/spectrum-guide.html`](../crates/umbra-spectrum/docs/spectrum-guide.html): light as a spectrum, sampled wavelengths, CIE XYZ, colour spaces and turning RGB into spectra.
- [`umbra-geometry/docs/geometry-guide.html`](../crates/umbra-geometry/docs/geometry-guide.html): rays, bounding boxes, ray–sphere intersection with floating-point care, and handles into arenas.
- [`umbra-integrators/docs/integrators-guide.html`](../crates/umbra-integrators/docs/integrators-guide.html): the rendering equation, evaluating a Pixel Sample, recorders for Pixel Traces, and the path-tracing theory still to come.
- [`umbra-scene/docs/scene-guide.html`](../crates/umbra-scene/docs/scene-guide.html): building a Scene, arenas and handles, the scene view and Render Job Options.
- [`umbra-render/docs/render-guide.html`](../crates/umbra-render/docs/render-guide.html): Tiles, Passes and Work Units, accumulating the Film, floating-point merge order and determinism.
- [`umbra-gpu/docs/gpu-guide.html`](../crates/umbra-gpu/docs/gpu-guide.html): GPUs for ray tracing, OptiX programs and the shader binding table, and the FFI boundary from Rust.
- [`umbra-kernels/docs/kernels-guide.html`](../crates/umbra-kernels/docs/kernels-guide.html): the core compiled for the GPU, kernels and launches, and the math kernels tested against the CPU.

## For contributors and agents

- [`CONTRIBUTING.md`](../CONTRIBUTING.md): branching, commits, checks and lint rules.
- [`agents/`](agents/): how coding agents use the issue tracker, triage labels and domain docs.
