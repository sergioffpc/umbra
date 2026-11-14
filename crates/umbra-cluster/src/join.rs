//! A Worker in one Render Job, over one connection (arc42 §8.13): `Hello`,
//! `Welcome` or `Reject`, the Scene stream, the heartbeat stream, then the
//! Work Units it is assigned, until the connection closes; and how it
//! closed, which decides whether the Worker rejoins.

use std::fmt::{self, Display};
use std::net::SocketAddr;
use std::sync::Arc;
use std::sync::atomic::{AtomicU64, Ordering};
use std::thread;
use std::time::{Duration, Instant};

use quinn::{Connection, Endpoint, RecvStream, SendStream};
use tokio::sync::mpsc::{UnboundedReceiver, UnboundedSender};
use tokio::task::JoinSet;
use tokio::time;
use tracing::{info, info_span, warn};
use umbra_render::{Cancellation, WorkResult};
use umbra_scene::{RenderJobOptions, Scene, SceneBuilder};

use crate::activity::Activity;
use crate::error::Error;
use crate::quic::{self, Closed, Closer, SERVER_NAME};
use crate::runtime;
use crate::transport::{self, ToCoordinator, ToWorker, WorkerEnd};
use crate::wire::{
    self, CloseCode, FromCoordinator, FromWorker, Hello, Loaded, Loading, Refusal,
    WorkResultHeader, read_frame, write_frame, write_work_result,
};
use crate::worker::Device;
use crate::{cpu_worker, gpu_worker};

/// How often a Worker sends a heartbeat (arc42 §8.13).
const HEARTBEAT: Duration = Duration::from_secs(1);

/// How often a Worker logs a summary of its Work Units in flight.
const SUMMARY: Duration = Duration::from_secs(10);

/// How a Worker's time in a Render Job ended (ADR 0021): what it does
/// next depends on it.
#[derive(Debug)]
pub(crate) enum Ending {
    /// The Render Job finished: idle.
    Finished,
    /// The Coordinator's operator cancelled it: idle.
    Cancelled,
    /// The Worker's operator stopped it: it exits.
    Stopped,
    /// The Coordinator refused it: never rejoin this Render Job.
    Rejected(Refusal),
    /// A malformed or out-of-order message: never rejoin this Render Job.
    ProtocolError(Error),
    /// It failed, was declared dead, lost the Coordinator, or could not
    /// connect: rejoin after a while.
    Failed(Error),
}

impl Display for Ending {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Finished => f.write_str("the Render Job finished"),
            Self::Cancelled => f.write_str("the Render Job was cancelled"),
            Self::Stopped => f.write_str("stopped"),
            Self::Rejected(reason) => write!(f, "refused by the Coordinator: {reason}"),
            Self::ProtocolError(error) => write!(f, "a protocol error: {error}"),
            Self::Failed(error) => write!(f, "failed: {error}"),
        }
    }
}

/// One attempt at serving a Render Job: how it ended, and how many Work
/// Results the Worker delivered meanwhile.
#[derive(Debug)]
pub(crate) struct Attempt {
    pub(crate) ending: Ending,
    pub(crate) delivered: u64,
}

