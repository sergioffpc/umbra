//! The Coordinator's side of QUIC: it hears each Worker that connects, has
//! the Coordinator admit or refuse it, streams it the Scene, watches its
//! heartbeats, and turns its connection into [`Event`]s (arc42 §8.13).

use std::collections::BTreeSet;
use std::sync::{Arc, Mutex, PoisonError};
use std::time::Duration;

use quinn::{Endpoint, Incoming, RecvStream, SendStream, VarInt};
use tokio::sync::mpsc::{UnboundedReceiver, UnboundedSender, unbounded_channel};
use tokio::sync::oneshot;
use tokio::task::JoinSet;
use tokio::time;
use tracing::{debug, warn};
use umbra_render::{Decomposition, WorkResult, WorkUnit};
use umbra_scene::{RenderJobOptions, SceneBuilder};

use crate::error::Error;
use crate::quic::{Closed, Closer};
use crate::transport::{Departure, Event, ToCoordinator, ToWorker, Verdict, WorkerId};
use crate::wire::{
    self, CloseCode, FromCoordinator, FromWorker, Heartbeat, WorkResultHeader, read_frame,
    read_work_result_body, read_work_result_header, write_frame,
};

/// How long a refused Worker is given to close by itself, once it has read
/// its `Reject`, before the Coordinator closes its connection.
const REJECT_WAIT: Duration = Duration::from_secs(2);

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

/// What the listener serves every Worker with: the Render Job's replica,
/// and the silence after which a Worker is dead.
#[derive(Debug)]
pub(crate) struct Serving {
    pub(crate) replica: Replica,
    /// No heartbeat for this long, and the Worker is dead (arc42 §8.8).
    pub(crate) death_time: Duration,
}

/// Hears every Worker that connects to `endpoint`, numbering them in the
/// order they connect, until the endpoint closes.
pub(crate) async fn listen(
    endpoint: Endpoint,
    serving: Arc<Serving>,
    events: UnboundedSender<Event>,
) {
    let mut next = WorkerId(0);
    let mut connections = JoinSet::new();
    while let Some(incoming) = endpoint.accept().await {
        connections.spawn(serve(incoming, next, Arc::clone(&serving), events.clone()));
        next = next.next();
    }
    connections.join_all().await;
}

/// Serves the Worker of `incoming` as `worker` until its connection
/// closes.
async fn serve(
    incoming: Incoming,
    worker: WorkerId,
    serving: Arc<Serving>,
    events: UnboundedSender<Event>,
) {
    let connection = match incoming.await {
        Ok(connection) => connection,
        Err(error) => {
            warn!("a Worker could not connect: {error}");
            return;
        }
    };
    let closer = Arc::new(Closer::new(connection));
    match admit(&closer, worker, &serving, &events).await {
        Ok(Admission::Welcomed(admitted)) => {
            relay(&closer, admitted, worker, serving, &events).await;
        }
        Ok(Admission::Rejected) => {}
        Err(error) => protocol_error(&closer, worker, &error),
    }
}

/// Whether a Worker that said `Hello` was admitted.
enum Admission {
    Welcomed(Admitted),
    Rejected,
}

/// A Worker admitted: the control stream's two halves, and the channel the
/// Coordinator's messages to it arrive on.
struct Admitted {
    control_send: SendStream,
    control_recv: RecvStream,
    from_coordinator: UnboundedReceiver<ToWorker>,
}

/// The handshake: `Hello`, the Coordinator's verdict, then `Welcome` and
/// the Scene stream, or `Reject`.
async fn admit(
    closer: &Closer,
    worker: WorkerId,
    serving: &Serving,
    events: &UnboundedSender<Event>,
) -> Result<Admission, Error> {
    let connection = closer.connection();
    let (mut send, mut recv) = connection.accept_bi().await?;
    let hello = match read_frame(&mut recv).await? {
        Some(FromWorker::Hello(hello)) => hello,
        Some(other) => return Err(Error::OutOfOrder(other.name())),
        None => return Err(Error::StreamEnded("the control stream")),
    };
    let (to_worker, from_coordinator) = unbounded_channel();
    let (verdict, heard) = oneshot::channel();
    let _ = events.send(Event::Arrived {
        worker,
        hello,
        to_worker,
        verdict,
    });
    // The Coordinator's loop outlives every connection: no verdict means it
    // is ending, and the Worker is refused nothing, only dropped.
    let Ok(verdict) = heard.await else {
        return Ok(Admission::Rejected);
    };
    match verdict {
        Verdict::Welcome => {
            write_frame(&mut send, &FromCoordinator::Welcome { worker_id: worker }).await?;
            let mut scene = connection.open_uni().await?;
            scene.write_all(&serving.replica.scene_stream).await?;
            scene.finish()?;
            Ok(Admission::Welcomed(Admitted {
                control_send: send,
                control_recv: recv,
                from_coordinator,
            }))
        }
        Verdict::Reject(reason) => {
            write_frame(&mut send, &FromCoordinator::Reject { reason }).await?;
            send.finish()?;
            // The Worker closes once it has read it; the Coordinator closes
            // after a while if it does not, which also ends a Worker that
            // never reads.
            let _ = time::timeout(REJECT_WAIT, connection.closed()).await;
            closer.close(CloseCode::Rejected, b"");
            Ok(Admission::Rejected)
        }
    }
}

