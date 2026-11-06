# Statistical validation against a published pbrt-v4 reference

Umbra is correct when its images are statistically indistinguishable from pbrt-v4's on the Reference Scenes. Decided in [Correctness validation strategy](https://github.com/sergioffpc/umbra/issues/29). A bit-exact comparison with pbrt-v4 is impossible: Umbra uses the pure-Rust `libm`, explicit FMA and no flush-to-zero ([ADR 0002](0002-bit-identical-results-across-architectures.md)), while pbrt-v4 uses the platform libm and whatever FMA contraction its compiler picks, so paths diverge after a few bounces. The comparison is therefore a hypothesis test that tells bias from noise.

- **Per-pixel Welch test.** pbrt-v4 and Umbra each render a Reference Scene K = 16 times with seeds 0–15, at the validation resolution and Pixel Samples per pixel fixed for that Reference Scene. For every pixel and RGB channel, the means and variances of the 16 estimates give a Welch t-test p-value. The Benjamini–Hochberg procedure at q = 0.01 runs over all (pixel, channel) tests, and the Reference Scene fails if it rejects even one. A pixel with zero variance on both sides passes only if both means are equal. Umbra's seeds are fixed, so the verdict for a given commit is deterministic ([ADR 0004](0004-deterministic-film-for-a-fixed-render-job.md)). Where heavy-tailed paths weaken the normality assumption, the fix is more Pixel Samples per render in that Reference Scene's validation settings, not a looser threshold.
- **Anomalous Pixel Samples fail.** Any Anomalous Pixel Sample in Umbra's validation renders fails the Reference Scene, whatever the test says, because a pixel that silently loses a Pixel Sample can still pass it.
- **Only Reference Scenes.** Validation uses the same pinned `mmp/pbrt-v4-scenes` collection as benchmarking ([ADR 0003](0003-pbrt-v4-scene-files-parsed-on-the-coordinator.md), arc42 §8.4), with separate settings for each use. Umbra keeps no Scenes of its own for validation.
- **A published, immutable reference.** A script in the repository builds pbrt-v4 at the pinned commit, renders the K reference images of every Reference Scene, and stores the per-pixel mean and variance as EXR files. They are published once as assets of a GitHub release named `pbrt-v4-reference-<commit>`. `umbra validate` downloads them and checks their hashes, so every machine judges against the same reference and only whoever moves the pin needs pbrt-v4.
- **Run by hand.** `umbra validate` runs on a target machine, as `umbra bench` does. CI never runs it.
- **Film hashes trigger it.** CI renders every Reference Scene of the current milestone at a minimal resolution and Pixel Sample count and compares the Film hashes with committed values on arm64 and x86-64. These hashes are the golden images. A pull request that changes any hash must update the committed values and attach a passing `umbra validate` report for the affected Reference Scenes. A pull request that changes no hash needs no validation.
- **Every milestone closes with a full validation**, in a manual validation issue next to the benchmark measurement issue, with its report committed to `docs/validation/<milestone>.json`.
- **Pinned commits.** `mmp/pbrt-v4` at `b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f` and `mmp/pbrt-v4-scenes` at `30cf4a0346ae5a80a2d7a530a3ef7d0fa4f70572`. Moving either is an explicit decision that publishes a new reference release.

## Considered options

- **An aggregate metric against a converged reference** (relMSE or FLIP under a per-Scene threshold): rejected. The threshold is arbitrary and cannot tell a small bias from noise. relMSE and FLIP remain in the report as diagnostics.
- **Small Scenes written for Umbra, one feature each**: rejected. Failures would be easier to locate, but the Reference Scenes already exist, are pbrt-v4's own, and exercise features together. The statistical sampling tests (arc42 §8.11) locate the routine at fault.
- **Validation in CI on every pull request**: rejected. Sixteen renders per Reference Scene do not fit a shared 4-core runner, and the Film hashes already show exactly which pull requests change pixels.
- **References generated locally on each machine**: rejected. Each machine would hold a slightly different reference, so the same commit could pass on one and fail on another, and every machine would need to build pbrt-v4.
- **References in the repository through Git LFS**: rejected. It is subject to quotas and the repository grows with every milestone.
- **Bonferroni correction**: rejected as too conservative; it hides small biases.

## Consequences

- A pull request that changes rendering costs one manual `umbra validate` run, and the changed Film hash in CI makes that run unavoidable.
- A milestone can only validate the Reference Scenes it can fully render. Until the first milestone covers at least one of them, correctness rests on unit and sampling tests alone.
- Moving the pinned pbrt-v4 commit means regenerating and publishing the reference and re-validating every Reference Scene.
