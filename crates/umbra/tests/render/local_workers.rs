//! Local Workers in child processes over QUIC, through `coordinate` (ADR
//! 0010, ADR 0016, ADR 0026, ADR 0035). The children are this crate's
//! `umbra worker` on the CPU, so that the transport, the wire encoding and
//! the reassignment of a lost Worker's Work Units run without a GPU: a CPU
//! Work Result does not depend on the Worker or its threads, so a
//! single-Pass Film has the same hash however its Work Units travel.

use std::net::SocketAddr;
use std::num::NonZeroU32;
use std::path::PathBuf;
use std::process::Command;
use std::time::{Duration, Instant};

use umbra_cluster::{
    ClusterName, CoordinatorOptions, DirtyBuilds, GpuOrdinal, Interface, Liveness, RenderDevice,
    RenderJob, RenderJobOutcome, Status, WorkerCommand, coordinate,
};
use umbra_math::Point2i;
use umbra_render::{BuildIdentity, Cancellation, FilmHash, ReproductionRecord, Tree};
use umbra_scene::{ContentHash, FilmParameters, RenderJobOptions, SceneBuilder};

/// A camera-only Render Job of `side` × `side` pixels, `spp` Pixel Samples
/// per pixel in one Pass, in Tiles of 16 × 16.
pub(crate) fn job(side: i32, spp: u32) -> RenderJob {
    job_in_tiles(side, spp, 16)
}

/// A camera-only Render Job of `side` × `side` pixels, `spp` Pixel Samples
/// per pixel in one Pass, in Tiles of `tile` × `tile`.
pub(crate) fn job_in_tiles(side: i32, spp: u32, tile: u32) -> RenderJob {
    let mut builder = SceneBuilder::new();
    builder.film(FilmParameters {
        resolution: Point2i::new(side, side),
    });
    let spp = NonZeroU32::new(spp).expect("Pixel Samples per pixel are positive");
    let options = RenderJobOptions {
        spp,
        spp_per_pass: spp,
        tile_size: NonZeroU32::new(tile).expect("a Tile has pixels"),
        ..RenderJobOptions::default()
    };
    let identity = BuildIdentity {
        commit: "0000000".to_owned(),
        tree: Tree::Clean,
    };
    let record = ReproductionRecord::new(
        PathBuf::from("scene.pbrt"),
        ContentHash([0; 32]),
        &options,
        identity,
    );
    RenderJob {
        scene: builder.build().expect("a camera-only Scene builds"),
        options,
        output: PathBuf::from("scene.exr"),
        record,
    }
}

/// How the Coordinator of these tests runs: `local_workers`, each started
/// with `command`, and no remote Worker, its Cluster being its own.
pub(crate) fn options<'c>(
    local_workers: Vec<RenderDevice>,
    command: &'c WorkerCommand<'c>,
) -> CoordinatorOptions<'c> {
    let cluster = format!("local-{}", std::process::id());
    CoordinatorOptions {
        local_workers,
        worker_command: command,
        cluster: cluster.parse::<ClusterName>().expect("a DNS label"),
        port: 0,
        interface: Interface::All,
        dirty_builds: DirtyBuilds::Refused,
        liveness: Liveness::default(),
    }
}

/// The Film hash of `job` rendered with `options`, which must finish it.
fn film_hash(job: RenderJob, options: &CoordinatorOptions<'_>) -> FilmHash {
    let outcome =
        coordinate(job, options, &Cancellation::new(), |_, _| {}).expect("the Render Job finishes");
    let RenderJobOutcome::Finished { report, .. } = outcome else {
        panic!("nothing cancelled it");
    };
    report
        .film_hash()
        .expect("a finished Render Job has a Film hash")
}

/// `umbra worker` on the CPU with `threads`, connecting to `coordinator`.
pub(crate) fn cpu_worker(coordinator: SocketAddr, threads: Threads) -> Command {
    let mut command = Command::new(env!("CARGO_BIN_EXE_umbra"));
    command.args(["--log", "warn", "--log-format", "json", "worker"]);
    command.args(["--render-device", "cpu"]);
    command.args(["--coordinator", &coordinator.to_string()]);
    if let Threads::One = threads {
        command.args(["--nthreads", "1"]);
    }
    command
}

