# umbra-cluster

The Coordinator and the Worker: `coordinate` runs a Render Job and `work`
runs a Worker, both synchronous, each owning its `tokio` runtime. Behind them
are the QUIC wire protocol (ADR 0010), discovery by Cluster Name, heartbeats,
pull-based Pass-major scheduling (ADR 0009), reassignment and the speculative
tail, the Local Workers, Build Identity admission, the status line's
content and the Render Job Report.

It is a `std` crate, outside the core. It depends on `umbra-render` and on
both renderers, `umbra-cpu` and `umbra-gpu`, and the `umbra` binary depends
on it; it receives a parsed Scene and does not depend on `umbra-parser`
(arc42 §5.1). Its contract is sketched in arc42 §5.3.

The example runs `umbra` from the `PATH` as each Local Worker:

```rust,no_run
use std::net::SocketAddr;
use std::num::NonZeroU32;
use std::path::PathBuf;
use std::process::Command;
use umbra_cluster::{CoordinatorOptions, RenderDevice, RenderJob, RenderJobOutcome, coordinate};
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

// A Local Worker on the CPU, a child process that logs JSON lines.
let worker = |device: RenderDevice, coordinator: SocketAddr| {
    let mut command = Command::new("umbra");
    command
        .args(["--log-format", "json", "worker"])
        .args(["--render-device", &device.to_string()])
        .args(["--coordinator", &coordinator.to_string()]);
    command
};
let options = CoordinatorOptions {
    local_workers: vec![RenderDevice::Cpu],
    worker_command: &worker,
};
let cancel = Cancellation::new();
// The status is the caller's to show; this one shows none.
let outcome = coordinate(job, &options, &cancel, |_, _| {})?;
let RenderJobOutcome::Finished { image, report } = outcome else {
    panic!("nothing cancelled it");
};
assert!(report.film_hash().is_some());
assert_eq!(image.resolution(), resolution);
# Ok::<(), Box<dyn std::error::Error>>(())
```

## What is in it

