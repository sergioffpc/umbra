# umbra-render

Rendering in one process: the `Renderer` that turns a Work Unit into a Work
Result and re-runs Pixel Samples as Pixel Traces through the same code path,
the Film that merges Work Results as they arrive (ADR 0022), and the EXR final
image that carries its Reproduction Record (ADR 0020).

It is a `std` crate, outside the core. It depends on `umbra-integrators` and
`umbra-scene`, and `umbra-cluster` and the `umbra` binary depend on it
(arc42 §5.1). Its contract is sketched in arc42 §5.3.

> **Status:** not implemented yet. Its first code arrives with
> [#77 Black frame through Renderer and Film](https://github.com/sergioffpc/umbra/issues/77).