/// The threads a CPU Worker of these tests renders with.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum Threads {
    One,
    All,
}

#[test]
fn one_worker_on_one_thread_and_two_on_every_thread_merge_the_same_film() {
    let one = |_: RenderDevice, coordinator: SocketAddr| cpu_worker(coordinator, Threads::One);
    let expected = film_hash(job(64, 64), &options(vec![RenderDevice::Cpu], &one));

    let all = |_: RenderDevice, coordinator: SocketAddr| cpu_worker(coordinator, Threads::All);
    let both = options(vec![RenderDevice::Cpu, RenderDevice::Cpu], &all);
    assert_eq!(film_hash(job(64, 64), &both), expected);
}

#[test]
fn a_worker_killed_mid_render_gives_its_work_units_back_and_the_film_completes() {
    let (side, spp) = (64, 1024);
    let all = |_: RenderDevice, coordinator: SocketAddr| cpu_worker(coordinator, Threads::All);
    let expected = film_hash(job(side, spp), &options(vec![RenderDevice::Cpu], &all));

    // The Worker started for `gpu:0` is a CPU Worker on one core, killed
    // `kill_after` after it starts: on any machine it is ready well before,
    // and the Render Job, on that core and the other Worker's one thread,
    // runs well past it (checked below), so it always holds its one Work
    // Unit in flight. The shell `exec`s the Worker, so the child process
    // the Coordinator started is the Worker, killed.
    let kill_after = Duration::from_secs(1);
    let command = |device: RenderDevice, coordinator: SocketAddr| match device {
        RenderDevice::Cpu => cpu_worker(coordinator, Threads::One),
        RenderDevice::Gpu(_) => killed_after(&cpu_worker(coordinator, Threads::All), kill_after),
    };
    let devices = vec![RenderDevice::Cpu, RenderDevice::Gpu(GpuOrdinal(0))];
    let start = Instant::now();
    let hash = film_hash(job(side, spp), &options(devices, &command));

    assert!(
        start.elapsed() > kill_after * 2,
        "the Render Job ran past the kill: {:?}",
        start.elapsed()
    );
    assert_eq!(hash, expected);
}

#[test]
fn with_every_worker_gone_the_render_job_waits_until_it_is_cancelled() {
    // A child that exits at once, before it ever joins.
    let command = |_: RenderDevice, _: SocketAddr| Command::new("true");
    let cancel = Cancellation::new();
    let mut statuses: Vec<Status> = Vec::new();
    let outcome = coordinate(
        job(16, 1),
        &options(vec![RenderDevice::Cpu], &command),
        &cancel,
        |status, _| {
            statuses.push(status.clone());
            // Waiting for a second is waiting: no Worker will come.
            if statuses.len() == 4 {
                cancel.cancel();
            }
        },
    )
    .expect("a Render Job without Workers waits, it does not fail");

    assert!(matches!(outcome, RenderJobOutcome::Cancelled { .. }));
    let last = statuses.last().expect("a status");
    let first = last.lines().next().expect("an aggregate line");
    assert!(first.starts_with("waiting: no Workers"), "{first}");
}

/// `worker` run by a shell that kills it with SIGKILL `after` it starts,
/// pinned to one core; the shell `exec`s it, so the process started is the
/// Worker.
pub(crate) fn killed_after(worker: &Command, after: Duration) -> Command {
    signalled_after(worker, "KILL", after)
}

/// `worker` run by a shell that sends it `signal` `after` it starts, pinned
/// to one core; the shell `exec`s it, so the process started is the Worker.
pub(crate) fn signalled_after(worker: &Command, signal: &str, after: Duration) -> Command {
    let words: Vec<String> = std::iter::once(worker.get_program())
        .chain(worker.get_args())
        .map(|word| format!("'{}'", word.to_string_lossy()))
        .collect();
    let line = format!(
        "(sleep {}; kill -{signal} $$) & exec taskset -c 0 {}",
        after.as_secs_f64(),
        words.join(" ")
    );
    let mut command = Command::new("sh");
    command.args(["-c", &line]);
    command
}
