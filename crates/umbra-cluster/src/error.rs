//! Why a Render Job did not finish, or a Worker could not serve one.

use std::io;

use rayon::ThreadPoolBuildError;
use thiserror::Error;

use umbra_render::WorkUnit;

use crate::wire::Refusal;

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
    /// A GPU was asked for, and none is usable.
    #[error("no usable GPU: {0}")]
    NoGpu(#[source] umbra_gpu::Error),
    /// A Render Device that is neither `cpu` nor `gpu:N`.
    #[error("unknown Render Device `{0}`: expected `cpu` or `gpu:N`")]
    UnknownRenderDevice(String),
    /// A Cluster Name that is not a DNS label.
    #[error(
        "invalid Cluster Name `{0}`: expected lowercase letters, digits and `-`, at most 63 characters, not starting or ending with `-`"
    )]
    ClusterName(String),
    /// Multicast DNS, which discovery needs, failed.
    #[error("multicast DNS failed: {0}")]
    Mdns(#[from] mdns_sd::Error),
    /// The network interfaces could not be listed.
    #[error("cannot list the network interfaces: {0}")]
    Interfaces(#[source] io::Error),
    /// `--interface` names no interface of this machine.
    #[error("no network interface `{name}`: the interfaces are {}", .known.join(", "))]
    UnknownInterface {
        /// The name given.
        name: String,
        /// The names of the interfaces that exist.
        known: Vec<String>,
    },
    /// `--interface` names an interface without an IPv4 address.
    #[error("the network interface `{0}` has no IPv4 address")]
    NoIpv4(String),
    /// The QUIC endpoint's UDP socket could not open.
    #[error("cannot open a UDP socket: {0}")]
    Listen(#[source] io::Error),
    /// The Coordinator's port could not be bound, such as one taken.
    #[error("cannot listen on {address}: {source}")]
    Bind {
        /// The address and port.
        address: std::net::SocketAddr,
        /// Why.
        source: io::Error,
    },
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
    /// A stream failed under the Scene being read.
    #[error("cannot read the Scene: {0}")]
    ReadChunk(#[from] quinn::ReadError),
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
    /// The Coordinator refused the Worker.
    #[error("refused by the Coordinator: {0}")]
    Rejected(Refusal),
    /// The connection closed, for this reason.
    #[error("the connection closed: {0}")]
    Closed(&'static str),
    /// The Worker cannot continue, and told the Coordinator so.
    #[error("the Worker cannot continue: {0}")]
    Fatal(String),
}

impl Error {
    /// Whether it is a message that one side should never send: always a
    /// bug, which closes the connection with `PROTOCOL_ERROR`.
    pub(crate) fn is_protocol_violation(&self) -> bool {
        match self {
            Self::Oversized(_)
            | Self::Malformed(_)
            | Self::Body(_)
            | Self::StreamEnded(_)
            | Self::OutOfOrder(_)
            | Self::UnknownWorkUnit(_)
            | Self::WrongSize { .. } => true,
            Self::Render(_)
            | Self::Runtime(_)
            | Self::ThreadPool(_)
            | Self::NoGpu(_)
            | Self::UnknownRenderDevice(_)
            | Self::ClusterName(_)
            | Self::Mdns(_)
            | Self::Interfaces(_)
            | Self::UnknownInterface { .. }
            | Self::NoIpv4(_)
            | Self::Listen(_)
            | Self::Bind { .. }
            | Self::Certificate(_)
            | Self::Tls(_)
            | Self::QuicTls(_)
            | Self::Connect(_)
            | Self::Connection(_)
            | Self::Read(_)
            | Self::ReadChunk(_)
            | Self::ReadToEnd(_)
            | Self::Write(_)
            | Self::ClosedStream(_)
            | Self::Scene(_)
            | Self::Rejected(_)
            | Self::Closed(_)
            | Self::Fatal(_) => false,
        }
    }
}
