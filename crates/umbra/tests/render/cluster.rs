//! Remote Workers on a named Cluster, through the binary (ADR 0021): `umbra
//! worker` processes that find `umbra render` by DNS-SD and join its Render
//! Job. Each test names its own Cluster, so that tests running at once, and
//! anything else on the LAN, never see each other's Render Jobs.

use std::collections::BTreeMap;
use std::io::{BufRead, BufReader, Read};
use std::process::{Child, ChildStderr, Command, Stdio};
use std::sync::mpsc::{self, Receiver};
use std::thread;
use std::time::{Duration, Instant};

use serde_json::Value;

use crate::cli::{CAMERA_ONLY, Workspace, field};

/// How long a test waits for something it expects the cluster to log.
const PATIENCE: Duration = Duration::from_secs(30);

/// A Cluster Name of its own for the test `test` of this run.
pub(crate) fn cluster(test: &str) -> String {
    let name = format!("t{}-{test}", std::process::id());
    name.chars().take(63).collect()
}

/// The camera-only Scene File, 256 × 256 pixels at enough Pixel Samples per
/// pixel that it renders for far longer than any test lasts.
pub(crate) fn long_scene() -> String {
    CAMERA_ONLY
        .replace("xresolution\" [ 40 ]", "xresolution\" [ 256 ]")
        .replace("yresolution\" [ 24 ]", "yresolution\" [ 256 ]")
        .replace("pixelsamples\" 4", "pixelsamples\" 1048576")
}

/// The message of a JSON log line, or the line itself if it is not one.
fn message(line: &str) -> String {
    serde_json::from_str::<Value>(line)
        .ok()
        .and_then(|event| event["fields"]["message"].as_str().map(str::to_owned))
        .unwrap_or_else(|| line.to_owned())
}

/// The log messages of a process, as it writes them on stderr.
pub(crate) struct Log {
    lines: Receiver<String>,
    seen: Vec<String>,
}

impl Log {
    /// Reads `stderr` on a thread of its own.
    fn of(stderr: ChildStderr) -> Self {
        let (send, lines) = mpsc::channel();
        thread::spawn(move || {
            BufReader::new(stderr)
                .lines()
                .map_while(Result::ok)
                .try_for_each(|line| send.send(message(&line)))
        });
        Self {
            lines,
            seen: Vec::new(),
        }
    }

    /// A log that holds nothing and receives nothing.
    fn closed() -> Self {
        Self {
            lines: mpsc::channel().1,
            seen: Vec::new(),
        }
    }

    /// Waits for a message that `matches`, and returns it.
    pub(crate) fn wait_for(&mut self, what: &str, matches: impl Fn(&str) -> bool) -> String {
        let deadline = Instant::now() + PATIENCE;
        let Self { lines, seen } = self;
        let found = std::iter::from_fn(|| {
            let left = deadline.saturating_duration_since(Instant::now());
            lines.recv_timeout(left).ok()
        })
        .inspect(|line| seen.push(line.clone()))
        .find(|line| matches(line));
        found.unwrap_or_else(|| panic!("no {what} in {seen:#?}"))
    }

    /// Every message so far, and the rest until the process closes stderr.
    pub(crate) fn rest(mut self) -> Vec<String> {
        self.seen.extend(self.lines.iter());
        self.seen
    }
}

/// `umbra render` of a Scene File in a workspace, running.
pub(crate) struct Coordinator {
    child: Child,
    pub(crate) log: Log,
}

/// How a Coordinator ended: its exit code, its report and its log.
pub(crate) struct Ended {
    pub(crate) code: Option<i32>,
    pub(crate) report: BTreeMap<String, String>,
    pub(crate) log: Vec<String>,
}

impl Coordinator {
    /// Starts `umbra render` of `scene.pbrt` in `ws` for `cluster`, its
    /// Local Worker on the CPU alone with one thread, and `args`.
    pub(crate) fn start(ws: &Workspace, cluster: &str, args: &[&str]) -> Self {
        let mut child = ws
            .command(&["--log-format", "json", "render", "scene.pbrt"])
            .args(["--cluster", cluster, "--allow-dirty"])
            .args(["--render-devices", "cpu", "--nthreads", "1"])
            .args(args)
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .spawn()
            .expect("umbra render runs");
        let log = Log::of(child.stderr.take().expect("stderr is piped"));
        Self { child, log }
    }

