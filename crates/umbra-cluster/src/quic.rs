//! The QUIC endpoints of both sides (ADR 0010, ADR 0016): the Coordinator's
//! self-signed certificate, the Workers' acceptance of any certificate, and
//! the timeouts of a connection.

use std::net::{Ipv4Addr, SocketAddr};
use std::sync::{Arc, OnceLock};
use std::time::Duration;

use quinn::crypto::rustls::QuicClientConfig;
use quinn::{
    ClientConfig, Connection, ConnectionError, Endpoint, IdleTimeout, ServerConfig, TransportConfig,
};
use rustls::DigitallySignedStruct;
use rustls::client::danger::{HandshakeSignatureValid, ServerCertVerified, ServerCertVerifier};
use rustls::crypto::{CryptoProvider, verify_tls12_signature, verify_tls13_signature};
use rustls::pki_types::{CertificateDer, PrivateKeyDer, PrivatePkcs8KeyDer, ServerName, UnixTime};

use crate::error::Error;
use crate::wire::CloseCode;

/// The name in the Coordinator's certificate. Workers accept any
/// certificate, so it is checked by nothing.
pub(crate) const SERVER_NAME: &str = "umbra";

/// How often an idle connection sends a packet to keep itself open, well
/// within any idle timeout.
const KEEP_ALIVE: Duration = Duration::from_secs(1);

/// The Coordinator's endpoint, on `address`, whose port is ephemeral when
/// it is 0 (ADR 0029); its certificate is self-signed at start-up (ADR
/// 0016). A connection without a packet for `death_time` is lost, on both
/// sides: the Workers set no idle timeout of their own, so QUIC takes the
/// Coordinator's (arc42 §8.13).
pub(crate) fn coordinator_endpoint(
    address: SocketAddr,
    death_time: Duration,
) -> Result<Endpoint, Error> {
    let certified = rcgen::generate_simple_self_signed([SERVER_NAME.to_owned()])?;
    let key = PrivatePkcs8KeyDer::from(certified.signing_key.serialize_der());
    let mut config = ServerConfig::with_single_cert(
        vec![certified.cert.der().clone()],
        PrivateKeyDer::Pkcs8(key),
    )?;
    config.transport_config(transport(Some(death_time)));
    Endpoint::server(config, address).map_err(|source| Error::Bind { address, source })
}

/// A Worker's endpoint, for one connection to the Coordinator at
/// `coordinator`, whose certificate it accepts whatever it is.
pub(crate) fn worker_endpoint(coordinator: SocketAddr) -> Result<Endpoint, Error> {
    let provider = Arc::new(rustls::crypto::ring::default_provider());
    let tls = rustls::ClientConfig::builder_with_provider(Arc::clone(&provider))
        .with_protocol_versions(&[&rustls::version::TLS13])?
        .dangerous()
        .with_custom_certificate_verifier(Arc::new(AnyCertificate(provider)))
        .with_no_client_auth();
    let crypto = QuicClientConfig::try_from(tls)?;
    let mut config = ClientConfig::new(Arc::new(crypto));
    config.transport_config(transport(None));
    let local = match coordinator {
        SocketAddr::V4(_) => SocketAddr::from((Ipv4Addr::UNSPECIFIED, 0)),
        SocketAddr::V6(_) => SocketAddr::from((std::net::Ipv6Addr::UNSPECIFIED, 0)),
    };
    let mut endpoint = Endpoint::client(local).map_err(Error::Listen)?;
    endpoint.set_default_client_config(config);
    Ok(endpoint)
}

/// A side's timeouts: lost after `idle` of silence, or after the other
/// side's when `None`; kept alive every [`KEEP_ALIVE`].
fn transport(idle: Option<Duration>) -> Arc<TransportConfig> {
    let mut transport = TransportConfig::default();
    transport.max_idle_timeout(idle.and_then(|idle| IdleTimeout::try_from(idle).ok()));
    transport.keep_alive_interval(Some(KEEP_ALIVE));
    Arc::new(transport)
}

/// The Workers' certificate verifier: it accepts any certificate, because
/// TLS is there only because QUIC requires it (ADR 0016). It still checks
/// the handshake's signatures against the certificate presented.
#[derive(Debug)]
struct AnyCertificate(Arc<CryptoProvider>);

impl ServerCertVerifier for AnyCertificate {
    fn verify_server_cert(
        &self,
        _end_entity: &CertificateDer<'_>,
        _intermediates: &[CertificateDer<'_>],
        _server_name: &ServerName<'_>,
        _ocsp_response: &[u8],
        _now: UnixTime,
    ) -> Result<ServerCertVerified, rustls::Error> {
        Ok(ServerCertVerified::assertion())
    }

    fn verify_tls12_signature(
        &self,
        message: &[u8],
        cert: &CertificateDer<'_>,
        dss: &DigitallySignedStruct,
    ) -> Result<HandshakeSignatureValid, rustls::Error> {
        verify_tls12_signature(
            message,
            cert,
            dss,
            &self.0.signature_verification_algorithms,
        )
    }

    fn verify_tls13_signature(
        &self,
        message: &[u8],
        cert: &CertificateDer<'_>,
        dss: &DigitallySignedStruct,
    ) -> Result<HandshakeSignatureValid, rustls::Error> {
        verify_tls13_signature(
            message,
            cert,
            dss,
            &self.0.signature_verification_algorithms,
        )
    }

    fn supported_verify_schemes(&self) -> Vec<rustls::SignatureScheme> {
        self.0.signature_verification_algorithms.supported_schemes()
    }
}

/// One side's hold on a connection: it closes it at most once, and
/// remembers the code it closed it with, which the connection's own close
/// reason does not keep.
#[derive(Debug)]
pub(crate) struct Closer {
    connection: Connection,
    code: OnceLock<CloseCode>,
}

/// How a connection closed, seen from one side.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum Closed {
    /// This side closed it with this code.
    Here(CloseCode),
    /// The other side closed it with this code, or one that is not
    /// Umbra's.
    There(Result<CloseCode, quinn::VarInt>),
    /// It was lost: no packet for the idle timeout, a reset, a transport
    /// error.
    Lost,
}

impl Closer {
    /// A hold on `connection`, open.
    pub(crate) fn new(connection: Connection) -> Self {
        Self {
            connection,
            code: OnceLock::new(),
        }
    }

    /// The connection.
    pub(crate) fn connection(&self) -> &Connection {
        &self.connection
    }

    /// Closes the connection with `code` and `reason`, unless it is closed
    /// already.
    pub(crate) fn close(&self, code: CloseCode, reason: &[u8]) {
        if self.connection.close_reason().is_none() && self.code.set(code).is_ok() {
            self.connection.close(code.into(), reason);
        }
    }

    /// Waits for the connection to close, and says how it did.
    pub(crate) async fn closed(&self) -> Closed {
        let reason = self.connection.closed().await;
        self.how(&reason)
    }

    /// How a connection that closed for `reason` closed.
    fn how(&self, reason: &ConnectionError) -> Closed {
        match reason {
            ConnectionError::LocallyClosed => {
                Closed::Here(self.code.get().copied().unwrap_or(CloseCode::Finished))
            }
            ConnectionError::ApplicationClosed(close) => {
                Closed::There(CloseCode::try_from(close.error_code))
            }
            ConnectionError::VersionMismatch
            | ConnectionError::TransportError(_)
            | ConnectionError::ConnectionClosed(_)
            | ConnectionError::Reset
            | ConnectionError::TimedOut
            | ConnectionError::CidsExhausted => Closed::Lost,
        }
    }
}