/// Joins the Render Job of the Coordinator at `address` as `hello` says,
/// and serves it on `device` until its connection closes, or `stop` is
/// made, which closes it with `WORKER_STOPPED`.
pub(crate) async fn join(
    address: SocketAddr,
    device: &Arc<Device>,
    hello: &Hello,
    stop: &Cancellation,
) -> Attempt {
    let attempt = |ending| Attempt {
        ending,
        delivered: 0,
    };
    let (endpoint, connection) = match connect(address, stop).await {
        Ok(Some(connected)) => connected,
        Ok(None) => return attempt(Ending::Stopped),
        Err(error) => return attempt(Ending::Failed(error)),
    };
    let closer = Arc::new(Closer::new(connection));
    let delivered = Arc::new(AtomicU64::new(0));
    let served = tokio::select! {
        served = serve(&closer, device, hello, &delivered) => served,
        () = stopped(stop) => {
            // Its Work Units in flight are dropped, not drained.
            closer.close(CloseCode::WorkerStopped, b"");
            Ok(Served::Ended)
        }
    };
    if let Err(error) = &served
        && error.is_protocol_violation()
    {
        closer.close(CloseCode::ProtocolError, error.to_string().as_bytes());
    }
    // Whatever ended it, the connection closes: the Coordinator never
    // waits on a Worker that has given up.
    closer.close(CloseCode::Finished, b"");
    let how = closer.closed().await;
    endpoint.wait_idle().await;
    let ending = match served {
        Ok(Served::Rejected(reason)) => Ending::Rejected(reason),
        Ok(Served::Ended) => ending(how, None),
        Err(error) => ending(how, Some(error)),
    };
    Attempt {
        ending,
        delivered: delivered.load(Ordering::Relaxed),
    }
}

/// Connects to the Coordinator at `address`; `None` if `stop` is made
/// first.
async fn connect(
    address: SocketAddr,
    stop: &Cancellation,
) -> Result<Option<(Endpoint, Connection)>, Error> {
    let endpoint = quic::worker_endpoint(address)?;
    let connecting = endpoint.connect(address, SERVER_NAME)?;
    tokio::select! {
        connection = connecting => Ok(Some((endpoint, connection?))),
        () = stopped(stop) => Ok(None),
    }
}

/// Returns once `stop` is made, which it checks at each tick.
async fn stopped(stop: &Cancellation) {
    let mut ticker = runtime::ticker();
    while !stop.is_cancelled() {
        ticker.tick().await;
    }
}

/// How a Worker's time in a Render Job ended, its connection having closed
/// as `closed` says, after `error` if the Worker met one.
fn ending(closed: Closed, error: Option<Error>) -> Ending {
    let error = |fallback: Error| error.unwrap_or(fallback);
    match closed {
        Closed::Here(CloseCode::Finished) | Closed::There(Ok(CloseCode::Finished)) => {
            Ending::Finished
        }
        Closed::Here(CloseCode::WorkerStopped) => Ending::Stopped,
        Closed::There(Ok(CloseCode::JobCancelled)) => Ending::Cancelled,
        Closed::Here(CloseCode::ProtocolError | CloseCode::Rejected)
        | Closed::There(Ok(CloseCode::ProtocolError | CloseCode::Rejected)) => {
            Ending::ProtocolError(error(Error::Closed("PROTOCOL_ERROR")))
        }
        Closed::There(Ok(CloseCode::WorkerTimedOut)) => Ending::Failed(Error::Closed(
            "WORKER_TIMED_OUT: no heartbeat arrived in time",
        )),
        Closed::Here(
            CloseCode::WorkerFatal | CloseCode::JobCancelled | CloseCode::WorkerTimedOut,
        )
        | Closed::There(Ok(CloseCode::WorkerFatal | CloseCode::WorkerStopped) | Err(_)) => {
            Ending::Failed(error(Error::Closed("an unexpected close")))
        }
        Closed::Lost => Ending::Failed(error(Error::Closed("the Coordinator is lost"))),
    }
}

/// How serving a Render Job ended, short of an error.
#[derive(Debug)]
enum Served {
    /// The Render Job ended for the Worker; its connection says how.
    Ended,
    /// The Coordinator refused the Worker.
    Rejected(Refusal),
}

