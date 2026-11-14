//! The GPU Local Workers' child processes (ADR 0026): the Coordinator
//! starts one per GPU, each connecting back to it over loopback QUIC, and
//! learns when one exits.

use std::fmt;
use std::io;
use std::net::SocketAddr;
use std::process::{Command, Stdio};

use tokio::process::Child;
use tokio::sync::mpsc::UnboundedSender;
use tracing::{info, warn};

use crate::device::{GpuOrdinal, RenderDevice};
use crate::transport::Event;

/// The command line of a GPU's Local Worker, given the GPU and the
/// Coordinator's address: a Worker on `gpu:N` that connects to that
/// address (`umbra worker --render-device gpu:N --coordinator ADDRESS`).
/// The Coordinator gives it no stdin or stdout, and its own process group,
/// so that Ctrl-C on the Coordinator's terminal reaches the Coordinator
/// alone.
pub type WorkerCommand<'c> = dyn Fn(GpuOrdinal, SocketAddr) -> Command + Sync + 'c;

/// The GPUs whose Local Workers run in child processes, and how each is
/// started.
pub struct GpuWorkers<'c> {
    /// The GPUs.
    pub gpus: Vec<GpuOrdinal>,
    /// The command line of each one's Local Worker.
    pub command: &'c WorkerCommand<'c>,
}

impl fmt::Debug for GpuWorkers<'_> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("GpuWorkers")
            .field("gpus", &self.gpus)
            .finish_non_exhaustive()
    }
}

/// Starts the Local Worker of each of `gpus` with `command`, connecting to
/// `coordinator`, and returns the GPUs whose process started. Each sends
/// [`Event::Exited`] to `events` when its process exits, and is killed if
/// it is still running when the Coordinator's runtime shuts down.
pub(crate) fn launch(
    gpus: &[GpuOrdinal],
    command: &WorkerCommand<'_>,
    coordinator: SocketAddr,
    events: &UnboundedSender<Event>,
) -> Vec<GpuOrdinal> {
    let mut started = Vec::new();
    for &gpu in gpus {
        let device = RenderDevice::Gpu(gpu);
        match start(command(gpu, coordinator)) {
            Ok(child) => {
                let pid = child.id().unwrap_or_default();
                info!("started the Local Worker of {device}, process {pid}");
                tokio::spawn(watch(child, gpu, events.clone()));
                started.push(gpu);
            }
            Err(error) => warn!("cannot start the Local Worker of {device}: {error}"),
        }
    }
    started
}

/// Starts `command` as a Local Worker's child process.
fn start(command: Command) -> io::Result<Child> {
    let mut command = tokio::process::Command::from(command);
    command
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .process_group(0)
        .kill_on_drop(true);
    command.spawn()
}

/// Waits for the process of the Local Worker of `gpu` to exit, and tells
/// `events`.
async fn watch(mut child: Child, gpu: GpuOrdinal, events: UnboundedSender<Event>) {
    let device = RenderDevice::Gpu(gpu);
    match child.wait().await {
        Ok(status) if status.success() => {}
        Ok(status) => warn!("the Local Worker of {device} exited: {status}"),
        Err(error) => warn!("the Local Worker of {device} is lost: {error}"),
    }
    // The Coordinator's loop outlives every child's task: a send fails only
    // as it returns.
    let _ = events.send(Event::Exited { gpu });
}
