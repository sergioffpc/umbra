# Pull-based, Pass-major scheduling with speculative tail

The Coordinator schedules a Render Job by letting Workers pull Work Units in Pass-major order, inside a bounded window, and by duplicating the oldest Work Units in flight once the queue is empty ([Work decomposition and scheduling](https://github.com/sergioffpc/umbra/issues/23)). The target machines differ widely in speed (M2 Pro, Threadripper PRO 3975WX, 2 × Xeon E5-2690 v4, §7.1), there is no live view, and [ADR 0004](0004-deterministic-film-for-a-fixed-render-job.md) requires every Tile's Work Results to be merged in Pass order with early arrivals held on the Coordinator.

- **Fixed decomposition.** Tile size and Pixel Samples per Pass are explicit Render Job options, 32 × 32 pixels and 64 Pixel Samples by default. They are never derived from the cluster, because the Film is only bit-identical for identical options.
- **Pull.** A Worker requests Work Units whenever it has free capacity and keeps about two in flight per eight threads to hide network latency. Load balancing across heterogeneous machines follows from pulling; the Coordinator keeps no speed model.
- **Pass-major order.** Work Units are handed out Pass by Pass, and within a Pass in ascending Tile index (raster order). The Coordinator never assigns Pass p of a Tile before Pass p − 2 of that Tile has been merged, so at most one Work Result per Tile waits in the early-arrival buffer.
- **Failure.** A Worker is dead when its connection closes or after 5 s without an application heartbeat (sent every second); both values are Coordinator options because they do not affect the Film. There is no per-Work-Unit timeout, because Work Unit cost varies too much with the Scene. A dead Worker's Work Units in flight go to the front of the queue, since they are the ones blocking ordered merges. A reconnecting Worker joins as a new Worker. A late Work Result from a Worker declared dead is accepted if it is the first for its Work Unit, and is otherwise hash-compared and discarded. With no Workers left, the Render Job waits.
- **Speculative tail.** When the queue is empty and a Worker has free capacity, the Coordinator assigns it a copy of the oldest Work Unit still in flight, at most one copy per Work Unit. The first Work Result wins; the other is cancelled, or hash-compared and discarded if it arrives anyway. On by default.
- **Local Worker.** The Coordinator can host a Worker in its own process, reached through an in-memory transport behind the same transport seam ([ADR 0006](0006-closed-enum-dispatch-for-rendering-interfaces.md)), so rendering on one machine is one command and the scheduler is tested without a network. It can be turned off.

Inside a Worker, Work Units in flight run concurrently on one rayon pool and each splits its rows with `par_iter`; work stealing balances the M2's performance and efficiency cores. Thread pinning, per-NUMA-node Scene replicas and per-core-type tuning are aggressive optimisations, decided with the per-architecture tuning plan.

## Considered options

- **One Pass with every Pixel Sample**: rejected. Work Units become arbitrarily large at high Pixel Sample counts, so a failure loses much work and the tail grows.
- **Pass size chosen by the Coordinator from the cluster size**: rejected. The same Scene would produce different Films on different clusters.
- **Push with a per-Worker speed estimate**: rejected. Pulling balances the load without a model that has to be calibrated per Scene.
- **Tile-major order**: rejected. Passes of one Tile run in parallel, so one slow Pass holds back every later Pass of that Tile and the early-arrival buffer grows.
- **Per-Work-Unit timeouts**: rejected. A timeout that fits a simple Scene falsely fails a slow Worker on a complex one.
- **No speculative tail**: rejected. Idle machines would wait on the slowest Worker for the last Work Units, and duplicates are already safe under ADR 0004.
- **A custom thread pool with core pinning, or one Work Unit per thread**: rejected as the default. The first is unproven complexity; the second needs as many Work Units in flight as threads and leaves the tail single-threaded.

## Consequences

- Speculative copies and reassignments produce duplicate Work Results routinely, so the per-Work-Result hash comparison of ADR 0004 is exercised in every Render Job, not only after failures.
- The wire protocol needs a cancel message for a Work Unit and progress in the heartbeat.
- The early-arrival buffer holds at most one Work Result per Tile. Splat accumulators the size of the image (BDPT, light tracing) still multiply that by the image size, which the BDPT design must address.