/// An admitted Worker's connection, as its readers and its writer share it:
/// the Work Units it was told to drop, whose Work Results are not read.
struct Link {
    closer: Arc<Closer>,
    worker: WorkerId,
    events: UnboundedSender<Event>,
    cancelled: Mutex<BTreeSet<WorkUnit>>,
}

impl Link {
    /// Whether the Worker was told to drop `unit`.
    fn is_cancelled(&self, unit: WorkUnit) -> bool {
        self.cancelled
            .lock()
            .unwrap_or_else(PoisonError::into_inner)
            .contains(&unit)
    }

    /// The Worker is told to drop `unit`.
    fn cancel(&self, unit: WorkUnit) {
        self.cancelled
            .lock()
            .unwrap_or_else(PoisonError::into_inner)
            .insert(unit);
    }

    /// Passes `event` on to the Coordinator.
    fn send(&self, event: Event) {
        // The Coordinator's loop outlives every connection: a send fails
        // only as it returns, when nothing is left to tell.
        let _ = self.events.send(event);
    }

    /// Closes the connection for `error`, unless it is closed already,
    /// which is what most read errors mean.
    fn protocol_error(&self, error: &Error) {
        protocol_error(&self.closer, self.worker, error);
    }
}

/// Relays between the admitted `worker` and the Coordinator until its
/// connection closes, then lets the Coordinator know it left, and why.
async fn relay(
    closer: &Arc<Closer>,
    admitted: Admitted,
    worker: WorkerId,
    serving: Arc<Serving>,
    events: &UnboundedSender<Event>,
) {
    let Admitted {
        control_send,
        control_recv,
        from_coordinator,
    } = admitted;
    let link = Arc::new(Link {
        closer: Arc::clone(closer),
        worker,
        events: events.clone(),
        cancelled: Mutex::new(BTreeSet::new()),
    });
    let mut readers = JoinSet::new();
    readers.spawn(read_control(Arc::clone(&link), control_recv));
    readers.spawn(read_streams(Arc::clone(&link), serving));
    let how = write(&link, control_send, from_coordinator).await;
    readers.join_all().await;
    let departure = departure(how);
    match departure {
        Departure::Ended | Departure::Stopped => debug!("Worker {worker} left: {departure}"),
        Departure::Failed | Departure::ProtocolError => {
            warn!("Worker {worker} lost: {departure} ({how:?})");
        }
    }
    link.send(Event::Left { worker, departure });
}

/// What a Worker's connection closing as `how` says means for its Worker
/// Slot.
fn departure(how: Closed) -> Departure {
    match how {
        Closed::Here(CloseCode::Finished | CloseCode::JobCancelled)
        | Closed::There(Ok(CloseCode::Finished | CloseCode::JobCancelled)) => Departure::Ended,
        Closed::There(Ok(CloseCode::WorkerStopped)) => Departure::Stopped,
        Closed::Here(CloseCode::ProtocolError) | Closed::There(Ok(CloseCode::ProtocolError)) => {
            Departure::ProtocolError
        }
        Closed::Here(
            CloseCode::WorkerTimedOut
            | CloseCode::Rejected
            | CloseCode::WorkerFatal
            | CloseCode::WorkerStopped,
        )
        | Closed::There(
            Ok(CloseCode::WorkerFatal | CloseCode::WorkerTimedOut | CloseCode::Rejected) | Err(_),
        )
        | Closed::Lost => Departure::Failed,
    }
}

/// Sends the Coordinator's messages on the control stream until the
/// connection closes, closing it on a cancellation; returns how it closed.
async fn write(
    link: &Link,
    mut send: SendStream,
    mut from_coordinator: UnboundedReceiver<ToWorker>,
) -> Closed {
    let closer = &link.closer;
    loop {
        tokio::select! {
            how = closer.closed() => return how,
            message = from_coordinator.recv() => {
                let frame = match message {
                    Some(ToWorker::Assign(work_unit)) => FromCoordinator::Assign { work_unit },
                    Some(ToWorker::Cancel(work_unit)) => {
                        link.cancel(work_unit);
                        FromCoordinator::Cancel { work_unit }
                    }
                    Some(ToWorker::JobFinished) => FromCoordinator::JobFinished,
                    Some(ToWorker::Stop) => {
                        closer.close(CloseCode::JobCancelled, b"");
                        return closer.closed().await;
                    }
                    // The Coordinator no longer reaches this Worker: it
                    // waits for the connection to close by itself.
                    None => return closer.closed().await,
                };
                // A failed write means a closed connection, which the next
                // turn sees.
                let _ = write_frame(&mut send, &frame).await;
            }
        }
    }
}

