//! `work`: a Worker in its own process, serving one Render Job over QUIC
//! (arc42 §8.13). Each Local Worker is one, started by the Coordinator.

use std::net::SocketAddr;
use std::num::NonZeroUsize;
use std::thread;

use quinn::{Connection, RecvStream, SendStream};
use rayon::{ThreadPool, ThreadPoolBuilder};
use tokio::sync::mpsc::{UnboundedReceiver, UnboundedSender};
use tokio::task::JoinSet;
use tracing::{debug, info_span};
use umbra_render::{Cancellation, WorkResult};
use umbra_scene::{RenderJobOptions, Scene, SceneBuilder};

use crate::children::ProcessId;
use crate::device::{GpuOrdinal, RenderDevice};
use crate::error::Error;
use crate::quic::{self, SERVER_NAME};
use crate::runtime;
use crate::transport::{self, ToCoordinator, ToWorker, WorkerEnd};
use crate::wire::{
    CloseCode, FromCoordinator, FromWorker, WorkResultHeader, read_frame, write_frame,
    write_work_result,
};
use crate::{cpu_worker, gpu_worker};

/// How many threads a CPU Worker renders with.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum Threads {
    /// One per logical CPU.
    #[default]
    All,
    /// Exactly this many.
    Count(NonZeroUsize),
}

/// How a Worker serves a Render Job.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct WorkerOptions {
    /// The Render Device it renders on.
    pub render_device: RenderDevice,
    /// Its threads, on the CPU.
    pub threads: Threads,
    /// The Coordinator's address.
    pub coordinator: SocketAddr,
}

/// A Render Device, ready to render: the CPU's thread pool, or a GPU.
#[derive(Debug)]
enum Device {
    Cpu(ThreadPool),
    Gpu(GpuOrdinal),
}

impl Device {
    /// The Render Device of `options`, with its threads on the CPU.
    fn new(options: &WorkerOptions) -> Result<Self, Error> {
        Ok(match options.render_device {
            RenderDevice::Cpu => {
                let count = match options.threads {
                    Threads::All => 0,
                    Threads::Count(n) => n.get(),
                };
                let pool = ThreadPoolBuilder::new()
                    .num_threads(count)
                    .build()
                    .map_err(Error::ThreadPool)?;
                Self::Cpu(pool)
            }
            RenderDevice::Gpu(gpu) => Self::Gpu(gpu),
        })
    }

    /// Which Render Device it is.
    fn render_device(&self) -> RenderDevice {
        match self {
            Self::Cpu(_) => RenderDevice::Cpu,
            Self::Gpu(gpu) => RenderDevice::Gpu(*gpu),
        }
    }

    /// Its CPU threads: none on a GPU.
    fn threads(&self) -> u32 {
        match self {
            Self::Cpu(pool) => u32::try_from(pool.current_num_threads()).unwrap_or(u32::MAX),
            Self::Gpu(_) => 0,
        }
    }
}

/// Serves one Render Job as `options` say: connects to the Coordinator,
/// receives the Scene and builds it, then renders the Work Units it is
/// assigned until the Render Job finishes or is cancelled, or its
/// connection closes. It blocks; the runtime it needs is its own. Once
/// `stop` is made, it closes its connection with `WORKER_STOPPED` and
/// returns.
///
/// # Errors
///
/// [`Error`] if the runtime, the CPU's threads or the endpoint cannot
/// start, the connection fails, the Coordinator breaks the wire protocol,
/// the Scene cannot be built, or the Render Device cannot render it.
pub fn work(options: &WorkerOptions, stop: &Cancellation) -> Result<(), Error> {
    let device = Device::new(options)?;
    let coordinator = options.coordinator;
    runtime::runtime()?.block_on(async {
        let endpoint = quic::worker_endpoint(coordinator)?;
        let connection = endpoint.connect(coordinator, SERVER_NAME)?.await?;
        let served = serve(&connection, device, stop).await;
        // Whatever ended it, the connection closes: the Coordinator never
        // waits on a Worker that has given up.
        if let Err(error) = &served {
            connection.close(close_code(error).into(), error.to_string().as_bytes());
        }
        endpoint.wait_idle().await;
        served
    })
}

/// The code a Worker closes its connection with after `error`:
/// `WORKER_FATAL` for its own failure, `PROTOCOL_ERROR` for the
/// Coordinator's.
fn close_code(error: &Error) -> CloseCode {
    if let Error::Fatal(_) = error {
        CloseCode::WorkerFatal
    } else {
        CloseCode::ProtocolError
    }
}