| Module        | Contents                                                                                                                                                                                         |
| ------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `coordinator` | `coordinate`, `RenderJob`, `CoordinatorOptions` (the Local Workers' Render Devices and their command line), `RenderJobOutcome` (finished or cancelled); the scheduling loop and reassignment     |
| `children`    | `WorkerCommand`: the Local Workers' child processes, started, known by their process, their logs logged again, watched and killed with the Coordinator's runtime                                 |
| `device`      | `RenderDevice` (`cpu` or `gpu:N`), `GpuOrdinal`, `RenderDevices` (`--render-devices`): which Local Workers run, and the GPUs that are usable                                                     |
| `error`       | `Error`: why a Render Job did not finish, or a Worker could not serve one, protocol violations among them                                                                                        |
| `worker`      | `work`, `WorkerOptions`, `Threads`: a Worker in its own process for one Render Job over QUIC, on the CPU or a GPU                                                                                |
| `cpu_worker`  | A CPU Worker's render loop: several Work Units at once on its own rayon pool, each timed on the clock and on its threads                                                                         |
| `gpu_worker`  | A GPU Worker's render loop: `umbra_gpu::Renderer::CAPACITY` Work Units in flight, rendered as one batch                                                                                          |
| `transport`   | The typed messages either way; the channels between a Worker's connection and its render loop; the Coordinator's `Event`s                                                                        |
| `wire`        | The wire protocol (§8.13): the control messages, the Work Result header, their `postcard` frames, the raw pixel sums, the close codes                                                            |
| `quic`        | The two QUIC endpoints: the Coordinator's self-signed certificate on loopback, the Workers' acceptance of any certificate, the 5 s idle timeout and 1 s keep-alive                               |
| `listener`    | The Coordinator's side of QUIC: `Hello`, `Welcome`, the Scene stream, then each Worker's messages and Work Results as `Event`s, a Work Result checked against the Render Job before it is merged |
| `runtime`     | The current-thread `tokio` runtime each side owns, and the 250 ms tick at which it checks a cancellation, a stop or the status                                                                   |
| `host`        | `Host`: the Local Worker's host name and CPU model, read from `/proc`                                                                                                                            |
| `progress`    | What is assigned and merged, the busy and thread time, each Tile's cost, the moving average of the rate and the ETA                                                                              |
| `status`      | `Status`, the status line's content: the aggregate line and the Local Worker's; `Moment`, running or ended                                                                                       |
| `report`      | `RenderJobReport`, `Outcome`, `Phase`; the report's text, one `Name: value` line per figure, is its `Display`                                                                                    |

Not in the report yet: the Scene and BVH statistics, with
the BVH; the Anomalous Pixel Sample count, with
[#85](https://github.com/sergioffpc/umbra/issues/85).

Not here yet, with remote Workers in
[#129](https://github.com/sergioffpc/umbra/issues/129): discovery, `Hello`'s
Cluster Name and Build Identity and their admission, heartbeats, the
speculative tail, Worker Slot failure counting, the Asset Cache, and `work`
staying for one Render Job after another. The status line and the report
still sum every Worker into the Local Worker's figures.

## Design decisions

### A runtime on the caller's thread, every Worker in a process

`coordinate` builds a current-thread `tokio` runtime and blocks on the
scheduling loop. Each Local Worker, the CPU's and each GPU's, is a child
process, `umbra worker --render-device cpu` or `gpu:N`, that connects back
over QUIC on loopback like a remote Worker, so a Worker that fails loses
its Work Units, not the Film (ADR 0026, ADR 0035). Every Worker's messages
arrive on one channel of `Event`s.

### Local Workers log, the Coordinator logs what they log

A Local Worker writes nothing to the terminal: it logs JSON lines to a
pipe, and the Coordinator logs each event at its level, prefixed with the
Worker's Render Device (`gpu:0: …`), so it appears above the status line
and through the same filter. Any other line, a panic's message, is logged
as a warning.

### The Scene travels as its builder

A Worker over QUIC is sent the Render Job Options and the `SceneBuilder`
the Scene was built from, and builds its own replica (arc42 §5.2, §8.13).
`Scene` keeps that builder for this.

### A Worker that goes away gives its Work Units back

A Worker goes away when its connection closes, by itself or after 5 s of
silence, or, for a Local Worker, when its child process exits, which is
noticed at once: its `Hello` names its process. Its Work Units in flight go
back to the front of the queue, and the Render Job finishes on the Workers
that remain; with none left, joined or still starting, it fails with
`Error::NoWorkers`.

### TLS because QUIC requires it

The Coordinator makes a self-signed certificate at start-up and Workers
accept any certificate, while still checking the handshake's signatures
against it (ADR 0016). A Work Result that answers no Work Unit of the
Render Job, or has the wrong number of pixels, closes its Worker's
connection with `PROTOCOL_ERROR` before it reaches the Film.

### The Worker pulls, the Coordinator assigns in order

A CPU Worker asks for as many Work Units as it renders at once, about two
per eight threads and at least one (§8.8), and a GPU Worker for
`umbra_gpu::Renderer::CAPACITY`; each then asks for one more per Work
Result. A GPU Worker renders everything it has been assigned as one batch,
and splits the batch's time among its Work Units by their Pixel Samples. The Coordinator assigns from the Render Job's Work Units, Pass by
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
  assigning, closes every QUIC connection with `JOB_CANCELLED`, which
  stops a CPU Worker at its next row, and returns
  `RenderJobOutcome::Cancelled` with the report of what was merged. The
  caller decides what makes it (Ctrl-C, in `umbra`).
- **The Coordinator waits at most 2 s** for its Workers to close once the
  Render Job ends, then kills the Local Workers' processes still running.
- **`WorkerCommand`** is the caller's: `umbra` runs its own executable as
  `umbra worker --log-format json`. The child gets no stdin or stdout, its
  stderr piped to the Coordinator, and its own process group, so Ctrl-C on
  the terminal reaches the Coordinator alone.
- **`RenderDevices::gpus`** loads CUDA and OptiX to count the usable GPUs;
  `CUDA_VISIBLE_DEVICES` hides GPUs from it as from CUDA.
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
status, Ctrl-C, and `--render-devices` without a GPU. Scheduling is checked
there, not through internal interfaces: the report's CPU model is checked
on x86-64 and on arm64, which CI runs too, and the status line's number
formats on its logged lines.

QUIC is checked through `coordinate` in `local_workers.rs` there, with
Local Workers that are `umbra worker --render-device cpu` child processes:
two of them on every thread merge the Film of one on one thread, bit for
bit, and one killed mid-render gives its Work Units back. The GPU tests,
`crates/umbra/tests/gpu/`, render with the GPU's Local Worker and kill it
mid-render.