/// Forwards the Worker's control messages until the stream ends.
async fn read_control(link: Arc<Link>, mut recv: RecvStream) {
    let worker = link.worker;
    loop {
        let message = match read_frame(&mut recv).await {
            Ok(Some(FromWorker::RequestWork { count })) => ToCoordinator::RequestWork(count),
            Ok(Some(FromWorker::Fatal { message })) => ToCoordinator::Fatal(message),
            Ok(Some(hello @ FromWorker::Hello(_))) => {
                return link.protocol_error(&Error::OutOfOrder(hello.name()));
            }
            Ok(None) => return link.protocol_error(&Error::StreamEnded("the control stream")),
            Err(error) => return link.protocol_error(&error),
        };
        link.send(Event::Message { worker, message });
    }
}

/// Reads the Worker's streams: its heartbeat stream, the first it opens,
/// then each Work Result stream, concurrently.
async fn read_streams(link: Arc<Link>, serving: Arc<Serving>) {
    let connection = link.closer.connection().clone();
    let mut streams = JoinSet::new();
    let death_time = serving.death_time;
    match time::timeout(death_time, connection.accept_uni()).await {
        Ok(Ok(heartbeats)) => {
            streams.spawn(read_heartbeats(Arc::clone(&link), heartbeats, death_time));
        }
        Ok(Err(_)) => return,
        Err(_) => return timed_out(&link),
    }
    while let Ok(recv) = connection.accept_uni().await {
        streams.spawn(read_work_result_stream(
            recv,
            Arc::clone(&link),
            Arc::clone(&serving),
        ));
    }
    streams.join_all().await;
}

/// Forwards each heartbeat on `recv`; after `death_time` without one, the
/// Worker is dead, and its connection closes with `WORKER_TIMED_OUT`.
async fn read_heartbeats(link: Arc<Link>, mut recv: RecvStream, death_time: Duration) {
    let worker = link.worker;
    loop {
        match time::timeout(death_time, read_frame::<Heartbeat>(&mut recv)).await {
            Ok(Ok(Some(heartbeat))) => link.send(Event::Heartbeat { worker, heartbeat }),
            // The stream ends as the connection closes.
            Ok(Ok(None)) => return,
            Ok(Err(error)) => return link.protocol_error(&error),
            Err(_) => return timed_out(&link),
        }
    }
}

/// Declares the Worker of `link` dead: no heartbeat for the death time.
fn timed_out(link: &Link) {
    if link.closer.connection().close_reason().is_none() {
        warn!("Worker {}: no heartbeat: declared dead", link.worker);
        link.closer.close(CloseCode::WorkerTimedOut, b"");
    }
}

/// Forwards the Work Result on `recv` once checked against the Render Job,
/// or closes the connection if it does not answer one of its Work Units.
/// One whose Work Unit the Worker was told to drop is not read: its stream
/// is stopped (`stop_sending`), and nothing is forwarded.
async fn read_work_result_stream(mut recv: RecvStream, link: Arc<Link>, serving: Arc<Serving>) {
    let header = match read_work_result_header(&mut recv).await {
        Ok(header) => header,
        Err(error) => return link.protocol_error(&error),
    };
    if link.is_cancelled(header.work_unit) {
        // The stream may have ended already: then there is nothing to stop.
        let _ = recv.stop(VarInt::from_u32(0));
        return;
    }
    match read_checked(recv, &header, &serving.replica.decomposition).await {
        Ok(result) => {
            let message = ToCoordinator::WorkResult {
                result,
                render_time: header.render_time,
                thread_time: header.thread_time,
            };
            link.send(Event::Message {
                worker: link.worker,
                message,
            });
        }
        Err(error) => link.protocol_error(&error),
    }
}

/// The pixel sums on `recv` of the Work Result `header` heads, if it
/// answers a Work Unit of the Render Job with one sum per pixel of its
/// Tile.
async fn read_checked(
    recv: RecvStream,
    header: &WorkResultHeader,
    decomposition: &Decomposition,
) -> Result<WorkResult, Error> {
    let unit = header.work_unit;
    if !decomposition.contains(unit) {
        return Err(Error::UnknownWorkUnit(unit));
    }
    let result = read_work_result_body(recv, header).await?;
    let pixels = result.pixels().len();
    if pixels != decomposition.tile_bounds(unit.tile).area() {
        return Err(Error::WrongSize { unit, pixels });
    }
    Ok(result)
}

/// Closes the connection of `worker` for `error`, unless it is closed
/// already, which is what most read errors mean.
fn protocol_error(closer: &Closer, worker: WorkerId, error: &Error) {
    if closer.connection().close_reason().is_none() {
        warn!("Worker {worker}: {error}: closing it");
        closer.close(CloseCode::ProtocolError, b"");
    }
}