/// Serves the Render Job of `closer`'s connection on `device`, counting
/// the Work Results it sends in `delivered`.
async fn serve(
    closer: &Arc<Closer>,
    device: &Arc<Device>,
    hello: &Hello,
    delivered: &Arc<AtomicU64>,
) -> Result<Served, Error> {
    let connection = closer.connection();
    let welcomed = Instant::now();
    let (mut control_send, control_recv) = match handshake(connection, hello).await? {
        Handshake::Welcomed(control) => control,
        Handshake::Rejected(reason) => {
            warn!("refused by the Coordinator: {reason}");
            closer.close(CloseCode::Rejected, b"");
            return Ok(Served::Rejected(reason));
        }
    };
    let activity = Arc::new(Activity::new());
    let heartbeats = tokio::spawn(heartbeat(connection.clone(), Arc::clone(&activity)));
    let received = receive_scene(connection, &activity, welcomed).await;
    let received = received.inspect_err(|_| heartbeats.abort())?;
    let (coordinator_end, worker_end) = transport::connect();
    let renders = {
        let (device, activity) = (Arc::clone(device), Arc::clone(&activity));
        thread::spawn(move || render(&device, received, worker_end, &activity))
    };
    let reader = tokio::spawn(read_control(
        Arc::clone(closer),
        control_recv,
        coordinator_end.to_worker.clone(),
    ));
    let mut outbox = Outbox::new(closer, delivered);
    let served = relay(
        &mut outbox,
        &mut control_send,
        coordinator_end.from_worker,
        &activity,
    )
    .await;
    heartbeats.abort();
    // The control stream's reader returns with the connection, or with the
    // protocol error it closed it for.
    let read = reader.await.unwrap_or(Ok(()));
    // The render thread has returned: its channel is closed.
    let _ = renders.join();
    served.and(read).map(|()| Served::Ended)
}

/// How the Coordinator answered `Hello`.
enum Handshake {
    /// `Welcome`: the control stream's two halves.
    Welcomed((SendStream, RecvStream)),
    /// `Reject`, for this reason.
    Rejected(Refusal),
}

/// `Hello`, answered by `Welcome` or `Reject`.
async fn handshake(connection: &Connection, hello: &Hello) -> Result<Handshake, Error> {
    let (mut send, mut recv) = connection.open_bi().await?;
    write_frame(&mut send, &FromWorker::Hello(Box::new(hello.clone()))).await?;
    match read_frame(&mut recv).await? {
        Some(FromCoordinator::Welcome { worker_id }) => {
            info!("joined the Render Job as Worker {worker_id}");
            Ok(Handshake::Welcomed((send, recv)))
        }
        Some(FromCoordinator::Reject { reason }) => Ok(Handshake::Rejected(reason)),
        Some(other) => Err(Error::OutOfOrder(other.name())),
        None => Err(Error::StreamEnded("the control stream")),
    }
}

/// Sends a heartbeat on a stream of its own every second, and as soon as
/// the Worker is ready, until the connection closes.
async fn heartbeat(connection: Connection, activity: Arc<Activity>) {
    let Ok(mut send) = connection.open_uni().await else {
        return;
    };
    let mut ticker = time::interval(HEARTBEAT);
    loop {
        tokio::select! {
            _ = ticker.tick() => {}
            () = activity.changed() => {}
        }
        if write_frame(&mut send, &activity.heartbeat()).await.is_err() {
            return;
        }
    }
}

/// What a Worker received of the Scene: the Render Job Options, the
/// Scene's builder, and how long and how big the transfer was.
struct Received {
    options: RenderJobOptions,
    builder: SceneBuilder,
    loaded: Loaded,
}

/// The Scene stream: the Render Job Options, then the Scene's builder,
/// then its end; its bytes counted on `activity` as they arrive, the time
/// since `welcomed`.
async fn receive_scene(
    connection: &Connection,
    activity: &Activity,
    welcomed: Instant,
) -> Result<Received, Error> {
    let mut recv = connection.accept_uni().await?;
    let mut bytes = Vec::new();
    while let Some(chunk) = recv.read_chunk(usize::MAX, true).await? {
        bytes.extend_from_slice(&chunk.bytes);
        if bytes.len() > wire::MAX_SCENE {
            return Err(Error::Oversized(bytes.len()));
        }
        let received = bytes.len() as u64;
        activity.loading(Loading::Receiving { bytes: received });
    }
    let size = bytes.len() as u64;
    let mut frames = wire::Frames::new(&bytes);
    let missing = || Error::StreamEnded("the Scene stream");
    let options = frames.next()?.ok_or_else(missing)?;
    let builder = frames.next()?.ok_or_else(missing)?;
    if frames.next::<()>()?.is_some() {
        return Err(Error::OutOfOrder("a frame after the Scene"));
    }
    let replication_time = welcomed.elapsed();
    info!(
        "Scene received: {size} bytes in {:.3} s",
        replication_time.as_secs_f64()
    );
    Ok(Received {
        options,
        builder,
        loaded: Loaded {
            bytes: size,
            replication_time,
            build_time: Duration::ZERO,
        },
    })
}

