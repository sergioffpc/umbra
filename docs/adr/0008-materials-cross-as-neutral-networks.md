# Materials cross into the Scene as neutral networks

A USD material reaches the render delegate as an `HdMaterialNetwork2`: its
nodes, their parameters and the connections between them, for MaterialX
nodedefs or OSL shaders. The OpenUSD bridge translates it into a material
network of umbra's types: each node is a MaterialX nodedef id or an `.oso`
shader, with its parameters and its connections. The Scene stores that
network, with no `pxr` type and no shader code.

`hdMtlx`, OpenUSD's conversion of Hydra networks into MaterialX documents, is
not used: it would bring MaterialX's types into the OpenUSD bridge, and the
translation umbra needs is a direct walk of the network's nodes.

Shaders are compiled by the renderer, not by the bridge or the Scene: a
MaterialX bridge generates OSL from a network (MaterialX's GenOsl), and the
OSL bridge compiles it, with the `.oso` shaders the network names. Compiling
shaders is a phase of the renderer, after the scene loads.

## Consequences

- The Scene stays a description: it can be built and inspected without
  MaterialX or OSL, and the renderer decides how to compile what it holds.
- Each dependency stays in its bridge: OpenUSD in `src/ext/usd`, MaterialX
  in its own, OSL in its own; none of their types crosses into the Scene.
- A material the bridge cannot translate, or whose shaders fail to compile,
  is replaced by the fallback material, with a warning that names it.
