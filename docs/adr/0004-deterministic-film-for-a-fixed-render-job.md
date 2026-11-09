# Deterministic Film for a fixed Render Job

> **Amended by [ADR 0022](0022-determinism-on-one-machine-statistical-equality-across-machines.md).** The Film is bit-identical only up to the order in which Work Results are merged. Work Results carry no content hash, there is no `verify` option, and the Film merges on arrival with no early-arrival buffer. Pure Pixel Samples, the per-pixel summation order and the deterministic BVH stay. CI's golden Film hashes come from single-Pass renders on x86-64.

Given the same Scene, seed and Render Job options (Tile size, Pixel Samples per Pass), the final Film is bit-identical. This holds however many Workers take part, whichever Worker or thread renders a Work Unit, in whatever order Work Results arrive, and despite Worker failures and reassignments. A Render Job can then be reproduced exactly, a pixel can be debugged on any machine, and any difference is a bug rather than noise. [ADR 0002](0002-bit-identical-results-across-architectures.md) already makes a single Work Result bit-identical across architectures. This ADR extends that guarantee to the whole Film.

Floating-point addition is not associative, so the guarantee comes from fixing every source of order and randomness:

- **Pixel Samples are pure functions.** A Pixel Sample depends only on `(Scene, seed, pixel, sample index)`. pbrt-v4's samplers are ported faithfully: `StartPixelSample` rebuilds the sampler state, and no state carries over between Pixel Samples. The same hashes (`MixBits`, `MurmurHash64A`), PCG32 and Sobol tables are used, so a seed yields the same sequences as pbrt-v4. All randomness in an integrator (Russian roulette, light selection, BSDF sampling) comes from the sampler. Thread-local RNGs, wall-clock time and shared counters are banned on the render path and enforced by lint. The default seed is 0, and `--seed` overrides it.
- **Canonical summation order.** Inside a Work Result, each pixel sums its Pixel Samples in ascending sample index. Threads split a Work Unit by pixel, never across the Pixel Samples of one pixel. Tiles are disjoint, so the Film only has to merge each Tile's Work Results in Pass order. The Coordinator holds any Work Result that arrives early until the previous Pass of that Tile has been merged.
- **Splats in a total order.** Contributions that cross Tile boundaries (BDPT `t=1` strategies, light tracing, MLT) go into a separate splat accumulator in the Work Result. The Coordinator merges these accumulators in the total order (Pass, Tile index). MLT and SPPM do not map directly onto Work Units, and their eventual designs must preserve this guarantee.
- **Deterministic BVH.** Each Worker builds its BVH from the Scene. The build is a pure function of the Scene, independent of thread count and architecture: SAH with stable partitioning, explicit tie-breaks, and parallelism only over disjoint subtrees joined in a fixed order. Ties in intersection distance and traversal-level Pixel Traces therefore reproduce everywhere. A faster non-deterministic builder is allowed only under the aggressive-optimisation rule and never as the default.
- **Run to completion.** A Render Job always renders every Pass. There is no time budget, no adaptive sampling and no partial image. A cancelled Render Job produces no image.

The guarantee is verified, not assumed. CI renders small Scenes and compares Film hashes with committed values on both arm64 and x86-64. Invariance tests render the same Render Job with 1 and N threads, with shuffled Work Unit order and with a simulated Worker failure, and compare the Films. Each Work Result carries a 64-bit content hash. When a reassigned Work Unit produces two Work Results, the Coordinator compares their hashes and logs a mismatch as an error naming both Workers. An opt-in `verify` Render Job option renders a fraction of Work Units on two different Workers and compares them.

## Considered options

- **Invariance to decomposition** (the same Film for any Tile or Pass size): rejected. It needs strict per-sample ordering across the whole Render Job, so Work Results would carry every contribution or use exact accumulation. That cost buys an invariance nobody needs, because Render Job options are recorded and can be resubmitted.
- **Determinism per Work Unit only** (the Film may differ in the last bit depending on arrival order): rejected. A re-render would not reproduce the image, and a mismatch could no longer be told apart from noise.
- **One accumulator per Pass, summed at the end**: rejected. It multiplies Film memory by the number of Passes.
- **Exact (fixed-point or superaccumulator) Film sums**: rejected. They cost more than ordered `f64` sums and depart from pbrt-v4's numerics ([ADR 0001](0001-numeric-model.md)).
- **Free BVH shape with a global tie-break at intersection** (lowest primitive index): rejected. It fixes the hit result, but traversal-level Pixel Traces would no longer reproduce across machines.

## Consequences

- The Coordinator must buffer early Work Results. Scheduling has to bound how far ahead of a Tile's merged Pass new Passes are assigned, so this buffer stays small.
- Reproducing a Render Job requires resubmitting its options, not only the Scene and seed.
