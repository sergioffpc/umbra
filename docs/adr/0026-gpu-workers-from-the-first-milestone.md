# GPU Workers from the first milestone

The GPU joins Umbra from `v0.1.0`, not after `v1.0.0`. Every milestone delivers its features on the CPU and on the GPU, and validates its Reference Scenes on both. Decided with the owner on 2026-11-11. This supersedes [ADR 0024](0024-a-gpu-backend-on-the-roadmap.md). It amends [ADR 0005](0005-chiseled-release-container-image.md), [ADR 0009](0009-pull-based-pass-major-scheduling.md), [ADR 0010](0010-quic-transport-between-coordinator-and-workers.md), [ADR 0014](0014-gpu-ready-no-std-rendering-core.md), [ADR 0017](0017-one-worker-per-numa-node.md) and [ADR 0019](0019-milestones-defined-by-the-reference-scenes-they-unlock.md).

ADR 0024 left the GPU for after the CPU milestones, with only the readiness rules of ADR 0014 in the meantime. Those rules are checked by review, and nothing would run on a GPU for fifteen milestones. The riskiest parts of a GPU backend would surface at the end: the toolchain, OptiX from Rust, the data layout shared with the host, divergence in `match` dispatch, and the wavefront driver. Building the GPU path from the first milestone exposes them while the core is still small.

- **Scope.** Every milestone adds its features on both Render Devices and validates its Reference Scenes on both. The exception is the integrators pbrt-v4 runs only on the CPU: `bdpt`, `sppm` and `mlt` stay CPU-only, and their Reference Scenes are validated on the CPU only.
- **A GPU is a Worker.** A machine runs one Worker per Render Device: one per NUMA node ([ADR 0017](0017-one-worker-per-numa-node.md)) and one per GPU. A Worker Slot is a machine and one of its Render Devices. A GPU Worker pulls Work Units, fails and is reassigned like any other, so scheduling needs no speed model and a CUDA failure takes down that Worker only.
- **Capacity, not decomposition.** Tile size and Pixel Samples per Pass stay as [ADR 0009](0009-pull-based-pass-major-scheduling.md) sets them. Each Worker declares how many Work Units it keeps in flight. A CPU Worker keeps about two per eight threads; a GPU Worker keeps enough to fill the GPU, about 16 to start with and tuned by benchmark, and renders them as one wavefront batch.
- **Ray intersection with OptiX.** From `v0.1.0`, a GPU Worker traces rays with OptiX on the RT cores. Triangles use the hardware intersection; other Shapes are OptiX custom intersection programs that call the core's code. The core's BVH is for the CPU only.
- **Local Workers.** `umbra render` starts one Local Worker per Render Device of its machine: the CPU one in the Coordinator's process, and each GPU one in a child process that connects back over QUIC like a remote Worker. A crash of the GPU driver then loses that child's Work Units, not the Film. `--render-devices cpu,gpu` (both by default) restricts which ones run; it is a Coordinator option, not a Render Job Option, because it does not change the expected image. With no usable GPU, the default falls back to the CPU with one warning, and an explicit `gpu` is an error.
- **Distribution moves into `v0.1.0`.** The GPU child needs the QUIC transport and reassignment on disconnect, so the whole former `v0.2.0 — Distribution` milestone (wire protocol, heartbeats, speculative tail, Build Identity admission, discovery, remote Workers) joins `v0.1.0`. Later milestones are renumbered down by one.
- **Statistical equality only.** A GPU Worker promises statistical equality with the CPU ([ADR 0013](0013-statistical-validation-against-a-published-pbrt-v4-reference.md)), even on one machine: it sums Pixel Samples in whatever order its wavefront produces them. Each Pixel Sample is still a pure function of its inputs, because pbrt-v4's samplers are deterministic. Golden Film Hashes, invariance tests and Pixel Traces stay CPU-only; to debug a GPU image, the Operator renders the same Render Job on the CPU and traces it.
- **Launch.** One binary and one release image serve both Render Devices. A remote Worker takes `--render-device cpu` (the default) or `--render-device gpu:N`. In a container a GPU Worker runs with `docker run --gpus device=N` and the NVIDIA Container Toolkit, which mounts the host driver's `libcuda` and `libnvoptix`; the image ships nothing from NVIDIA. On a machine with a GPU, the CPU Worker is bound to all cores but one, which the GPU Worker's host process uses.
- **Testing.** CI compiles the kernels but has no GPU. The GPU tests (OptiX intersection against the core on the CPU, the accuracy of the GPU math backend, a small render, and `umbra validate`) run on the P620 through a script, and each milestone's closing validation issue runs it.
- **Order of work.** `v0.1.0` starts with a tracer bullet: the pinned nightly toolchain ([ADR 0027](0027-a-pinned-nightly-toolchain.md)), the `nvptx64` backend of `umbra-math`, and `umbra-kernels` and `umbra-gpu` tracing one triangle and one custom-intersected sphere through OptiX back to the host. After it, each feature lands on both Render Devices in the same issue or in a sibling issue.

## Considered options

- **Keep ADR 0024: the GPU after `v1.0.0`**: rejected. The readiness rules would go untested for fifteen milestones.
- **Compilation only**: build the core for `nvptx64` in CI, with no host or kernels. Rejected: it catches compile errors but not layout, divergence, OptiX or driver problems.
- **A GPU skeleton in `v0.1.0`, then no feature parity until `v1.0.0`**: rejected. The gap would grow back milestone by milestone.
- **A GPU as a device inside the CPU Worker**: rejected. The Worker would need its own scheduler between devices, and a GPU failure would take its CPU work down with it.
- **The GPU Local Worker inside the Coordinator's process**: rejected by the owner. It would have kept QUIC in its own milestone, but a driver crash would lose the Film.
- **Only the transport and reassignment in `v0.1.0`, the rest of Distribution in `v0.2.0`**: rejected by the owner in favour of one milestone.
- **Larger Work Units for the GPU, or larger defaults for everyone**: rejected. The first breaks the fixed decomposition of ADR 0009; the second worsens the tail and the work lost on a CPU failure.
- **A software BVH on the GPU first, OptiX later**: rejected. It postpones the riskiest part and builds a path that would be thrown away.
- **Deterministic GPU results with an ordered reduction**: rejected. It costs memory and a reduction kernel for hashes that no automated test would check.
- **A self-hosted GPU runner on the P620 for CI**: rejected by the owner. GPU regressions surface at the latest when a milestone closes.
- **A separate GPU image, or GPU Workers outside containers**: rejected. ADR 0025 just reduced the release to one image.

## Consequences

- `v0.1.0` becomes much larger: it holds First light, all of Distribution and the GPU path.
- The P620 runs two Workers. Status lines and the Render Job Report list a GPU Worker like any other.
- A Cluster with a GPU produces images that differ from a CPU-only Cluster by noise, as [ADR 0022](0022-determinism-on-one-machine-statistical-equality-across-machines.md) already allows.
- A GPU regression introduced in a pull request may go unnoticed until the milestone's closing validation.
- Two facts are checked in the tracer bullet: the license of the OptiX SDK headers that the device bindings are written against, and the `NVIDIA_DRIVER_CAPABILITIES` OptiX needs in a container.
