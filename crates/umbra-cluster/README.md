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
use umbra_cluster::{
    ClusterName, CoordinatorOptions, DirtyBuilds, Interface, Liveness, RenderDevice, RenderJob,
    RenderJobOutcome, coordinate,
};
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
// The default Cluster, on every interface and a port the OS picks: the
// remote Workers of that Cluster on the LAN join as well.
let options = CoordinatorOptions {
    local_workers: vec![RenderDevice::Cpu],
    worker_command: &worker,
    cluster: ClusterName::default(),
    port: 0,
    interface: Interface::All,
    dirty_builds: DirtyBuilds::Refused,
    liveness: Liveness::default(),
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

| Module        | Contents                                                                                                                                                                                           |
| ------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `coordinator` | `coordinate`, `RenderJob`, `CoordinatorOptions`, `DirtyBuilds`, `RenderJobOutcome`: listening and announcing, admission, the scheduling loop, reassignment and the speculative tail                |
| `roster`      | `Liveness`; each Worker the Coordinator admitted: its Work Units in flight, its heartbeats, rate and state (`loading`, `alive`, `stale`, `stalled`, `dead`), its status line and report row        |
| `slot`        | The Worker Slot (host, NUMA node, Render Device) and each slot's failures and refusals; three failures in a row refuse it                                                                          |
| `discovery`   | `Interface` (`--interface`); the Coordinator's DNS-SD announcement of `_umbra._udp` with `cluster=<name>`, and a Worker's browsing for those of its Cluster, with `mdns-sd`                        |
| `cluster`     | `ClusterName`: a DNS label, `default` unless set                                                                                                                                                   |
| `children`    | `WorkerCommand`: the Local Workers' child processes, started, known by their process, their logs logged again, watched and killed with the Coordinator's runtime                                   |
| `device`      | `RenderDevice` (`cpu` or `gpu:N`), `GpuOrdinal`, `RenderDevices` (`--render-devices`): which Local Workers run, and the GPUs that are usable                                                       |
| `error`       | `Error`: why a Render Job did not finish, or a Worker could not serve one, protocol violations among them                                                                                          |
| `worker`      | `work`, `WorkerOptions`, `CoordinatorAddress`, `Threads`: a Worker that joins the Render Jobs of its Cluster one after another, with the backoff of its rejoins, or a Local Worker's one           |
| `join`        | A Worker in one Render Job over one connection: `Hello`, `Welcome` or `Reject`, the Scene stream, the heartbeat stream, the render thread and its relay, and how the connection ended              |
| `activity`    | What a Worker is doing, shared by its render loop and its heartbeat: loading, then each Work Unit in flight with its cancellation and its `Meter`                                                  |
| `cpu_worker`  | A CPU Worker's render loop: several Work Units at once on its own rayon pool, each cancellable on its own, timed on the clock and on its threads                                                   |
| `gpu_worker`  | A GPU Worker's render loop: `umbra_gpu::Renderer::CAPACITY` Work Units in flight, rendered as one batch; one cancelled before it is batched is dropped                                             |
| `transport`   | The typed messages either way; the channels between a Worker's connection and its render loop; the Coordinator's `Event`s, the admission's `Verdict`, a Worker's `Departure`                       |
| `wire`        | The wire protocol (§8.13): `Hello`, `Reject` and its `Refusal`, the control messages, the heartbeat, the Work Result header, their `postcard` frames, the raw pixel sums, the close codes          |
| `quic`        | The two QUIC endpoints: the Coordinator's self-signed certificate and idle timeout, the Workers' acceptance of any certificate, the 1 s keep-alive; `Closer`, which remembers how each side closed |
| `listener`    | The Coordinator's side of QUIC: `Hello` to the scheduler's verdict, `Welcome` and the Scene stream or `Reject`, the heartbeats and their death time, each Work Result checked, as `Event`s         |
| `runtime`     | The current-thread `tokio` runtime each side owns, and the 250 ms tick at which it checks a cancellation, a stop or the status                                                                     |
| `host`        | The host name and CPU model, read from `/proc`, and the NUMA node a process's CPU affinity is bound to, from `/sys`                                                                                |
| `progress`    | What is merged, each Tile's cost, the moving average of the rate and the ETA                                                                                                                       |
| `status`      | `Status`, the status line's content: the aggregate line, or `waiting: no Workers`, and one line per Worker; `Moment`, running or ended                                                             |
| `report`      | `RenderJobReport`, `Outcome`, `Phase`; the report's text, one `Name: value` line per figure, is its `Display`                                                                                      |

Not in the report yet: the Scene and BVH statistics, with
the BVH; the Anomalous Pixel Sample count, with
[#85](https://github.com/sergioffpc/umbra/issues/85).

Not here yet: the Asset Cache of ADR 0030, with the first Scene that has
assets; until then the Scene stream is the Render Job Options and the
Scene's builder, and a Worker reports no bytes read from a cache.

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

### Remote Workers find the Render Job, the Coordinator admits them

The Coordinator listens on every interface and a port the OS picks, unless
`port` and `interface` say otherwise, then announces the Render Job by
DNS-SD with its Cluster Name, and withdraws it before the end (ADR 0021,
ADR 0029). Each `Hello` goes to the scheduling loop, which answers
`Welcome`, or `Reject` for another Cluster, another commit, a dirty build
without `DirtyBuilds::Allowed`, or a Worker Slot that failed three times in
a row. A Local Worker, known by its process on this host, is the
Coordinator's own executable and always admitted. A refused Worker never
rejoins that Render Job.

### A Worker that goes away gives its Work Units back

A Worker goes away when its connection closes, or, for a Local Worker,
when its child process exits, which is noticed at once: its `Hello` names
its process. A Worker silent for the death time, 5 s by default, is closed
with `WORKER_TIMED_OUT`; QUIC's idle timeout is the same on both sides, so
a Worker loses its Coordinator as fast. Its Work Units in flight that no
other Worker holds go back to the front of the queue, and the Render Job
finishes on the Workers that remain; with none left, it waits for one to
join, and the status reads `waiting: no Workers`. A failure (`Fatal`,
`WORKER_TIMED_OUT`, a lost connection) counts against the Worker's slot; a
Worker stopped by its operator does not.

### The speculative tail

Once the queue is empty, a Worker that asks for more is given a copy of
the oldest Work Unit in flight that another Worker holds and that has no
copy yet (ADR 0009). The first Work Result merged wins; every other holder
is sent `Cancel`, and drops the Work Unit if it is still rendering, a CPU
Worker between rows, a GPU Worker only before it batches it. A Worker sends
exactly one credit per Work Unit, rendered or cancelled. A Work Result for
a Work Unit its Worker was told to drop has its stream stopped after its
header (`stop_sending`), and one for a Work Unit merged already is
discarded unread. On `JobFinished`, a Worker drops its Work Units in
flight instead of finishing them.

### Heartbeats carry progress

A Worker sends a heartbeat every second on a stream of its own, from
`Welcome` on, and at once when it becomes ready: while loading, the bytes
of the Scene received or that it is building; afterwards, how it loaded
(bytes, replication and build time), each Work Unit in flight with the
Pixel Samples its `Meter` counted, row by row on the CPU, and its busy
threads. The Coordinator derives every rate from those counts.

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

The value types, `ClusterName` among them, in `tests/umbra_cluster/`.
Everything else through the binary, in `crates/umbra/tests/render/`
(spec #72, Seam 1): the image and report of a camera-only Scene File, and the
same Film hash with one thread and with all of them, the report's fields,
the logged status, Ctrl-C, and `--render-devices` without a GPU.

QUIC is checked through `coordinate` in `local_workers.rs` there, with
Local Workers that are `umbra worker --render-device cpu` child processes:
two of them on every thread merge the Film of one on one thread, bit for
bit, one killed mid-render gives its Work Units back, and with every
Worker gone the Render Job waits. `scheduling.rs` checks admission by
commit, the speculative tail and the Workers' states through `coordinate`,
and `cluster.rs` remote Workers over real multicast DNS, each test on a
Cluster of its own. The GPU tests, `crates/umbra/tests/gpu/`, render with
the GPU's Local Worker and kill it mid-render.