/// Builds the Scene `received` holds and the renderer of `device`, and
/// runs its render loop, on this thread: the runtime's thread keeps the
/// connection alive meanwhile, and a GPU's renderer stays on the thread
/// that made it. A Worker that cannot render tells the Coordinator why.
fn render(device: &Device, received: Received, connection: WorkerEnd, activity: &Activity) {
    let Received {
        options,
        builder,
        mut loaded,
    } = received;
    activity.loading(Loading::Building {
        bytes: loaded.bytes,
    });
    let start = Instant::now();
    let render_loop = RenderLoop {
        connection,
        activity,
    };
    match info_span!("scene_build").in_scope(|| builder.build()) {
        Ok(scene) => {
            loaded.build_time = start.elapsed();
            info!("Scene built in {:.3} s", loaded.build_time.as_secs_f64());
            render_scene(device, &scene, &options, render_loop, loaded);
        }
        Err(error) => render_loop.fatal(&Error::Scene(error)),
    }
}

/// What a render loop runs with: its end of the Worker's connection, and
/// the Worker's activity.
struct RenderLoop<'a> {
    connection: WorkerEnd,
    activity: &'a Activity,
}

impl RenderLoop<'_> {
    /// Tells the Coordinator why the Worker cannot continue.
    fn fatal(&self, error: &dyn std::error::Error) {
        let _ = self
            .connection
            .to_coordinator
            .send(ToCoordinator::Fatal(error.to_string()));
    }
}

/// Builds the renderer of `device` for `scene` and runs `render_loop`; the
/// Worker is ready, having loaded the Scene as `loaded` says, once the
/// renderer is.
fn render_scene(
    device: &Device,
    scene: &Scene,
    options: &RenderJobOptions,
    render_loop: RenderLoop<'_>,
    loaded: Loaded,
) {
    let RenderLoop {
        connection,
        activity,
    } = render_loop;
    match device {
        Device::Cpu(pool) => match umbra_cpu::Renderer::new(scene, options) {
            Ok(renderer) => {
                activity.loaded(loaded);
                cpu_worker::work(&renderer, pool, connection, activity);
            }
            Err(error) => RenderLoop {
                connection,
                activity,
            }
            .fatal(&error),
        },
        Device::Gpu(gpu) => match umbra_gpu::Renderer::new(usize::from(*gpu), scene, options) {
            Ok(renderer) => {
                activity.loaded(loaded);
                let to_coordinator = connection.to_coordinator.clone();
                if let gpu_worker::End::Failed(error) =
                    gpu_worker::work(&renderer, connection, activity)
                {
                    let _ = to_coordinator.send(ToCoordinator::Fatal(error.to_string()));
                }
            }
            Err(error) => RenderLoop {
                connection,
                activity,
            }
            .fatal(&error),
        },
    }
}

