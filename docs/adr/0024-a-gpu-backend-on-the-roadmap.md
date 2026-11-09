# A GPU backend on the roadmap

The NVIDIA RTX 5070 Ti in the P620 becomes part of Umbra's scope, as the goal of a milestone after the CPU milestones. CPU work comes first. Decided with the owner on 2026-11-09. This amends [ADR 0014](0014-gpu-ready-no-std-rendering-core.md), whose readiness rules still apply, and moves the GPU from out of scope (arc42 §2.2, §3.3) to the roadmap (arc42 §4.1).

The GPU is the most powerful hardware the owner has, and the project's priority is to get the most out of it. The main obstacle was bit-identity with the CPU. A GPU has its own transcendentals, FMA contraction, reduction order and ray-tracing hardware, so matching the CPU bit for bit would have meant giving up the RT cores. [ADR 0022](0022-determinism-on-one-machine-statistical-equality-across-machines.md) dropped bit-identity across machines, and with it that obstacle.

- **Statistical equality with the CPU.** A GPU backend is correct when its images are statistically equal to pbrt-v4's under [ADR 0013](0013-statistical-validation-against-a-published-pbrt-v4-reference.md), as the CPU's are. It may use RT cores, hardware transcendentals and the GPU's own reduction order.
- **Pixel Traces stay on the CPU.** `umbra trace` re-executes on the CPU, so it does not reproduce a Pixel Sample rendered on the GPU. To debug a GPU image, the operator renders the same Render Job locally on the CPU and traces that render.
- **Decided with the milestone, not now:** the toolchain (CUDA and OptiX are the obvious candidates for an NVIDIA-only GPU), the wavefront driver, the SoA layout, whether a GPU joins a Render Job as one more Worker beside CPU Workers, and which Reference Scenes the milestone covers. The deferred list of ADR 0014 stands.

## Considered options

- **GPU first**: rejected. The CPU milestones build the integrators, the Scene and the validation that a GPU backend would be tested against.
- **GPU out of scope**: rejected. It would leave the strongest hardware of the Cluster unused.

## Consequences

- The `no_std` core rules of ADR 0014 now serve a planned port rather than a hypothetical one.
- A Cluster with a GPU produces images that differ from a CPU-only Cluster by noise. Under ADR 0022 that is acceptable.