/// Serves the Render Job of `connection` on `device`.
async fn serve(connection: &Connection, device: Device, stop: &Cancellation) -> Result<(), Error> {
    let (mut control_send, control_recv) = handshake(connection, &device).await?;
    let (options, builder) = receive_scene(connection).await?;
    let (coordinator_end, worker_end) = transport::connect();
    let renders = thread::spawn(move || render(device, builder, &options, worker_end));
    let reader = tokio::spawn(read_control(
        control_recv,
        coordinator_end.to_worker.clone(),
    ));
    let served = relay(
        connection,
        &mut control_send,
        coordinator_end.from_worker,
        &coordinator_end.to_worker,
        stop,
    )
    .await;
    reader.abort();
    // The render thread has returned: its channel is closed.
    let _ = renders.join();
    served
}

/// `Hello`, answered by `Welcome`: the control stream's two halves.
async fn handshake(
    connection: &Connection,
    device: &Device,
) -> Result<(SendStream, RecvStream), Error> {
    let (mut send, mut recv) = connection.open_bi().await?;
    let hello = FromWorker::Hello {
        render_device: device.render_device(),
        threads: device.threads(),
        process_id: ProcessId::this(),
    };
    write_frame(&mut send, &hello).await?;
    match read_frame(&mut recv).await? {
        Some(FromCoordinator::Welcome { worker_id }) => {
            debug!("joined the Render Job as Worker {worker_id}");
            Ok((send, recv))
        }
        Some(other) => Err(Error::OutOfOrder(other.name())),
        None => Err(Error::StreamEnded("the control stream")),
    }
}

/// The Scene stream: the Render Job Options, then the Scene's builder,
/// then its end.
async fn receive_scene(connection: &Connection) -> Result<(RenderJobOptions, SceneBuilder), Error> {
    let mut recv = connection.accept_uni().await?;
    let missing = || Error::StreamEnded("the Scene stream");
    let options = read_frame(&mut recv).await?.ok_or_else(missing)?;
    let builder = read_frame(&mut recv).await?.ok_or_else(missing)?;
    match read_frame::<()>(&mut recv).await? {
        None => Ok((options, builder)),
        Some(()) => Err(Error::OutOfOrder("a frame after the Scene")),
    }
}

/// Builds the Scene of `builder` and the renderer of `device`, and runs its
/// render loop, on this thread: the runtime's thread keeps the connection
/// alive meanwhile, and a GPU's renderer stays on the thread that made it.
/// A Worker that cannot render tells the Coordinator why.
fn render(
    device: Device,
    builder: SceneBuilder,
    options: &RenderJobOptions,
    connection: WorkerEnd,
) {
    match info_span!("scene_build").in_scope(|| builder.build()) {
        Ok(scene) => render_scene(device, &scene, options, connection),
        Err(error) => fatal(&connection, &Error::Scene(error)),
    }
}

/// Tells the Coordinator, through `connection`, why the Worker cannot
/// continue.
fn fatal(connection: &WorkerEnd, error: &dyn std::error::Error) {
    let _ = connection
        .to_coordinator
        .send(ToCoordinator::Fatal(error.to_string()));
}

/// Builds the renderer of `device` for `scene` and runs its render loop.
fn render_scene(device: Device, scene: &Scene, options: &RenderJobOptions, connection: WorkerEnd) {
    match device {
        Device::Cpu(pool) => match umbra_cpu::Renderer::new(scene, options) {
            Ok(renderer) => cpu_worker::work(&renderer, &pool, connection),
            Err(error) => fatal(&connection, &error),
        },
        Device::Gpu(gpu) => {
            let renderer = match umbra_gpu::Renderer::new(usize::from(gpu), scene, options) {
                Ok(renderer) => renderer,
                Err(error) => return fatal(&connection, &error),
            };
            let to_coordinator = connection.to_coordinator.clone();
            match gpu_worker::work(&renderer, connection) {
                gpu_worker::End::Done => {}
                gpu_worker::End::Failed(error) => {
                    let _ = to_coordinator.send(ToCoordinator::Fatal(error.to_string()));
                }
            }
        }
    }
}

