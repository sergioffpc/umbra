# umbra-cluster

The Coordinator and the Worker: `coordinate` runs a Render Job and `work`
runs a Worker, both synchronous, each owning its `tokio` runtime. Behind them
are the QUIC wire protocol (ADR 0010), discovery by Cluster Name, heartbeats,
pull-based Pass-major scheduling (ADR 0009), reassignment and the speculative
tail, the Local Worker, Build Identity admission, the status line's content
and the Render Job Report.

It is a `std` crate, outside the core. It depends on `umbra-render` and on
the CPU's renderer, `umbra-cpu`; the GPU's, `umbra-gpu`, joins with the GPU
Local Worker (#128). The `umbra` binary depends on it; it receives a parsed Scene and does not depend
on `umbra-parser` (arc42 §5.1). Its contract is sketched in arc42 §5.3.

```rust
use std::num::NonZeroU32;
use std::path::PathBuf;
use umbra_cluster::{CoordinatorOptions, RenderJob, RenderJobOutcome, coordinate};
use umbra_render::{BuildIdentity, Cancellation, ReproductionRecord, Tree};
use umbra_scene::{ContentHash, RenderJobOptions, SceneBuilder};

// pbrt-v4's default camera-only Scene, one Pixel Sample per pixel.
let scene = SceneBuilder::new().build()?;
let resolution = scene.full_resolution();
let options = RenderJobOptions {
    spp: NonZeroU32::MIN,
    ..RenderJobOptions::default()
};
let identity = BuildIdentity {
    commit: "0000000".to_owned(),
    tree: Tree::Clean,
};
let record =
    ReproductionRecord::new(PathBuf::from("scene.pbrt"), ContentHash([0; 32]), &options, identity);
let output = PathBuf::from("pbrt.exr");
let job = RenderJob { scene, options, output, record };

let cancel = Cancellation::new();
// The status is the caller's to show; this one shows none.
let outcome = coordinate(job, &CoordinatorOptions::default(), &cancel, |_, _| {})?;
let RenderJobOutcome::Finished { image, report } = outcome else {
    panic!("nothing cancelled it");
};
assert!(report.film_hash().is_some());
assert_eq!(image.resolution(), resolution);
# Ok::<(), Box<dyn std::error::Error>>(())
```

## What is in it

| Module         | Contents                                                                                                                                                          |
| -------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `coordinator`  | `coordinate`, `RenderJob`, `CoordinatorOptions` (the Local Worker's `Threads`), `RenderJobOutcome` (finished or cancelled); the scheduling loop                   |
| `error`        | `Error`: why a Render Job did not finish                                                                                                                          |
| `local_worker` | The CPU Local Worker: renders its Work Units with its `Renderer`, several at once on its own rayon pool, and times each, on the clock and on its threads          |
| `renderer`     | `Renderer`, chosen by the Worker's `RenderDevice` (ADR 0033): the CPU's so far                                                                                    |
| `host`         | `Host`: the Local Worker's host name and CPU model, read from `/proc`                                                                                             |
| `progress`     | What is assigned and merged, the busy and thread time, each Tile's cost, the moving average of the rate and the ETA                                               |
| `status`       | `Status`, the status line's content: the aggregate line and the Local Worker's; `Moment`, running or ended                                                        |
| `transport`    | The in-memory transport: `Assign`, `JobFinished` and `Cancel` to the Worker, `RequestWork` and timed Work Results to the Coordinator, typed and unencoded (§8.13) |
| `report`       | `RenderJobReport`, `Outcome`, `Phase`; the report's text, one `Name: value` line per figure, is its `Display`                                                     |

Not in the report yet: the Scene and BVH statistics, with
the BVH; the Anomalous Pixel Sample count, with
[#85](https://github.com/sergioffpc/umbra/issues/85).

Not here yet: QUIC and the GPU
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
  chooses where they go (arc42 §5.3); `umbra` writes the report to stdout.
- **`cancel`** is polled every 250 ms: once made, the Coordinator stops
  assigning, tells the Worker to stop at its next row, and returns
  `RenderJobOutcome::Cancelled` with the report of what was merged. The
  caller decides what makes it (Ctrl-C, in `umbra`).
- **The status is content only.** `coordinate` calls its `status`
  argument with a `Status` at every tick, about every 250 ms, as
  `Moment::Running`, and once at the end as `Moment::Ended`. It neither
  draws nor logs it: `umbra` redraws it at the bottom of a terminal, or
  logs it about every 10 s and at the end.
- **The report's parse, Scene build and image write times** are the
  caller's to record, with `RenderJobReport::record_phase`.

## Tests

Through the binary, in `crates/umbra/tests/render/` (spec #72, Seam 1):
the image and report of a camera-only Scene File, and the same Film hash
with one thread and with all of them, the report's fields, the logged
status, and Ctrl-C. Scheduling is checked there, not through internal
interfaces: the report's CPU model is checked on x86-64 and on arm64, which
CI runs too, and the status line's number formats on its logged lines.
