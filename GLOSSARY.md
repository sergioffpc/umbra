# Umbra

Umbra is a distributed spectral path tracer for production rendering. This glossary fixes the language of its architecture.

## Distribution

**Render node**:
One machine taking part in rendering a frame; it hosts one or more executors.
_Avoid_: Worker, host, server

**Executor**:
A processor with its own memory budget that holds shards and traces and shades the rays that visit them: the host CPU, or one GPU.
_Avoid_: Device, backend, worker

**Shard**:
A self-contained slice of the scene (its geometry, acceleration data, shading data and the textures it references) that can be placed on one or more executors.
_Avoid_: Partition, chunk, brick

**Placement plan**:
The per-frame assignment of every shard to the executors that hold it.
_Avoid_: Distribution map, layout

**Full replication**:
A placement plan in which every executor holds every shard; no ray ever leaves the executor it was born on.
_Avoid_: Replicated mode, tile mode

**Selective replication**:
A placement plan in which hot shards are held by every executor and the remaining shards are spread across executors.
_Avoid_: Hybrid mode

**Ray migration**:
Sending a ray together with its path state to an executor that holds the shard the ray must visit next, on the same render node or another.
_Avoid_: Ray forwarding, ray shipping

## Quality

**Statistical reproducibility**:
Every execution topology (node count, thread count, node failures) produces an image with the same expected value and no worse variance.
_Avoid_: Determinism, bit-exactness
