# ext

Public headers of the bridges to third-party libraries that need RTTI or
exceptions, one directory per bridge: `include/umbra/ext/<dependency>/`.

These headers expose only umbra types: they never include the library's
headers, and callers never see its types or exceptions. The exceptions are
the OpenUSD bridge's `usd/stage.h`, which hands the render delegate the
stage it opens, and the Embree bridge's `embree/owners.h`,
`embree/errors.h`, `embree/conversions.h` and `embree/nonblocking.h`,
which the CPU device builds and traces its BVHs with. See
[src/ext/README.md](../../../src/ext/README.md) for the rules.
