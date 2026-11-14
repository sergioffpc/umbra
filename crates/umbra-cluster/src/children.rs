//! The Local Workers' child processes (ADR 0026, ADR 0035): the Coordinator
//! starts one per Render Device of its machine, each connecting back to it
//! over loopback QUIC, passes on what each logs, and learns when one exits.

use std::collections::BTreeMap;
use std::fmt::{self, Display};
use std::io;
use std::net::SocketAddr;
use std::process::{self, Command, Stdio};

use serde::{Deserialize, Serialize};
use serde_json::Value;
use tokio::io::{AsyncBufReadExt, BufReader};
use tokio::process::{Child, ChildStderr};
use tokio::sync::mpsc::UnboundedSender;
use tracing::{debug, error, info, trace, warn};

use crate::device::RenderDevice;
use crate::transport::Event;

/// The command line of a Local Worker, given its Render Device and the
/// Coordinator's address: a Worker on that device that connects to that
/// address (`umbra worker --render-device cpu --coordinator ADDRESS`), and
/// whose logs on stderr are JSON lines. The Coordinator gives it no stdin
/// or stdout, and its own process group, so that Ctrl-C on the
/// Coordinator's terminal reaches the Coordinator alone.
pub type WorkerCommand<'c> = dyn Fn(RenderDevice, SocketAddr) -> Command + Sync + 'c;

/// A process, by its operating system's identifier: how a Local Worker's
/// `Hello` names the child process it is.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Serialize, Deserialize)]
pub(crate) struct ProcessId(pub(crate) u32);

impl ProcessId {
    /// This process's.
    pub(crate) fn this() -> Self {
        Self(process::id())
    }
}

impl Display for ProcessId {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}", self.0)
    }
}

/// Starts the Local Worker of each of `devices` with `command`, connecting
/// to `coordinator`, and returns the processes that started, with their
/// Render Device. Each passes what it logs on to the Coordinator's log,
/// sends [`Event::Exited`] to `events` when it exits, and is killed if it
/// is still running when the Coordinator's runtime shuts down.
pub(crate) fn launch(
    devices: &[RenderDevice],
    command: &WorkerCommand<'_>,
    coordinator: SocketAddr,
    events: &UnboundedSender<Event>,
) -> BTreeMap<ProcessId, RenderDevice> {
    let mut started = BTreeMap::new();
    for &device in devices {
        match start(command(device, coordinator)) {
            Ok((child, process)) => {
                info!("started the Local Worker of {device}, process {process}");
                tokio::spawn(watch(child, process, device, events.clone()));
                started.insert(process, device);
            }
            Err(error) => warn!("cannot start the Local Worker of {device}: {error}"),
        }
    }
    started
}

/// Starts `command` as a Local Worker's child process, its stderr piped to
/// the Coordinator.
fn start(command: Command) -> io::Result<(Child, ProcessId)> {
    let mut command = tokio::process::Command::from(command);
    command
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::piped())
        .process_group(0)
        .kill_on_drop(true);
    let child = command.spawn()?;
    let process = child
        .id()
        .map(ProcessId)
        .ok_or_else(|| io::Error::other("the child exited at once"))?;
    Ok((child, process))
}

/// Passes on what the Local Worker of `device` logs until its process
/// exits, then tells `events`.
async fn watch(
    mut child: Child,
    process: ProcessId,
    device: RenderDevice,
    events: UnboundedSender<Event>,
) {
    if let Some(stderr) = child.stderr.take() {
        relay_logs(stderr, device).await;
    }
    match child.wait().await {
        Ok(status) if status.success() => {}
        Ok(status) => warn!("the Local Worker of {device} exited: {status}"),
        Err(error) => warn!("the Local Worker of {device} is lost: {error}"),
    }
    // The Coordinator's loop outlives every child's task: a send fails only
    // as it returns.
    let _ = events.send(Event::Exited { process });
}

/// Logs each line `stderr` carries, as the Local Worker of `device` logged
/// it, until its process closes it.
async fn relay_logs(stderr: ChildStderr, device: RenderDevice) {
    let mut lines = BufReader::new(stderr).lines();
    while let Ok(Some(line)) = lines.next_line().await {
        relay_line(&line, device);
    }
}

/// Logs `line`, a JSON log event of the Local Worker of `device`, at its
/// level; any other line, such as a panic's message, as a warning.
fn relay_line(line: &str, device: RenderDevice) {
    let event: Option<Value> = serde_json::from_str(line).ok();
    let logged = event.as_ref().and_then(|event| {
        let level = event["level"].as_str()?;
        let message = event["fields"]["message"].as_str()?;
        Some((level, message))
    });
    match logged {
        Some(("ERROR", message)) => error!("{device}: {message}"),
        Some(("WARN", message)) => warn!("{device}: {message}"),
        Some(("INFO", message)) => info!("{device}: {message}"),
        Some(("DEBUG", message)) => debug!("{device}: {message}"),
        Some((_, message)) => trace!("{device}: {message}"),
        None => warn!("{device}: {line}"),
    }
}
