//! Admission and scheduling through `coordinate` (ADR 0009, ADR 0012, arc42
//! §8.8, §8.10): Workers refused for their Build Identity, the speculative
//! tail, and what the status shows of each Worker.

use std::net::SocketAddr;
use std::sync::Arc;
use std::thread;
use std::time::{Duration, Instant};

use umbra_cluster::{
    Liveness, RenderDevice, RenderJobOutcome, RenderJobReport, Status, coordinate,
};
use umbra_render::Cancellation;

use crate::cluster::RemoteWorker;
use crate::local_workers::{Threads, cpu_worker, job, job_in_tiles, options, signalled_after};

/// The value of the line `name` of `report`.
fn line(report: &RenderJobReport, name: &str) -> String {
    let text = report.to_string();
    text.lines()
        .find_map(|line| line.strip_prefix(&format!("{name}: ")))
        .unwrap_or_else(|| panic!("{name} in {text}"))
        .to_owned()
}

/// The report of `outcome`, finished or cancelled.
fn report(outcome: RenderJobOutcome) -> RenderJobReport {
    match outcome {
        RenderJobOutcome::Finished { report, .. } | RenderJobOutcome::Cancelled { report } => {
            report
        }
    }
}

/// Waits for `worker` to be refused for the commit 0000000 and to give up
/// on the Render Job, then makes `cancel`; returns the refusal it logged.
fn cancel_once_given_up(worker: &mut RemoteWorker, cancel: &Cancellation) -> String {
    let refused = worker.log.wait_for("the refusal", |line| {
        line.contains("refused by the Coordinator") && line.contains("0000000")
    });
    worker.log.wait_for("the Worker giving up", |line| {
        line.starts_with("never rejoining")
    });
    cancel.cancel();
    refused
}

#[test]
fn a_worker_on_another_commit_is_refused_and_never_rejoins() {
    // The Render Job's Build Identity is commit 0000000; the Worker is this
    // build, on another.
    let one = |_: RenderDevice, coordinator: SocketAddr| cpu_worker(coordinator, Threads::One);
    let options = options(vec![RenderDevice::Cpu], &one);
    let mut worker = RemoteWorker::start(&options.cluster.to_string(), &[]);
    let cancel = Arc::new(Cancellation::new());
    let watcher = {
        let cancel = Arc::clone(&cancel);
        thread::spawn(move || cancel_once_given_up(&mut worker, &cancel))
    };
    let outcome =
        coordinate(job(64, 1 << 20), &options, &cancel, |_, _| {}).expect("the Render Job runs");
    let refused = watcher.join().expect("the watcher ends");

    assert!(refused.contains("Coordinator's commit"), "{refused}");
    assert_eq!(line(&report(outcome), "Workers"), "1 joined, 1 refused");
}

#[test]
fn the_oldest_work_unit_of_a_stopped_worker_is_copied_and_the_copy_wins() {
    // Worker `gpu:0` is a CPU Worker that stops (SIGSTOP) a second after it
    // starts, holding its one Work Unit: once the other has rendered the
    // rest, it is given a copy of that one, whose Work Result is merged,
    // long before the stopped one is declared dead.
    let command = |device: RenderDevice, coordinator: SocketAddr| {
        let worker = cpu_worker(coordinator, Threads::One);
        match device {
            RenderDevice::Cpu => worker,
            RenderDevice::Gpu(_) => signalled_after(&worker, "STOP", Duration::from_secs(1)),
        }
    };
    let devices = vec![
        RenderDevice::Cpu,
        RenderDevice::Gpu(umbra_cluster::GpuOrdinal(0)),
    ];
    let mut options = options(devices, &command);
    options.liveness = Liveness {
        death_time: Duration::from_secs(30),
        ..Liveness::default()
    };
    let mut lines: Vec<String> = Vec::new();
    let outcome = coordinate(
        job(64, 2048),
        &options,
        &Cancellation::new(),
        |status, _| {
            lines.extend(status.lines().map(str::to_owned));
        },
    )
    .expect("the Render Job finishes");

    let report = report(outcome);
    let scheduling = line(&report, "Scheduling");
    assert!(
        scheduling.contains("speculative copies 1 won"),
        "{scheduling}"
    );
    assert!(
        lines.iter().any(|line| line.contains("· stale ·")),
        "the stopped Worker shown stale: {lines:#?}"
    );
}

#[test]
fn a_worker_whose_progress_stops_shows_as_stalled_and_its_progress_counts() {
    // Tiles of 4 × 4 pixels, each row of 4 pixels at 2^20 Pixel Samples
    // each: seconds per row, between which the Worker makes no progress.
    let one = |_: RenderDevice, coordinator: SocketAddr| cpu_worker(coordinator, Threads::One);
    let mut options = options(vec![RenderDevice::Cpu], &one);
    options.liveness = Liveness {
        stall_time: Duration::from_millis(300),
        ..Liveness::default()
    };
    let cancel = Cancellation::new();
    let start = Instant::now();
    let (mut stalled, mut counted) = (false, false);
    coordinate(
        job_in_tiles(8, 1 << 20, 4),
        &options,
        &cancel,
        |status: &Status, _| {
            let mut lines = status.lines();
            let aggregate = lines.next().unwrap_or_default();
            counted |= aggregate.contains(" 0/4 Work Units") && !aggregate.contains(" 0.0 %");
            stalled |= lines.any(|line| line.contains("· stalled ·"));
            if (stalled && counted) || start.elapsed() > Duration::from_secs(60) {
                cancel.cancel();
            }
        },
    )
    .expect("the Render Job runs");

    assert!(stalled, "the Worker shown stalled between rows");
    assert!(counted, "the rows rendered counted before any merge");
}
