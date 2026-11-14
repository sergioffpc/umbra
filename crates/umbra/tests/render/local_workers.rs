//! Local Workers in child processes over QUIC, through `coordinate` (ADR
//! 0010, ADR 0016, ADR 0026). The children are this crate's `umbra worker`
//! on the CPU, so that the transport, the wire encoding and the
//! reassignment of a lost Worker's Work Units run without a GPU: a CPU
//! Work Result does not depend on the Worker or its threads, so a
//! single-Pass Film has the same hash however its Work Units travel.

use std::net::SocketAddr;
use std::num::{NonZeroU32, NonZeroUsize};
use std::path::PathBuf;
use std::process::Command;
use std::time::{Duration, Instant};

use umbra_cluster::{
    CoordinatorOptions, GpuOrdinal, GpuWorkers, RenderJob, RenderJobOutcome, StatusLine, Threads,
    coordinate,
};
use umbra_math::Point2i;
use umbra_render::{BuildIdentity, Cancellation, FilmHash, ReproductionRecord, Tree};
use umbra_scene::{ContentHash, FilmParameters, RenderJobOptions, SceneBuilder};

/// A camera-only Render Job of `side` × `side` pixels, `spp` Pixel Samples
/// per pixel in one Pass, in Tiles of 16 × 16.
fn job(side: i32, spp: u32) -> RenderJob {
    let mut builder = SceneBuilder::new();
    builder.film(FilmParameters {
        resolution: Point2i::new(side, side),
    });
    let spp = NonZeroU32::new(spp).expect("Pixel Samples per pixel are positive");
    let options = RenderJobOptions {
        spp,
        spp_per_pass: spp,
        tile_size: NonZeroU32::new(16).expect("16 is positive"),
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
        record,
    }
}

/// The Film hash of `job` rendered with `options`, which must finish it.
fn film_hash(job: RenderJob, options: &CoordinatorOptions<'_>) -> FilmHash {
    let outcome = coordinate(job, options, &Cancellation::new(), StatusLine::Logged)
        .expect("the Render Job finishes");
    let RenderJobOutcome::Finished { report, .. } = outcome else {
        panic!("nothing cancelled it");
    };
    report
        .film_hash()
        .expect("a finished Render Job has a Film hash")
}

/// `umbra worker` on the CPU, connecting to `coordinator`.
fn cpu_worker(coordinator: SocketAddr) -> Command {
    let mut command = Command::new(env!("CARGO_BIN_EXE_umbra"));
    command.args([
        "--log",
        "warn",
        "worker",
        "--render-device",
        "cpu",
        "--coordinator",
        &coordinator.to_string(),
    ]);
    command
}

#[test]
fn workers_over_quic_merge_the_film_of_the_in_memory_worker() {
    let expected = film_hash(job(64, 64), &CoordinatorOptions::default());

    let command = |_: GpuOrdinal, coordinator: SocketAddr| cpu_worker(coordinator);
    let options = CoordinatorOptions {
        cpu: None,
        gpus: Some(GpuWorkers {
            gpus: vec![GpuOrdinal(0), GpuOrdinal(1)],
            command: &command,
        }),
    };
    assert_eq!(film_hash(job(64, 64), &options), expected);
}

#[test]
fn a_worker_killed_mid_render_gives_its_work_units_back_and_the_film_completes() {
    let (side, spp) = (64, 1024);
    let expected = film_hash(job(side, spp), &CoordinatorOptions::default());

    // The child renders on one core, so on any machine it is ready well
    // before it is killed, and the Render Job, on that core and one thread
    // of this process, runs well past the kill (checked below): the child
    // always holds its one Work Unit in flight. Its loss is found by the
    // QUIC idle timeout, as the child joins as a CPU Worker, not as the GPU
    // whose process exited.
    let kill_after = Duration::from_secs(1);
    let command = |_: GpuOrdinal, coordinator: SocketAddr| {
        let worker = cpu_worker(coordinator);
        let mut command = Command::new("timeout");
        command
            .args(["--signal=KILL", &format!("{}s", kill_after.as_secs())])
            .args(["taskset", "-c", "0"])
            .arg(worker.get_program())
            .args(worker.get_args());
        command
    };
    let options = CoordinatorOptions {
        cpu: Some(Threads::Count(NonZeroUsize::MIN)),
        gpus: Some(GpuWorkers {
            gpus: vec![GpuOrdinal(0)],
            command: &command,
        }),
    };
    let start = Instant::now();
    let hash = film_hash(job(side, spp), &options);

    assert!(
        start.elapsed() > kill_after * 2,
        "the Render Job ran past the kill: {:?}",
        start.elapsed()
    );
    assert_eq!(hash, expected);
}

#[test]
fn with_every_worker_gone_the_render_job_fails() {
    // A child that exits at once, before it ever joins.
    let command = |_: GpuOrdinal, _: SocketAddr| Command::new("true");
    let options = CoordinatorOptions {
        cpu: None,
        gpus: Some(GpuWorkers {
            gpus: vec![GpuOrdinal(0)],
            command: &command,
        }),
    };
    let error = coordinate(
        job(16, 1),
        &options,
        &Cancellation::new(),
        StatusLine::Logged,
    )
    .unwrap_err();
    assert!(matches!(error, umbra_cluster::Error::NoWorkers), "{error}");
}
