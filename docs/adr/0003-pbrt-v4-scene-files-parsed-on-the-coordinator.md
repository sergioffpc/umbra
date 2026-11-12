# pbrt-v4 Scene Files, parsed once on the Coordinator

> **Amended by [ADR 0018](0018-sessions-launched-from-the-operators-machine.md).** The set of Workers is not a Render Job option: it is the launch script's host list ([arc42 §7.3](../architecture/07-deployment-view.md#73-deployment-and-launch)), and it never changes the Film ([ADR 0004](0004-deterministic-film-for-a-fixed-render-job.md)). The rest of this ADR still applies.
>
> **Amended by [ADR 0021](0021-plug-and-play-workers-in-a-named-cluster.md).** The Workers are whichever Workers of the Coordinator's Cluster join while it runs. They are still not a Render Job option.
>
> **Amended by [ADR 0031](0031-renders-checked-by-eye.md).** pbrt-v4's renders are no longer Umbra's correctness oracle: the owner checks renders by eye. The pinned commits stay, for the Reference Scenes and the speed comparison of QS-5, and moving them publishes nothing.

Umbra reads exactly one input format: pbrt-v4's `.pbrt` Scene File, unextended. Each milestone declares the subset it supports and grows it until the whole format is covered. This lets the pbrt-v4 test scenes, and reference images rendered by pbrt-v4 itself, serve as Umbra's correctness oracle and benchmark suite without a converter.

- **Strict validation.** The Coordinator rejects a Render Job before assigning any Work Unit if its Scene File uses a directive, type or parameter the current milestone does not support. It reports every such use with `file:line:column`, not only the first. There is no lenient mode, because a render that silently skips unsupported features looks plausible but no longer matches the reference. Parameters that are recognised but unused produce a warning, as in pbrt-v4.
- **Parsed once, on the Coordinator.** The Coordinator parses and validates the Scene File and builds a Scene: a format-independent description with parameters resolved, transforms applied and assets (meshes, images) referenced by content hash. That Scene, not the Scene File, is replicated to the Workers. Each Worker builds its own acceleration structures. Workers need no file-system access and no knowledge of the `.pbrt` grammar, and every Worker starts from identical data ([ADR 0002](0002-bit-identical-results-across-architectures.md)). How the Scene is encoded and transferred belongs to the wire protocol.
- **No grammar extensions.** Settings that exist only in Umbra (Tile size, Pixel Samples per Pass, the set of Workers) are Render Job options supplied at submission. So are pbrt-v4-style overrides (`--spp`, `--seed`, `--cropwindow`, …), which keep pbrt-v4's names and semantics, take precedence over the Scene File, and are applied by the Coordinator when it builds the Scene. Any Umbra Scene File therefore still renders in pbrt-v4 for comparison.
- **A pinned reference.** "pbrt-v4" means fixed commits of `mmp/pbrt-v4` and `mmp/pbrt-v4-scenes`, recorded with the validation infrastructure. Moving them is an explicit decision that regenerates the reference images.

The lexical layer and directive grammar use `nom` with `nom_locate` for positions. They sit behind a `ParserTarget`-style interface, as in pbrt-v4's `parser.cpp`. The graphics state (CTM, attributes, named materials, object instances, includes) and the Scene builder follow pbrt-v4, so the parser library can be swapped without touching them.

## Considered options

- **A custom format (TOML/JSON/RON) with a `.pbrt` converter**: rejected. It means two grammars to maintain, and a converter that would itself need validating against pbrt-v4.
- **Lenient parsing, or a `--lenient` flag**: rejected. A wrong image costs more than a refused Render Job.
- **Shipping the Scene File and assets to each Worker to re-parse**: rejected. It means N parsers running, a file-system dependency on every Worker, and room for Workers to diverge.
- **Building the BVH on the Coordinator and shipping it**: rejected. It transfers far more data, ties the acceleration-structure layout to the protocol, and serialises a parallel build on one machine.
- **pbrt-v3 input**: out of scope. `pbrt --upgrade` converts it.
- **A hand-written tokenizer mirroring pbrt-v4**: viable, and closer to the book. `nom` was chosen for its maturity on edge cases and on large inline arrays. The choice sits behind the tokenizer, so it is cheap to reverse.
