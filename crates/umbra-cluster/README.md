# umbra-cluster

The Coordinator and the Worker: `coordinate` runs a Render Job and `work`
runs a Worker, both synchronous, each owning its `tokio` runtime. Behind them
are the QUIC wire protocol (ADR 0010), discovery by Cluster Name, heartbeats,
pull-based Pass-major scheduling (ADR 0009), reassignment and the speculative
tail, the Local Worker, Build Identity admission, the status line and the
Render Job Report.

It is a `std` crate, outside the core. It depends on `umbra-render`, and the
`umbra` binary depends on it; it receives a parsed Scene and does not depend
on `umbra-parser` (arc42 §5.1). Its contract is sketched in arc42 §5.3.

```rust
use std::num::NonZeroU32;
use std::path::PathBuf;
use umbra_cluster::{CoordinatorOptions, Outcome, RenderJob, coordinate};
use umbra_render::{BuildIdentity, ReproductionRecord, Tree};
use umbra_scene::{ContentHash, RenderJobOptions, SceneBuilder};

// pbrt-v4's default camera-only Scene, one Pixel Sample per pixel.
let scene = SceneBuilder::new().build()?;
let resolution = scene.full_resolution();
let options = RenderJobOptions {
    pixel_samples_per_pixel: NonZeroU32::MIN,
    ..RenderJobOptions::default()
};
let identity = BuildIdentity {
    commit: "0000000".to_owned(),
    tree: Tree::Clean,
};
let record =
    ReproductionRecord::new(PathBuf::from("scene.pbrt"), ContentHash([0; 32]), &options, identity);
let job = RenderJob { scene, options, record };

let outcome = coordinate(job, &CoordinatorOptions::default(), &mut std::io::sink())?;
assert_eq!(outcome.report.outcome(), Outcome::Finished);
assert_eq!(outcome.image.resolution(), resolution);
# Ok::<(), Box<dyn std::error::Error>>(())
```

## What is in it

| Module         | Contents                                                                                                                                          |
| -------------- | ------------------------------------------------------------------------------------------------------------------------------------------------- |
| `coordinator`  | `coordinate`, `RenderJob`, `CoordinatorOptions` (the Local Worker's `Threads`), `RenderJobOutcome`, `ClusterError`; the scheduling loop           |
| `local_worker` | The CPU Local Worker: renders its Work Units with `umbra-render`'s `Renderer`, several at once on its own rayon pool                              |
| `transport`    | The in-memory transport: `Assign` and `JobFinished` to the Worker, `RequestWork` and Work Results to the Coordinator, typed and unencoded (§8.13) |
| `report`       | `RenderJobReport`, so far its schema version, `Outcome` and Film hash                                                                             |

Not here yet: the status line, the full Render Job Report and Ctrl-C, with
[#86](https://github.com/sergioffpc/umbra/issues/86); QUIC and the GPU
Local Worker, with [#128](https://github.com/sergioffpc/umbra/issues/128);
remote Workers, discovery and `work`, with
[#129](https://github.com/sergioffpc/umbra/issues/129).

## Design decisions

### A runtime on the caller's thread, the Worker on its own

`coordinate` builds a current-thread `tokio` runtime and blocks on the
scheduling loop; the Local Worker runs on a scoped thread beside it, so it
borrows the Scene and the `Renderer` without `Arc`. The two talk over
`tokio` channels, the same messages QUIC will carry (§8.13).

### The Worker pulls, the Coordinator assigns in order

The Local Worker asks for as many Work Units as it renders at once, about
two per eight threads and at least one (§8.8), then for one more per Work
Result. The Coordinator assigns from the Render Job's Work Units, Pass by
Pass in raster Tile order (ADR 0009), and never answers "nothing": a
request it cannot serve yet stays as demand.

### Renders run on the pool, the Worker's loop does not

The Worker receives on its own thread and spawns each render into its pool
with `ThreadPool::in_place_scope`, so a pool of one thread still renders:
no pool thread ever waits on the channel.

### The Film is the Coordinator's

The Film is merged on the Coordinator's thread only, as each Work Result
arrives (ADR 0022); the Work Results it merges do not depend on the thread
count, so a single-Pass Film hash does not either.

## Things to know when using it

- **`coordinate` blocks** until the last Pass is merged; it must not be
  called from inside a `tokio` runtime.
- **The image and the report are returned, not written**: the caller
  chooses where they go (arc42 §5.3).
- **`status`** gets one line at the end so far; the status line of §8.10
  arrives with #86.

## Tests

Through the binary, in `crates/umbra/tests/render/` (spec #72, Seam 1):
the image and report of a camera-only Scene File, and the same Film hash
with one thread and with all of them. Scheduling is checked there, not
through internal interfaces.
