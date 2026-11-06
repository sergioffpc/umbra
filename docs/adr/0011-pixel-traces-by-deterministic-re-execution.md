# Pixel Traces by deterministic re-execution through a monomorphised Recorder

A Pixel Trace is never recorded during a Render Job. It is produced afterwards by `umbra trace`. That command re-runs the requested Pixel Samples in one local process, with no Coordinator and no Workers, and records them as they run. This is sound because a Pixel Sample is a pure function of `(Scene, seed, pixel, sample index)` and the BVH is a pure function of the Scene ([ADR 0004](0004-deterministic-film-for-a-fixed-render-job.md)). The same computation therefore repeats on any machine, down to BVH traversal ([ADR 0002](0002-bit-identical-results-across-architectures.md)). The wire protocol and the Work Result never carry traces.

- **One code path, two recorders.** The integrators and the BVH traversal take a generic `R: Recorder`, and they report events to it as they compute: camera ray, path vertex, light sample, BSDF sample, Russian roulette, termination, BVH node visit and primitive test. Every method of the `Recorder` trait is a no-op by default. The trait lives in the `no_std` rendering core. `NoRecorder` is a zero-sized type whose methods all stay empty, so when the Worker renders with it the compiler removes every call. `TraceRecorder` lives outside the core, may allocate (§8.7) and builds the Pixel Trace. One binary contains both monomorphisations, so `umbra trace` runs exactly the code the Worker runs.
- **Recording cannot change the result.** Arguments to an event are references or copies of values the integrator has already computed. Anything computed only for the trace is computed inside `TraceRecorder`. An oracle test re-renders Pixel Samples with both recorders and checks two things are bit-identical: the radiance, and the recorded pixel summary compared with the Work Result.
- **Every integrator records.** An integrator is not accepted without tracing. Each one reports its own events: medium interactions for volpath, and both subpaths plus each `(s, t)` strategy with its MIS weight for BDPT. What a Pixel Trace means for MLT and SPPM is decided together with their work decomposition.

`dyn Recorder` was not considered, because [ADR 0006](0006-closed-enum-dispatch-for-rendering-interfaces.md) forbids `dyn` on the per-ray path. This is static dispatch over a parameter that never varies within a Render Job, not interface dispatch over a heterogeneous set.

## Considered options

- **Record during the Render Job** for pixels chosen at submission: rejected. Traces would travel in Work Results and the protocol. Pixels would have to be chosen before anyone has seen the image, and the image is only seen once the Render Job finishes.
- **A Cargo feature `trace`** that compiles recording in or out: rejected. The trace binary would differ from the rendering binary, so the claim that the trace shows what the Worker computed would rest on discipline rather than on structure. It also needs a feature matrix in CI, a rebuild before every debugging session, and feature unification can switch recording on in a release build unnoticed.
- **A runtime `if tracing_enabled` check**: rejected. It is cheap, but it is not free on the hottest path.

## Consequences

- `R: Recorder` appears in the signatures of every integrator and of the traversal. This is the price of keeping the guarantee structural.
- Reproducing a Pixel Sample needs the exact Scene, seed, Render Job options and Build Identity. The final image therefore carries a Reproduction Record, and `umbra trace` refuses to run when it does not match.
- The trace of a pixel does not show splats that other Pixel Samples (BDPT `t = 1`, light tracing) sent to that pixel. It shows only the splats this pixel's Pixel Samples sent elsewhere.