/// Passes the Coordinator's control messages to the render loop; once the
/// stream ends, the connection is closing, and the render loop stops. A
/// malformed or out-of-order message closes the connection with
/// `PROTOCOL_ERROR`.
async fn read_control(
    closer: Arc<Closer>,
    mut recv: RecvStream,
    to_worker: UnboundedSender<ToWorker>,
) -> Result<(), Error> {
    // A send fails only once the render loop has returned.
    let result = loop {
        match read_frame(&mut recv).await {
            Ok(Some(FromCoordinator::Assign { work_unit })) => {
                let _ = to_worker.send(ToWorker::Assign(work_unit));
            }
            Ok(Some(FromCoordinator::Cancel { work_unit })) => {
                let _ = to_worker.send(ToWorker::Cancel(work_unit));
            }
            Ok(Some(FromCoordinator::JobFinished)) => {
                let _ = to_worker.send(ToWorker::JobFinished);
            }
            Ok(Some(
                other @ (FromCoordinator::Welcome { .. } | FromCoordinator::Reject { .. }),
            )) => {
                break Err(Error::OutOfOrder(other.name()));
            }
            Err(error) if error.is_protocol_violation() => break Err(error),
            Ok(None) | Err(_) => break Ok(()),
        }
    };
    if let Err(error) = &result {
        closer.close(CloseCode::ProtocolError, error.to_string().as_bytes());
    }
    let _ = to_worker.send(ToWorker::Stop);
    result
}

/// Sends the render loop's messages to the Coordinator through `outbox`
/// until the loop returns, then closes the connection as [`Closing`] says.
async fn relay(
    outbox: &mut Outbox<'_>,
    control: &mut SendStream,
    mut from_worker: UnboundedReceiver<ToCoordinator>,
    activity: &Activity,
) -> Result<(), Error> {
    let mut summary = time::interval(SUMMARY);
    summary.tick().await;
    loop {
        tokio::select! {
            message = from_worker.recv() => match message {
                Some(message) => outbox.send(control, message).await,
                None => break,
            },
            _ = summary.tick() => info!(
                "{} Work Units in flight, {} Work Results sent",
                activity.in_flight(),
                outbox.delivered.load(Ordering::Relaxed)
            ),
        }
    }
    outbox.close().await
}

/// Sends `result` after `header` on a new stream of `connection`, and
/// counts it in `delivered` once sent.
async fn upload(
    connection: Connection,
    header: WorkResultHeader,
    result: WorkResult,
    delivered: Arc<AtomicU64>,
) -> Result<(), Error> {
    let send = connection.open_uni().await?;
    write_work_result(send, &header, &result).await?;
    delivered.fetch_add(1, Ordering::Relaxed);
    Ok(())
}

/// How a Worker closes its connection once its render loop returns.
#[derive(Debug)]
enum Closing {
    /// `FINISHED`: the Render Job ended, finished or cancelled.
    Finished,
    /// `WORKER_FATAL`, after `Fatal` with this reason.
    Fatal(String),
}

/// What a Worker sends the Coordinator: control messages on the control
/// stream, each Work Result on a stream of its own.
struct Outbox<'c> {
    closer: &'c Closer,
    uploads: JoinSet<Result<(), Error>>,
    closing: Closing,
    delivered: Arc<AtomicU64>,
}

impl<'c> Outbox<'c> {
    /// Sends through `closer`'s connection, counting the Work Results sent
    /// in `delivered`.
    fn new(closer: &'c Closer, delivered: &Arc<AtomicU64>) -> Self {
        Self {
            closer,
            uploads: JoinSet::new(),
            closing: Closing::Finished,
            delivered: Arc::clone(delivered),
        }
    }

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
                let connection = self.closer.connection().clone();
                let upload = upload(connection, header, result, Arc::clone(&self.delivered));
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

    /// Waits for every Work Result to be sent, then closes the connection,
    /// with the reason of a `Fatal` in the close itself: closing drops
    /// whatever a stream has not sent yet.
    async fn close(&mut self) -> Result<(), Error> {
        while self.uploads.join_next().await.is_some() {}
        match &self.closing {
            Closing::Finished => {
                self.closer.close(CloseCode::Finished, b"");
                Ok(())
            }
            Closing::Fatal(message) => {
                self.closer
                    .close(CloseCode::WorkerFatal, message.as_bytes());
                Err(Error::Fatal(message.clone()))
            }
        }
    }
}
