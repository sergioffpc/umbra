//! The wire protocol between the Coordinator and a Worker over QUIC (arc42
//! §8.13, ADR 0016): the messages, how each is framed on its stream, and
//! the codes a connection closes with.
//!
//! Control messages, the Scene stream's frames and Work Result headers are
//! `postcard` behind a little-endian `u32` length; a Work Result's pixel
//! sums follow its header as raw little-endian bytes, to the end of its
//! stream. There is no protocol version: both sides are on the same commit
//! (ADR 0012).

use std::time::Duration;

use quinn::{RecvStream, SendStream, VarInt};
use serde::de::DeserializeOwned;
use serde::{Deserialize, Serialize};
use umbra_render::{PixelSum, WorkResult, WorkUnit};

use crate::device::RenderDevice;
use crate::error::Error;
use crate::transport::WorkerId;

/// The largest frame either side reads: far above the largest Scene
/// structure so far, and a bound on what a corrupt length prefix allocates.
const MAX_FRAME: usize = 64 << 20;

/// The largest Work Result body a Worker sends: the pixel sums of a Tile
/// of 4096 × 4096 pixels.
const MAX_BODY: usize = 4096 * 4096 * size_of::<PixelSum>();

/// What a Worker sends on the control stream.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub(crate) enum FromWorker {
    /// The first message: which Render Device the Worker renders on.
    Hello { render_device: RenderDevice },
    /// Add `count` Work Units to the Worker's demand; the first is the
    /// Worker's readiness.
    RequestWork { count: u32 },
    /// The Worker cannot continue: it closes next, with `WORKER_FATAL`.
    Fatal { message: String },
}

impl FromWorker {
    /// The message's name, for a protocol error.
    pub(crate) fn name(&self) -> &'static str {
        match self {
            Self::Hello { .. } => "Hello",
            Self::RequestWork { .. } => "RequestWork",
            Self::Fatal { .. } => "Fatal",
        }
    }
}

/// What the Coordinator sends on the control stream.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub(crate) enum FromCoordinator {
    /// The Worker is admitted, under this identifier.
    Welcome { worker_id: WorkerId },
    /// Render this Work Unit.
    Assign { work_unit: WorkUnit },
    /// Every Work Unit is merged: the Worker closes with `FINISHED`.
    JobFinished,
}

impl FromCoordinator {
    /// The message's name, for a protocol error.
    pub(crate) fn name(self) -> &'static str {
        match self {
            Self::Welcome { .. } => "Welcome",
            Self::Assign { .. } => "Assign",
            Self::JobFinished => "JobFinished",
        }
    }
}

/// What precedes a Work Result's pixel sums on its stream.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub(crate) struct WorkResultHeader {
    pub(crate) work_unit: WorkUnit,
    /// How long the Worker took to render it (arc42 §8.10).
    pub(crate) render_time: Duration,
}

/// Why a connection closed, as its QUIC application close code.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum CloseCode {
    /// The Render Job finished; the Worker closes.
    Finished = 0,
    /// The Coordinator's operator cancelled the Render Job.
    JobCancelled = 1,
    /// The Worker could not continue, and said why with `Fatal`.
    WorkerFatal = 3,
    /// The Worker's operator stopped it.
    WorkerStopped = 5,
    /// A malformed or out-of-order message: always a bug.
    ProtocolError = 6,
}

impl From<CloseCode> for VarInt {
    fn from(code: CloseCode) -> Self {
        Self::from(code as u32)
    }
}

impl TryFrom<VarInt> for CloseCode {
    /// The code itself, which is not one of Umbra's.
    type Error = VarInt;

    fn try_from(code: VarInt) -> Result<Self, Self::Error> {
        [
            Self::Finished,
            Self::JobCancelled,
            Self::WorkerFatal,
            Self::WorkerStopped,
            Self::ProtocolError,
        ]
        .into_iter()
        .find(|&known| VarInt::from(known) == code)
        .ok_or(code)
    }
}

/// Writes `message` as one frame.
pub(crate) async fn write_frame<T: Serialize>(
    send: &mut SendStream,
    message: &T,
) -> Result<(), Error> {
    send.write_all(&frame(message)?).await?;
    Ok(())
}

/// `message` as one frame: its length, then its encoding.
pub(crate) fn frame<T: Serialize>(message: &T) -> Result<Vec<u8>, Error> {
    let body = postcard::to_stdvec(message)?;
    let len = u32::try_from(body.len()).map_err(|_| Error::Oversized(body.len()))?;
    Ok([len.to_le_bytes().as_slice(), &body].concat())
}

/// Reads the next frame as a `T`; `None` at the end of the stream.
pub(crate) async fn read_frame<T: DeserializeOwned>(
    recv: &mut RecvStream,
) -> Result<Option<T>, Error> {
    let mut len = [0; 4];
    match recv.read_exact(&mut len).await {
        Ok(()) => {}
        Err(quinn::ReadExactError::FinishedEarly(0)) => return Ok(None),
        Err(error) => return Err(error.into()),
    }
    let len = usize::try_from(u32::from_le_bytes(len)).expect("a u32 fits in a usize");
    if len > MAX_FRAME {
        return Err(Error::Oversized(len));
    }
    let mut body = vec![0; len];
    recv.read_exact(&mut body).await?;
    Ok(Some(postcard::from_bytes(&body)?))
}

/// Writes `result`, rendered in `render_time`, on its own stream, and
/// ends it.
pub(crate) async fn write_work_result(
    mut send: SendStream,
    result: &WorkResult,
    render_time: Duration,
) -> Result<(), Error> {
    let header = WorkResultHeader {
        work_unit: result.unit(),
        render_time,
    };
    write_frame(&mut send, &header).await?;
    send.write_all(bytemuck::cast_slice(result.pixels()))
        .await?;
    send.finish()?;
    Ok(())
}

/// Reads a Work Result from its stream, and its render time.
pub(crate) async fn read_work_result(
    mut recv: RecvStream,
) -> Result<(WorkResult, Duration), Error> {
    let header: WorkResultHeader = read_frame(&mut recv)
        .await?
        .ok_or(Error::StreamEnded("a Work Result stream"))?;
    let body = recv.read_to_end(MAX_BODY).await?;
    if !body.len().is_multiple_of(size_of::<PixelSum>()) {
        return Err(Error::Body(body.len()));
    }
    let pixels = bytemuck::pod_collect_to_vec(&body);
    Ok((
        WorkResult::new(header.work_unit, pixels),
        header.render_time,
    ))
}