    /// Waits until a remote Worker is ready, and returns its log line.
    pub(crate) fn wait_for_remote_worker(&mut self) -> String {
        self.log.wait_for("a remote Worker ready", |line| {
            line.starts_with("Worker ") && line.ends_with("is ready")
        })
    }

    /// Sends it SIGINT, and waits for it to end.
    pub(crate) fn interrupt(self) -> Ended {
        signal(&self.child, "INT");
        self.wait()
    }

    /// Waits for it to end.
    pub(crate) fn wait(mut self) -> Ended {
        let mut stdout = String::new();
        let mut out = self.child.stdout.take().expect("stdout is piped");
        out.read_to_string(&mut stdout).expect("stdout is text");
        let status = self.child.wait().expect("umbra render ends");
        let log = std::mem::replace(&mut self.log, Log::closed());
        let report = stdout
            .lines()
            .filter_map(|line| line.split_once(": "))
            .map(|(name, value)| (name.to_owned(), value.to_owned()))
            .collect();
        Ended {
            code: status.code(),
            report,
            log: log.rest(),
        }
    }
}

impl Drop for Coordinator {
    fn drop(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}

/// `umbra worker` on the CPU, running until the test stops it.
pub(crate) struct RemoteWorker {
    child: Child,
    pub(crate) log: Log,
}

impl RemoteWorker {
    /// Starts `umbra worker` of `cluster` with one thread, and `args`, with
    /// every GPU hidden, as `Workspace::command` hides them.
    pub(crate) fn start(cluster: &str, args: &[&str]) -> Self {
        let mut child = Command::new(env!("CARGO_BIN_EXE_umbra"))
            .env("CUDA_VISIBLE_DEVICES", "")
            .args(["--log-format", "json", "worker", "--cluster", cluster])
            .args(["--nthreads", "1"])
            .args(args)
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::piped())
            .spawn()
            .expect("umbra worker runs");
        let log = Log::of(child.stderr.take().expect("stderr is piped"));
        Self { child, log }
    }

    /// Sends it `signal` (`TERM`, `KILL`, `STOP`, `CONT`).
    pub(crate) fn signal(&self, name: &str) {
        signal(&self.child, name);
    }

    /// Stops it with SIGTERM, and returns its exit code and log.
    pub(crate) fn terminate(mut self) -> (Option<i32>, Vec<String>) {
        signal(&self.child, "TERM");
        let status = self.child.wait().expect("umbra worker ends");
        let log = std::mem::replace(&mut self.log, Log::closed());
        (status.code(), log.rest())
    }
}

