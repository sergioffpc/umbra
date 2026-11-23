# ext

Public headers of the bridges to third-party libraries that need RTTI or
exceptions, one directory per bridge: `include/umbra/ext/<dependency>/`.

These headers expose only umbra types: they never include the library's
headers, and callers never see its types or exceptions. See
[src/ext/README.md](../../../src/ext/README.md) for the rules.
