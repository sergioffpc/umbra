//! The Coordinator's side of QUIC: it admits each Worker that connects,
//! streams it the Scene, and turns its connection into [`Event`]s (arc42
//! §8.13).

use std::sync::Arc;

use quinn::{Connection, ConnectionError, Endpoint, Incoming, RecvStream, SendStream};
use tokio::sync::mpsc::{UnboundedReceiver, UnboundedSender, unbounded_channel};
use tokio::task::JoinSet;
use tracing::{debug, warn};
use umbra_render::{Decomposition, WorkResult};
use umbra_scene::{RenderJobOptions, SceneBuilder};

use crate::device::RenderDevice;
use crate::error::Error;
use crate::transport::{Event, ToCoordinator, ToWorker, WorkerId};
use crate::wire::{
    self, CloseCode, FromCoordinator, FromWorker, read_frame, read_work_result, write_frame,
};

/// What every Worker that joins is sent, and what its Work Results are
/// checked against.
#[derive(Debug)]
pub(crate) struct Replica {
    /// The Scene stream: the Render Job Options, then the Scene's builder,
    /// each one frame.
    scene_stream: Vec<u8>,
    decomposition: Decomposition,
}

impl Replica {
    /// What a Worker needs of a Render Job of the Scene `builder` builds,
    /// with `options`, cut as `decomposition`.
    pub(crate) fn new(
        options: &RenderJobOptions,
        builder: &SceneBuilder,
        decomposition: Decomposition,
    ) -> Self {
        let encode = |error| panic!("plain data always encodes: {error}");
        let options = wire::frame(options).unwrap_or_else(encode);
        let builder = wire::frame(builder).unwrap_or_else(encode);
        Self {
            scene_stream: [options, builder].concat(),
            decomposition,
        }
    }
}

/// Admits every Worker that connects to `endpoint`, numbering them from
/// `first`, until the endpoint closes.
pub(crate) async fn listen(
    endpoint: Endpoint,
    replica: Arc<Replica>,
    first: WorkerId,
    events: UnboundedSender<Event>,
) {
    let mut next = first;
    let mut connections = JoinSet::new();
    while let Some(incoming) = endpoint.accept().await {
        connections.spawn(serve(incoming, next, Arc::clone(&replica), events.clone()));
        next = next.next();
    }
    connections.join_all().await;
}

/// Serves the Worker of `incoming` as `worker` until its connection
/// closes.
async fn serve(
    incoming: Incoming,
    worker: WorkerId,
    replica: Arc<Replica>,
    events: UnboundedSender<Event>,
) {
    let connection = match incoming.await {
        Ok(connection) => connection,
        Err(error) => {
            warn!("a Worker could not connect: {error}");
            return;
        }
    };
    match admit(&connection, worker, &replica, &events).await {
        Ok(joined) => relay(&connection, joined, worker, replica, &events).await,
        Err(error) => close(&connection, worker, &error),
    }
}

/// Relays between the joined `worker` and the Coordinator until its
/// connection closes, then lets the Coordinator know it left.
async fn relay(
    connection: &Connection,
    joined: Joined,
    worker: WorkerId,
    replica: Arc<Replica>,
    events: &UnboundedSender<Event>,
) {
    let Joined {
        device,
        control_send,
        control_recv,
        from_coordinator,
    } = joined;
    let mut readers = JoinSet::new();
    readers.spawn(read_control(
        connection.clone(),
        control_recv,
        worker,
        events.clone(),
    ));
    readers.spawn(read_work_results(
        connection.clone(),
        worker,
        Arc::clone(&replica),
        events.clone(),
    ));
    let reason = write(connection, control_send, from_coordinator, worker).await;
    readers.join_all().await;
    match CloseReason::from(&reason) {
        CloseReason::Expected => debug!("Worker {worker} on {device} left: {reason}"),
        CloseReason::Unexpected => warn!("Worker {worker} on {device} lost: {reason}"),
    }
    // The Coordinator's loop outlives every connection: a send fails only
    // as it returns, when nothing is left to tell.
    let _ = events.send(Event::Left { worker });
}

/// A Worker admitted: its Render Device, the control stream's two halves,
/// and the channel the Coordinator's messages to it arrive on.
struct Joined {
    device: RenderDevice,
    control_send: SendStream,
    control_recv: RecvStream,
    from_coordinator: UnboundedReceiver<ToWorker>,
}

/// The handshake: `Hello`, `Welcome`, then the Scene stream; the Worker
/// joins once the Scene is sent.
async fn admit(
    connection: &Connection,
    worker: WorkerId,
    replica: &Replica,
    events: &UnboundedSender<Event>,
) -> Result<Joined, Error> {
    let (mut send, mut recv) = connection.accept_bi().await?;
    let device = match read_frame(&mut recv).await? {
        Some(FromWorker::Hello { render_device }) => render_device,
        Some(other) => return Err(Error::OutOfOrder(other.name())),
        None => return Err(Error::StreamEnded("the control stream")),
    };
    write_frame(&mut send, &FromCoordinator::Welcome { worker_id: worker }).await?;
    let mut scene = connection.open_uni().await?;
    scene.write_all(&replica.scene_stream).await?;
    scene.finish()?;
    let (to_worker, from_coordinator) = unbounded_channel();
    let _ = events.send(Event::Joined {
        worker,
        device,
        to_worker,
    });
    Ok(Joined {
        device,
        control_send: send,
        control_recv: recv,
        from_coordinator,
    })
}

