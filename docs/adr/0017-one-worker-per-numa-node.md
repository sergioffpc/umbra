# One Worker per NUMA node

> **Amended by [ADR 0021](0021-plug-and-play-workers-in-a-named-cluster.md).** No script starts the Workers and there is no host list. The operator starts one Worker per NUMA node on each host and leaves it running, bound with `numactl --cpunodebind=N --membind=N` or, in a container, with `--cpuset-cpus` and `--cpuset-mems`. The rest of this ADR still applies.
>
> **Amended by [ADR 0026](0026-gpu-workers-from-the-first-milestone.md).** A machine runs one Worker per Render Device: one per NUMA node, plus one per GPU. On a machine with a GPU, the CPU Worker leaves one core for the GPU Worker's host process. The rest of this ADR still applies.

A machine runs one Worker per NUMA node, each bound to its node's cores and memory. In practice the R630 runs two Workers and every other target machine runs one. Decided in [Per-architecture tuning plan](https://github.com/sergioffpc/umbra/issues/48). This supersedes the rejection of "a Worker process per NUMA node" in [ADR 0008](0008-index-handle-arenas-and-allocation-free-rendering.md).

The R630 has two sockets and two NUMA nodes ([arc42 §7.1](../architecture/07-deployment-view.md#71-infrastructure)). With a single Worker, one thread builds the Scene and the BVH, and Linux's first-touch policy places all of them on node 0. Half the threads would then read every BVH node and triangle across the inter-socket link, which is the main risk to the R630's QS-6 target (75 %).

- **Launch.** `scripts/workers up` starts one Worker per NUMA node, as `numactl --cpunodebind=N --membind=N umbra worker …`. The operator's host list records each host's `numa_nodes`, so the R630 has 2 and the others have 1. Each Worker uses all logical CPUs of its node by default.
- **Replication.** Each Worker receives its own copy of the Scene from the Coordinator and builds its own BVH in its own node's memory. NUMA locality comes from the distributed design, with no NUMA code in the Worker.
- **Scheduling.** Nothing changes. The two Workers pull Work Units independently ([ADR 0009](0009-pull-based-pass-major-scheduling.md)), and the Film does not depend on how many Workers there are ([ADR 0004](0004-deterministic-film-for-a-fixed-render-job.md)).
- **Before distribution.** Until the milestone that introduces remote Workers, the only Worker is the Coordinator's local one. On the R630 it runs under `numactl --interleave=all`.

## Considered options

- **One Worker per machine with `numactl --interleave=all`**: rejected as the default. It needs no code and spreads pages evenly, but half the memory accesses stay remote. It is the baseline the two-Worker setup is measured against once.
- **A Scene and BVH replica per NUMA node inside one Worker, with one rayon pool per node**: deferred. It saves one Scene transfer and one connection, but it needs NUMA-aware allocation and pool placement in the Worker. It is reopened only if the R630 misses QS-6 with two Workers ([arc42 §8.14](../architecture/08-crosscutting-concepts.md#814-per-architecture-tuning)).
- **One Worker per CCX on the P620** (BIOS in NPS4 or "L3 as NUMA"): rejected. It would multiply the Scene transfers and BVH builds by four or eight for a small expected gain, because the Scene is read-only and every CCX already keeps its own hot copy in its L3. The P620's BIOS stays in NPS1.

## Consequences

- The R630's Scene replication costs twice the network traffic, about 2 × Scene size over 1 GbE, and the Coordinator tracks one more Worker.
- QS-6 on the R630 is measured over the machine's two Workers together.
- Status lines and the Render Job Report list the R630 as two Workers on one host.
