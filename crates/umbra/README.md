# umbra

The `umbra` binary: the `render`, `worker`, `trace` and `bench` subcommands,
and how they put the other crates together. It parses the Scene File with
`umbra-parser`, applies the command-line overrides, builds the Reproduction
Record, and calls `umbra-cluster`, or `umbra-render` for `trace`. The
statistics of `bench` are modules of the binary.

It is the root of the workspace graph: it depends on `umbra-parser`,
`umbra-render` and `umbra-cluster`, and nothing depends on it (arc42 §5.1).
Its contract is sketched in arc42 §5.3. What Umbra is, how to build it and
how to run Workers on several machines are in the
[repository's README](https://github.com/sergioffpc/umbra#readme).

Its integration tests are the cross-crate tests of arc42 §5.4, the crate
that sees every other: so far the counting-allocator test of ADR 0008,
`tests/allocation.rs`, which renders Work Units with N and 2N Pixel Samples
and requires the same number of heap allocations.

> **Status:** not implemented yet. Its first code arrives with
> [#78 umbra render end to end on a camera-only Scene File](https://github.com/sergioffpc/umbra/issues/78).