/// Sends the Coordinator's messages on the control stream until the
/// connection closes, closing it on a cancellation; returns why it closed.
async fn write(
    connection: &Connection,
    mut send: SendStream,
    mut from_coordinator: UnboundedReceiver<ToWorker>,
    worker: WorkerId,
) -> ConnectionError {
    loop {
        tokio::select! {
            reason = connection.closed() => return reason,
            message = from_coordinator.recv() => {
                let frame = match message {
                    Some(ToWorker::Assign(work_unit)) => FromCoordinator::Assign { work_unit },
                    Some(ToWorker::JobFinished) => FromCoordinator::JobFinished,
                    Some(ToWorker::Cancel) => {
                        connection.close(CloseCode::JobCancelled.into(), b"");
                        return connection.closed().await;
                    }
                    // The Coordinator no longer reaches this Worker: it
                    // waits for the connection to close by itself.
                    None => return connection.closed().await,
                };
                if let Err(error) = write_frame(&mut send, &frame).await {
                    close(connection, worker, &error);
                }
            }
        }
    }
}

/// Forwards the Worker's control messages until the stream ends.
async fn read_control(
    connection: Connection,
    mut recv: RecvStream,
    worker: WorkerId,
    events: UnboundedSender<Event>,
) {
    loop {
        let message = match read_frame(&mut recv).await {
            Ok(Some(FromWorker::RequestWork { count })) => ToCoordinator::RequestWork(count),
            Ok(Some(FromWorker::Fatal { message })) => ToCoordinator::Fatal(message),
            Ok(Some(hello @ FromWorker::Hello { .. })) => {
                return close(&connection, worker, &Error::OutOfOrder(hello.name()));
            }
            Ok(None) => {
                return close(
                    &connection,
                    worker,
                    &Error::StreamEnded("the control stream"),
                );
            }
            Err(error) => return close(&connection, worker, &error),
        };
        let _ = events.send(Event::Message { worker, message });
    }
}

/// Reads each Work Result stream the Worker opens, concurrently.
async fn read_work_results(
    connection: Connection,
    worker: WorkerId,
    replica: Arc<Replica>,
    events: UnboundedSender<Event>,
) {
    let mut streams = JoinSet::new();
    while let Ok(recv) = connection.accept_uni().await {
        streams.spawn(read_work_result_stream(
            recv,
            connection.clone(),
            worker,
            Arc::clone(&replica),
            events.clone(),
        ));
    }
    streams.join_all().await;
}

/// Forwards the Work Result on `recv` once checked against the Render Job,
/// or closes the connection of `worker` if it does not answer one of its
/// Work Units.
async fn read_work_result_stream(
    recv: RecvStream,
    connection: Connection,
    worker: WorkerId,
    replica: Arc<Replica>,
    events: UnboundedSender<Event>,
) {
    match read_checked(recv, &replica.decomposition).await {
        Ok((result, render_time)) => {
            let message = ToCoordinator::WorkResult {
                result,
                render_time,
            };
            let _ = events.send(Event::Message { worker, message });
        }
        Err(error) => close(&connection, worker, &error),
    }
}

/// The Work Result on `recv`, if it answers a Work Unit of the Render Job
/// with one sum per pixel of its Tile.
async fn read_checked(
    recv: RecvStream,
    decomposition: &Decomposition,
) -> Result<(WorkResult, std::time::Duration), Error> {
    let (result, render_time) = read_work_result(recv).await?;
    let unit = result.unit();
    if !decomposition.contains(unit) {
        return Err(Error::UnknownWorkUnit(unit));
    }
    let pixels = result.pixels().len();
    if pixels != decomposition.tile_bounds(unit.tile).area() {
        return Err(Error::WrongSize { unit, pixels });
    }
    Ok((result, render_time))
}

/// Closes the connection of `worker` for `error`, unless it is closed
/// already, which is what most read errors mean.
fn close(connection: &Connection, worker: WorkerId, error: &Error) {
    if connection.close_reason().is_none() {
        warn!("Worker {worker}: {error}: closing it");
        connection.close(CloseCode::ProtocolError.into(), b"");
    }
}

/// Whether a connection closed as a Render Job ends, or lost its Worker.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum CloseReason {
    Expected,
    Unexpected,
}

impl From<&ConnectionError> for CloseReason {
    fn from(reason: &ConnectionError) -> Self {
        let code = match reason {
            ConnectionError::ApplicationClosed(close) => CloseCode::try_from(close.error_code),
            ConnectionError::LocallyClosed => return Self::Expected,
            ConnectionError::VersionMismatch
            | ConnectionError::TransportError(_)
            | ConnectionError::ConnectionClosed(_)
            | ConnectionError::Reset
            | ConnectionError::TimedOut
            | ConnectionError::CidsExhausted => return Self::Unexpected,
        };
        match code {
            Ok(CloseCode::Finished | CloseCode::JobCancelled) => Self::Expected,
            Ok(CloseCode::WorkerFatal | CloseCode::WorkerStopped | CloseCode::ProtocolError)
            | Err(_) => Self::Unexpected,
        }
    }
}
