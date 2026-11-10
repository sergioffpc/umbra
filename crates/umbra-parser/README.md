# umbra-parser

The adapter from a pbrt-v4 Scene File to a Scene: it tokenises the `.pbrt`
file with locations, resolves its includes and assets as pbrt-v4 does,
validates it strictly, and returns either the Scene with its Render Job
Options and the content hash of the Scene File, its includes and its assets,
or an Invalid Scene File with every reason, with nothing built (ADR 0003). `parse` is its one
public function.

It is a `std` crate, outside the core. It depends on `umbra-scene`, and only
the `umbra` binary depends on it: `umbra-cluster` receives an already parsed
Scene (arc42 §5.1). Its contract is sketched in arc42 §5.3.

> **Status:** not implemented yet. Its first code arrives with
> [#78 umbra render end to end on a camera-only Scene File](https://github.com/sergioffpc/umbra/issues/78).
