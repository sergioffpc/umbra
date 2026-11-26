# Materials cross into the Scene as neutral networks

Revised by docs/adr/0013-devices-render-from-their-own-memory.md: the host
compiles each material at the commit, once for each kind of device the
render uses, and the devices receive the compiled shaders.

A USD material reaches the render delegate as an `HdMaterialNetwork2`: its
nodes, their parameters and the connections between them, for MaterialX
nodedefs or OSL shaders. The OpenUSD bridge translates it into a material
network of umbra's types: each node is a MaterialX nodedef id or an `.oso`
shader, with its parameters and its connections. The network is neutral: it
holds no type of OpenUSD, MaterialX or OSL, and no shader code, and the
Scene stores it as it is.

`hdMtlx`, OpenUSD's conversion of Hydra networks into MaterialX documents, is
not used: it would bring MaterialX's types into the OpenUSD bridge, and the
translation umbra needs is a direct walk of the network's nodes.

Shaders are compiled by the renderer, not by the bridge or the Scene: a
MaterialX bridge generates OSL from a network (MaterialX's GenOsl), and the
OSL bridge compiles it, with the `.oso` shaders the network names. Compiling
shaders is a phase of the renderer, after the scene loads.

A prim whose material does not translate uses the fallback material, an 18%
gray diffuse, and so does a prim without a material, unless its
`displayColor` gives one.

## Consequences

- The Scene stays a description: it can be built and inspected without
  MaterialX or OSL, and the renderer decides how to compile what it holds.
- MaterialX and OSL each stay in their own bridge, as OpenUSD stays in
  `src/ext/usd`.
