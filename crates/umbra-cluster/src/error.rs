//! Why a Render Job did not finish, or a Worker could not serve one.

use std::io;

use rayon::ThreadPoolBuildError;
use thiserror::Error;

use umbra_render::WorkUnit;

/// Why a Render Job did not finish, or a Worker could not serve one.
#[derive(Debug, Error)]
#[non_exhaustive]
pub enum Error {
    /// The Render Job cannot be cut into Work Units.
    #[error("cannot render the Render Job: {0}")]
    Render(#[from] umbra_render::Error),
    /// The runtime could not start.
    #[error("cannot start the runtime: {0}")]
    Runtime(#[source] io::Error),
    /// A CPU Worker's threads could not start.
    #[error("cannot start the CPU Worker's threads: {0}")]
    ThreadPool(#[source] ThreadPoolBuildError),
    /// Every Worker went away before the last Work Unit was merged.
    #[error("every Worker went away before the end")]
    NoWorkers,
    /// A GPU was asked for, and none is usable.
    #[error("no usable GPU: {0}")]
    NoGpu(#[source] umbra_gpu::Error),
    /// A Render Device that is neither `cpu` nor `gpu:N`.
    #[error("unknown Render Device `{0}`: expected `cpu` or `gpu:N`")]
    UnknownRenderDevice(String),
    /// The QUIC endpoint's UDP socket could not open.
    #[error("cannot open a UDP socket: {0}")]
    Listen(#[source] io::Error),
    /// The Coordinator's self-signed certificate could not be made.
    #[error("cannot make the Coordinator's certificate: {0}")]
    Certificate(#[from] rcgen::Error),
    /// The TLS that QUIC requires could not be set up.
    #[error("cannot set up TLS: {0}")]
    Tls(#[from] rustls::Error),
    /// The TLS set up has no cipher suite that QUIC can start with.
    #[error("cannot set up TLS for QUIC: {0}")]
    QuicTls(#[from] quinn::crypto::rustls::NoInitialCipherSuite),
    /// The Worker could not start connecting to the Coordinator.
    #[error("cannot connect to the Coordinator: {0}")]
    Connect(#[from] quinn::ConnectError),
    /// A connection between the Coordinator and a Worker failed.
    #[error("the connection failed: {0}")]
    Connection(#[from] quinn::ConnectionError),
    /// A stream failed under a message being read.
    #[error("cannot read a message: {0}")]
    Read(#[from] quinn::ReadExactError),
    /// A stream failed under a Work Result being read.
    #[error("cannot read a Work Result: {0}")]
    ReadToEnd(#[from] quinn::ReadToEndError),
    /// A stream failed under a message being written.
    #[error("cannot send a message: {0}")]
    Write(#[from] quinn::WriteError),
    /// A stream was finished or reset already.
    #[error("cannot end a stream: {0}")]
    ClosedStream(#[from] quinn::ClosedStream),
    /// A message longer than either side ever sends.
    #[error("a message of {0} bytes, longer than any the other side sends")]
    Oversized(usize),
    /// A message that does not decode as the one expected.
    #[error("a malformed message: {0}")]
    Malformed(#[from] postcard::Error),
    /// A Work Result body that is not a whole number of pixel sums.
    #[error("a Work Result of {0} bytes, not a whole number of pixel sums")]
    Body(usize),
    /// A stream ended before what it carries did.
    #[error("{0} ended early")]
    StreamEnded(&'static str),
    /// A message the other side should not send at this point.
    #[error("`{0}` out of order")]
    OutOfOrder(&'static str),
    /// A Work Result that answers no Work Unit of the Render Job.
    #[error(
        "a Work Result for Tile {} of Pass {}, which the Render Job does not have",
        .0.tile.0,
        .0.pass.0
    )]
    UnknownWorkUnit(WorkUnit),
    /// A Work Result without one sum per pixel of its Tile.
    #[error(
        "a Work Result for Tile {} of Pass {} with {pixels} pixels, not one per pixel of the Tile",
        unit.tile.0,
        unit.pass.0
    )]
    WrongSize {
        /// The Work Unit it answers.
        unit: WorkUnit,
        /// The pixel sums it holds.
        pixels: usize,
    },
    /// The Scene the Coordinator sent cannot be built.
    #[error("cannot build the Scene: {0}")]
    Scene(#[from] umbra_scene::Error),
    /// The Worker cannot continue, and told the Coordinator so.
    #[error("the Worker cannot continue: {0}")]
    Fatal(String),
}