impl Drop for RemoteWorker {
    fn drop(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}

/// Sends `child` the signal `name`.
fn signal(child: &Child, name: &str) {
    let sent = Command::new("kill")
        .args([&format!("-{name}"), &child.id().to_string()])
        .status()
        .expect("kill runs");
    assert!(sent.success(), "SIG{name} sent");
}

/// The rows of the Workers in `report`, by Worker number.
pub(crate) fn workers(report: &BTreeMap<String, String>) -> Vec<&str> {
    report
        .iter()
        .filter(|(name, _)| name.starts_with("Worker ") && name[7..].parse::<u32>().is_ok())
        .map(|(_, row)| row.as_str())
        .collect()
}

/// The Work Units of a Worker's row.
pub(crate) fn work_units(row: &str) -> u64 {
    row.split(", ")
        .find_map(|part| part.strip_suffix(" Work Units"))
        .and_then(|n| n.parse().ok())
        .unwrap_or_else(|| panic!("Work Units in {row}"))
}

#[test]
fn a_worker_of_the_cluster_joins_the_render_job_and_renders() {
    let ws = Workspace::new();
    ws.file("scene.pbrt", &long_scene());
    let cluster = cluster("joins");
    let mut coordinator = Coordinator::start(&ws, &cluster, &["--tile-size", "8"]);
    let worker = RemoteWorker::start(&cluster, &[]);

    coordinator.wait_for_remote_worker();
    // Time for the remote Worker to deliver Work Results.
    thread::sleep(Duration::from_secs(1));
    let ended = coordinator.interrupt();

    assert_eq!(
        ended.report["Workers"], "2 joined, 0 refused",
        "{:?}",
        ended.log
    );
    let rows = workers(&ended.report);
    let remote = rows
        .iter()
        .find(|row| row.starts_with("Worker on cpu"))
        .unwrap_or_else(|| panic!("a remote Worker in {rows:?}"));
    assert!(work_units(remote) > 0, "{remote}");
    assert_eq!(field(remote, "node"), "0", "{remote}");
    let (code, log) = worker.terminate();
    assert_eq!(code, Some(0), "{log:#?}");
}

/// The value of `name` in `slots`, the report's Worker Slots, of the one
/// slot listed: `failures` or `refusals`.
fn slot_count(slots: &str, name: &str) -> u32 {
    slots
        .split(": ")
        .nth(1)
        .and_then(|counts| {
            counts
                .split(", ")
                .find_map(|part| part.strip_suffix(&format!(" {name}")))
        })
        .and_then(|n| n.parse().ok())
        .unwrap_or_else(|| panic!("{name} in {slots}"))
}

/// The count before `name` in the report's Scheduling line.
fn scheduling(report: &BTreeMap<String, String>, name: &str) -> u64 {
    let line = &report["Scheduling"];
    line.split(", ")
        .find_map(|part| part.strip_suffix(&format!(" {name}")))
        .and_then(|n| n.rsplit(' ').next())
        .and_then(|n| n.parse().ok())
        .unwrap_or_else(|| panic!("{name} in {line}"))
}

#[test]
fn a_worker_of_another_cluster_does_not_join() {
    let ws = Workspace::new();
    ws.file("scene.pbrt", &long_scene());
    let mut coordinator = Coordinator::start(&ws, &cluster("ours"), &[]);
    let worker = RemoteWorker::start(&cluster("theirs"), &[]);

    coordinator
        .log
        .wait_for("the Local Worker ready", |line| line.ends_with("is ready"));
    // Time enough to discover the Render Job, had it been of its Cluster.
    thread::sleep(Duration::from_secs(3));
    let ended = coordinator.interrupt();

    assert_eq!(
        ended.report["Workers"], "1 joined, 0 refused",
        "{:?}",
        ended.log
    );
    let (_, log) = worker.terminate();
    assert!(
        !log.iter().any(|line| line.contains("discovered")),
        "{log:#?}"
    );
}

#[test]
fn a_worker_killed_mid_render_is_a_failure_of_its_slot_and_its_work_units_go_back() {
    let ws = Workspace::new();
    ws.file("scene.pbrt", &long_scene());
    let cluster = cluster("killed");
    // Work Units that each take seconds, so the Worker always holds one.
    let args = ["--tile-size", "8", "--spp-per-pass", "65536"];
    let mut coordinator = Coordinator::start(&ws, &cluster, &args);
    let worker = RemoteWorker::start(&cluster, &[]);
    coordinator.wait_for_remote_worker();

    worker.signal("KILL");
    coordinator
        .log
        .wait_for("its Work Units given back", |line| {
            line.starts_with("Worker ") && line.contains("go back to the queue")
        });
    coordinator.log.wait_for("its failure", |line| {
        line.starts_with("Worker ") && line.ends_with("failed")
    });
    let ended = coordinator.interrupt();

    assert_eq!(slot_count(&ended.report["Worker Slots"], "failures"), 1);
    assert!(
        scheduling(&ended.report, "reassigned") >= 1,
        "{:?}",
        ended.report
    );
}

#[test]
fn a_worker_slot_that_fails_three_times_in_a_row_is_refused() {
    let ws = Workspace::new();
    ws.file("scene.pbrt", &long_scene());
    let cluster = cluster("barred");
    let mut coordinator = Coordinator::start(&ws, &cluster, &[]);

    // A GPU Worker on a machine with no GPU visible fails as soon as it has
    // the Scene: `WORKER_FATAL`. Each is replaced at once, as a supervisor
    // would, so each failure is a new Worker of the same slot.
    for _ in 0..3 {
        let worker = RemoteWorker::start(&cluster, &["--render-device", "gpu:0"]);
        coordinator.log.wait_for("a failure", |line| {
            line.starts_with("Worker ") && line.ends_with("failed")
        });
        drop(worker);
    }
    let mut fourth = RemoteWorker::start(&cluster, &["--render-device", "gpu:0"]);
    coordinator.log.wait_for("the refusal", |line| {
        line.contains("refused") && line.contains("failed three times in a row")
    });
    fourth
        .log
        .wait_for("the Worker told it is refused", |line| {
            line.contains("refused by the Coordinator")
        });
    let ended = coordinator.interrupt();

    assert_eq!(
        ended.report["Workers"], "4 joined, 1 refused",
        "{:?}",
        ended.log
    );
}

#[test]
fn a_worker_stopped_by_its_operator_is_not_a_failure() {
    let ws = Workspace::new();
    ws.file("scene.pbrt", &long_scene());
    let cluster = cluster("stopped");
    let args = ["--tile-size", "8", "--spp-per-pass", "65536"];
    let mut coordinator = Coordinator::start(&ws, &cluster, &args);
    let worker = RemoteWorker::start(&cluster, &[]);
    coordinator.wait_for_remote_worker();

    let (code, log) = worker.terminate();
    assert_eq!(code, Some(0), "{log:#?}");
    coordinator
        .log
        .wait_for("its Work Units given back", |line| {
            line.starts_with("Worker ") && line.contains("go back to the queue")
        });
    let ended = coordinator.interrupt();

    assert_eq!(slot_count(&ended.report["Worker Slots"], "failures"), 0);
    assert!(
        scheduling(&ended.report, "reassigned") >= 1,
        "{:?}",
        ended.report
    );
}

#[test]
fn a_worker_stays_up_and_joins_the_next_render_job() {
    let ws = Workspace::new();
    ws.file("scene.pbrt", &long_scene());
    let cluster = cluster("next");
    let mut worker = RemoteWorker::start(&cluster, &[]);

    for _ in 0..2 {
        let mut coordinator = Coordinator::start(&ws, &cluster, &[]);
        coordinator.wait_for_remote_worker();
        coordinator.interrupt();
        worker.log.wait_for("the end of the Render Job", |line| {
            line.contains("cancelled")
        });
    }
    let (code, log) = worker.terminate();
    assert_eq!(code, Some(0), "{log:#?}");
}

#[test]
fn a_silent_worker_is_declared_dead_and_rejoins_once_it_wakes() {
    let ws = Workspace::new();
    ws.file("scene.pbrt", &long_scene());
    let cluster = cluster("silent");
    let mut coordinator = Coordinator::start(&ws, &cluster, &[]);
    let worker = RemoteWorker::start(&cluster, &[]);
    let first = coordinator.wait_for_remote_worker();

    worker.signal("STOP");
    coordinator.log.wait_for("its death", |line| {
        line.starts_with("Worker ") && line.ends_with("failed")
    });
    worker.signal("CONT");
    let second = coordinator.wait_for_remote_worker();
    assert_ne!(first, second, "a rejoin is a new Worker");
    let ended = coordinator.interrupt();

    assert_eq!(
        ended.report["Workers"], "3 joined, 0 refused",
        "{:?}",
        ended.log
    );
    assert_eq!(slot_count(&ended.report["Worker Slots"], "failures"), 1);
}

#[test]
fn an_unknown_interface_is_an_error_that_lists_the_interfaces() {
    let ws = Workspace::new();
    ws.file("scene.pbrt", CAMERA_ONLY);
    let coordinator = Coordinator::start(&ws, &cluster("iface"), &["--interface", "nope0"]);
    let ended = coordinator.wait();

    assert_eq!(ended.code, Some(1), "{:?}", ended.log);
    let error = ended
        .log
        .iter()
        .find(|line| line.contains("nope0"))
        .unwrap_or_else(|| panic!("the error in {:?}", ended.log));
    assert!(error.contains("lo"), "the interfaces listed: {error}");
}

#[test]
fn a_port_taken_is_an_error() {
    let taken = std::net::UdpSocket::bind("0.0.0.0:0").expect("a UDP socket");
    let port = taken.local_addr().expect("its address").port().to_string();
    let ws = Workspace::new();
    ws.file("scene.pbrt", CAMERA_ONLY);
    let coordinator = Coordinator::start(&ws, &cluster("port"), &["--port", &port]);
    let ended = coordinator.wait();

    assert_eq!(ended.code, Some(1), "{:?}", ended.log);
    assert!(
        ended
            .log
            .iter()
            .any(|line| line.contains("cannot listen on")),
        "{:?}",
        ended.log
    );
}
