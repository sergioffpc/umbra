# Sharded scene with selective replication

Umbra must render scenes whose geometry exceeds the memory of one render node, and must also split a frame across nodes when the scene fits everywhere. We chose a single mechanism for both: the scene is always cut into shards, and a per-frame placement plan decides which render nodes hold each shard. Full replication (every node holds everything, no ray migrates) is the degenerate plan. Selective replication keeps hot shards (camera-visible geometry, lights, small heavily-instanced prototypes) on every node and spreads the rest, so that only rays reaching cold geometry migrate. When a shard lives on several nodes, a migrating ray goes to the least loaded one. The placement plan is computed automatically from per-node memory budgets and a heat estimate, and RenderSettings can override it per shard.

## Considered options

- **Replication only:** simpler, but it limits scene size to the RAM of the smallest node.
- **Contracts that allow sharding, replicated implementation first:** rejected because a later move to sharding would still force a rewrite of the core around it.
- **Separate tile mode and sharded mode:** rejected because it means two code paths with different failure behaviour.

## Consequences

- **Wavefront core.** A path is a data object that moves between queues: trace, shade, send to a shard. A ray cannot block on the network in the middle of a traversal, so the core is wavefront from the start.
- **Local acceleration only.** Embree, or any single-address-space BVH, accelerates only the shards local to a node. Above it sits a replicated top-level structure of shard bounds and owners.
- **OSL `trace`.** `RendererServices::trace` is synchronous, so it cannot reach geometry on another node and needs its own policy.
- **Shading runs on the owner.** Hit shading runs on a node that holds the hit shard.
