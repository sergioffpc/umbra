# Renders checked by eye

Umbra no longer compares its images with pbrt-v4's or with committed bits. The owner checks renders by eye. Decided with the owner on 2026-11-11 in [#158](https://github.com/sergioffpc/umbra/issues/158). This supersedes [ADR 0013](0013-statistical-validation-against-a-published-pbrt-v4-reference.md). It amends [ADR 0019](0019-milestones-defined-by-the-reference-scenes-they-unlock.md), [ADR 0020](0020-bit-identical-exr-final-image.md) and [ADR 0022](0022-determinism-on-one-machine-statistical-equality-across-machines.md), and, where they cite the comparisons, [ADR 0003](0003-pbrt-v4-scene-files-parsed-on-the-coordinator.md), [ADR 0004](0004-deterministic-film-for-a-fixed-render-job.md), [ADR 0015](0015-layered-workspace-with-a-seven-crate-no-std-core.md) and [ADR 0026](0026-gpu-workers-from-the-first-milestone.md).

ADR 0013 compared every Reference Scene with pbrt-v4 pixel by pixel. That took 16 pbrt-v4 renders and a Reference Image per Reference Scene and variant, published as a GitHub release by a script of the repository, 16 Umbra renders per Reference Scene on the CPU and again on the GPU, and committed Golden Film Hashes that change with every change to rendering and, since [ADR 0022](0022-determinism-on-one-machine-statistical-equality-across-machines.md), with every glibc CI's runner gets.

The decision:

- **What goes.**
  - `umbra validate`, the per-pixel Welch test with Benjamini–Hochberg, and the validation reports (`docs/validation/<milestone>.json`).
  - Golden Film Hashes in CI.
  - The release `pbrt-v4-reference/<commit>`, its per-pixel statistics and Reference Images, and `scripts/pbrt_reference.py`, which produced them.
  - QS-1, relMSE against the Reference Image ([arc42 §10](../architecture/10-quality-requirements.md)).
  - The `validation` and `golden` settings of the Reference Scenes in `reference/scenes.toml`.
- **What stays.**
  - The Reference Scenes, from `mmp/pbrt-v4-scenes` at its pinned commit: each milestone is still defined by the Reference Scenes it must render ([ADR 0019](0019-milestones-defined-by-the-reference-scenes-they-unlock.md)), and `umbra bench` measures them ([arc42 §8.4](../architecture/08-crosscutting-concepts.md#84-performance-measurement)). Reference Scene Variants stay too, for the features no Reference Scene uses.
  - The pinned `mmp/pbrt-v4` commit, for the speed ratio of QS-5. The benchmark measurement issue applies a variant's edit before pbrt-v4 renders it.
  - Unit tests, the ports of pbrt-v4's tests, and the statistical sampling tests (χ², `pdf`, normalisation, reciprocity) of each crate ([arc42 §8.11](../architecture/08-crosscutting-concepts.md#811-correctness-checks)).
  - Determinism on one machine ([ADR 0022](0022-determinism-on-one-machine-statistical-equality-across-machines.md)) and the tests that compare renders made in the same test run: the invariance tests, ADR 0020's whole-file test, the Pixel Trace oracle and the equivalence tests of an optimisation against its reference.
- **Checked by eye.** Every milestone closes with a manual check issue next to its benchmark measurement issue. The owner renders every Reference Scene and variant unlocked so far on the CPU and on the GPU (the CPU only for `bdpt`, `sppm` and `mlt`, [ADR 0026](0026-gpu-workers-from-the-first-milestone.md)) and looks at the images. A render whose Render Job Report counts an Anomalous Pixel Sample fails the check, as it failed ADR 0013's test, because the eye cannot see a lost Pixel Sample.
- **Across machines.** Images rendered on different machines, or on a Cluster, are statistically equal ([ADR 0022](0022-determinism-on-one-machine-statistical-equality-across-machines.md)), and no test measures it: QS-2 and QS-8 are checked by eye.

## Considered options

- **Keeping ADR 0013**: rejected by the owner for the cost listed above.
- **Keeping only the Golden Film Hashes**: rejected. Without `umbra validate` a changed hash says only that pixels changed, not whether they are right, and the hashes would still have to be regenerated whenever CI's glibc changes the last bits.
- **Keeping only QS-1**: rejected. It needs the Reference Image, so the release and the script would stay.

## Consequences

- A bias too small to see ships. The sampling tests still catch it inside a routine, but not in how an integrator combines routines. Risk RK-13 ([arc42 §11](../architecture/11-risks-and-technical-debt.md)) records it.
- CI no longer shows which pull requests change pixels. The owner looks at the renders when a milestone closes.
- Risks RK-6 (reference renders are expensive) and RK-7 (statistical tests give false verdicts) are retired.
- Moving the pinned commits of pbrt-v4 or of its scenes publishes nothing. It changes what QS-5 compares with, and may change the Reference Scenes.
- `umbra` has the subcommands `render`, `worker`, `trace` and `bench`.
- The rules of ADR 0022 that keep one binary's bits the same on every `x86-64-v3` CPU stay. They are cheap, and they make a Pixel Trace on another machine of a Cluster likely to follow the path that was rendered.
- The release `pbrt-v4-reference/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f` is no longer used.
- The repository has no Python left. `python3` stays in the dev container, which uses it to extract CUDA's `libdevice`.
