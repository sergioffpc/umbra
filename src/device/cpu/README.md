# device/cpu

The CPU device, `umbra::device::cpu`: one per NUMA node, which renders from
Embree's BVHs, built in the node's own memory
(`docs/adr/0004-render-devices.md`,
`docs/adr/0013-devices-render-from-their-own-memory.md`). It implements the
`Device` interface of `umbra::device`, which never depends on it.

It calls Embree's API directly, with the handles, errors and conversions of
the Embree bridge, `umbra::ext::embree`, and calls on its hot path only the
functions the bridge redeclares nonblocking
(`docs/adr/0017-ext-is-a-thin-layer-over-each-dependency.md`).

Public headers (`include/umbra/device/cpu/`):

- `cpu_device.h`: `CpuDevice`, the device of a NUMA node, and the rays it
  intersects with its BVHs.

Sources (`src/device/cpu/`):

- `cpu_device.cc`: the device, which runs its commit and its renders on
  its arena.
- `arena.h`: its oneTBB arena, bound to the node, whose threads bind their
  memory to the node.
- `held.h`: what it holds of the commits, in the id tables of
  `umbra/device/id_table.h`: its copies, its BVHs and its top level.
- `commit.h`: its commit: the ids of the change set applied with
  `ApplyIds`, the copies and the BVHs built, refitted or rebuilt, and the
  top level rebuilt. `commit_log.h` logs what it built.
- `copies.h`: its own copies of the prims a change set gives, in the node's
  memory.
- `bvh.h`: its BVHs: one per prototype, one per light with a shape, and the
  top level that instances them.
- `trace.h`: the intersection of a ray with its top level, on the hot path.
- `render.h`: its film and sampler's cache, and the render of the tiles it
  takes, a batch at a time, `[[clang::nonblocking]]` from each tile it
  takes to its copy into the render buffers.