/// Passes the Coordinator's control messages to the render loop; once the
/// stream ends, the connection is closing, and the render loop stops.
async fn read_control(mut recv: RecvStream, to_worker: UnboundedSender<ToWorker>) {
    // A send fails only once the render loop has returned.
    loop {
        match read_frame(&mut recv).await {
            Ok(Some(FromCoordinator::Assign { work_unit })) => {
                let _ = to_worker.send(ToWorker::Assign(work_unit));
            }
            Ok(Some(FromCoordinator::JobFinished)) => {
                let _ = to_worker.send(ToWorker::JobFinished);
            }
            Ok(Some(FromCoordinator::Welcome { .. }) | None) | Err(_) => {
                let _ = to_worker.send(ToWorker::Cancel);
                return;
            }
        }
    }
}

/// Sends the render loop's messages to the Coordinator until the loop
/// returns, then closes the connection as [`Closing`] says.
async fn relay(
    connection: &Connection,
    control: &mut SendStream,
    mut from_worker: UnboundedReceiver<ToCoordinator>,
    to_worker: &UnboundedSender<ToWorker>,
    stop: &Cancellation,
) -> Result<(), Error> {
    let mut outbox = Outbox {
        connection,
        uploads: JoinSet::new(),
        closing: Closing::Finished,
    };
    let mut ticker = runtime::ticker();
    loop {
        tokio::select! {
            message = from_worker.recv() => match message {
                Some(message) => outbox.send(control, message).await,
                None => break,
            },
            _ = ticker.tick() => {
                if stop.is_cancelled() {
                    outbox.stop(to_worker);
                }
            }
        }
    }
    outbox.close().await
}

/// Sends `result` after `header` on a new stream of `connection`.
async fn upload(
    connection: Connection,
    header: WorkResultHeader,
    result: WorkResult,
) -> Result<(), Error> {
    let send = connection.open_uni().await?;
    write_work_result(send, &header, &result).await
}

/// How a Worker closes its connection once its render loop returns.
#[derive(Debug)]
enum Closing {
    /// `FINISHED`: the Render Job ended, finished or cancelled.
    Finished,
    /// `WORKER_FATAL`, after `Fatal` with this reason.
    Fatal(String),
    /// `WORKER_STOPPED`: its operator stopped it.
    Stopped,
}

impl Closing {
    /// The close code it closes with.
    fn code(&self) -> CloseCode {
        match self {
            Self::Finished => CloseCode::Finished,
            Self::Fatal(_) => CloseCode::WorkerFatal,
            Self::Stopped => CloseCode::WorkerStopped,
        }
    }
}

/// What a Worker sends the Coordinator: control messages on the control
/// stream, each Work Result on a stream of its own.
struct Outbox<'c> {
    connection: &'c Connection,
    uploads: JoinSet<Result<(), Error>>,
    closing: Closing,
}

impl Outbox<'_> {
    /// Sends `message` from the render loop.
    async fn send(&mut self, control: &mut SendStream, message: ToCoordinator) {
        // A failed write means a closed connection, which the control
        // stream's reader sees too.
        match message {
            ToCoordinator::RequestWork(count) => {
                let _ = write_frame(control, &FromWorker::RequestWork { count }).await;
            }
            ToCoordinator::WorkResult {
                result,
                render_time,
                thread_time,
            } => {
                let header = WorkResultHeader {
                    work_unit: result.unit(),
                    render_time,
                    thread_time,
                };
                let upload = upload(self.connection.clone(), header, result);
                self.uploads.spawn(upload);
            }
            ToCoordinator::Fatal(message) => {
                let fatal = FromWorker::Fatal {
                    message: message.clone(),
                };
                let _ = write_frame(control, &fatal).await;
                self.closing = Closing::Fatal(message);
            }
        }
    }

    /// Closes the connection at once with `WORKER_STOPPED`, and stops the
    /// render loop through `to_worker`.
    fn stop(&mut self, to_worker: &UnboundedSender<ToWorker>) {
        self.closing = Closing::Stopped;
        self.connection.close(self.closing.code().into(), b"");
        let _ = to_worker.send(ToWorker::Cancel);
    }

    /// Waits for every Work Result to be sent, then closes the connection,
    /// with the reason of a `Fatal` in the close itself: closing drops
    /// whatever a stream has not sent yet.
    async fn close(mut self) -> Result<(), Error> {
        while self.uploads.join_next().await.is_some() {}
        let reason = match &self.closing {
            Closing::Fatal(message) => message.as_bytes(),
            Closing::Finished | Closing::Stopped => b"",
        };
        self.connection.close(self.closing.code().into(), reason);
        match self.closing {
            Closing::Finished | Closing::Stopped => Ok(()),
            Closing::Fatal(message) => Err(Error::Fatal(message)),
        }
    }
}
