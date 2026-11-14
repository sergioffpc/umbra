//! The QUIC endpoints of both sides (ADR 0010, ADR 0016): the Coordinator's
//! self-signed certificate, the Workers' acceptance of any certificate, and
//! the timeouts of a connection.

use std::net::{Ipv4Addr, SocketAddr};
use std::sync::Arc;
use std::time::Duration;

use quinn::crypto::rustls::QuicClientConfig;
use quinn::{ClientConfig, Endpoint, IdleTimeout, ServerConfig, TransportConfig};
use rustls::DigitallySignedStruct;
use rustls::client::danger::{HandshakeSignatureValid, ServerCertVerified, ServerCertVerifier};
use rustls::crypto::{CryptoProvider, verify_tls12_signature, verify_tls13_signature};
use rustls::pki_types::{CertificateDer, PrivateKeyDer, PrivatePkcs8KeyDer, ServerName, UnixTime};

use crate::error::Error;

/// The name in the Coordinator's certificate. Workers accept any
/// certificate, so it is checked by nothing.
pub(crate) const SERVER_NAME: &str = "umbra";

/// How long a connection lives without a packet: the time after which
/// either side takes the other for lost (arc42 §8.13).
const IDLE_TIMEOUT: Duration = Duration::from_secs(5);

/// How often an idle connection sends a packet to keep itself open, well
/// within [`IDLE_TIMEOUT`].
const KEEP_ALIVE: Duration = Duration::from_secs(1);

/// The Coordinator's endpoint, on an ephemeral UDP port of the loopback
/// interface, which its Local Workers connect to; its certificate is
/// self-signed at start-up (ADR 0016).
pub(crate) fn coordinator_endpoint() -> Result<Endpoint, Error> {
    let certified = rcgen::generate_simple_self_signed([SERVER_NAME.to_owned()])?;
    let key = PrivatePkcs8KeyDer::from(certified.signing_key.serialize_der());
    let mut config = ServerConfig::with_single_cert(
        vec![certified.cert.der().clone()],
        PrivateKeyDer::Pkcs8(key),
    )?;
    config.transport_config(transport());
    let address = SocketAddr::from((Ipv4Addr::LOCALHOST, 0));
    Endpoint::server(config, address).map_err(Error::Listen)
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
    config.transport_config(transport());
    let local = match coordinator {
        SocketAddr::V4(_) => SocketAddr::from((Ipv4Addr::UNSPECIFIED, 0)),
        SocketAddr::V6(_) => SocketAddr::from((std::net::Ipv6Addr::UNSPECIFIED, 0)),
    };
    let mut endpoint = Endpoint::client(local).map_err(Error::Listen)?;
    endpoint.set_default_client_config(config);
    Ok(endpoint)
}

/// Both sides' timeouts: lost after [`IDLE_TIMEOUT`] of silence, kept alive
/// every [`KEEP_ALIVE`].
fn transport() -> Arc<TransportConfig> {
    let mut transport = TransportConfig::default();
    transport.max_idle_timeout(Some(
        IdleTimeout::try_from(IDLE_TIMEOUT).expect("5 s is a valid idle timeout"),
    ));
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
