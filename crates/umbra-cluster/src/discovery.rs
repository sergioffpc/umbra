//! Discovery by DNS-SD over multicast DNS (ADR 0021, ADR 0029): a
//! Coordinator announces its Render Job as an instance of `_umbra._udp`,
//! with its QUIC port and its Cluster Name, and an idle Worker browses for
//! the Render Jobs of its Cluster.

use std::collections::hash_map::RandomState;
use std::fmt::{self, Display};
use std::hash::{BuildHasher, Hasher};
use std::net::{IpAddr, Ipv4Addr, SocketAddr};
use std::time::Duration;

use if_addrs::IfAddr;
use mdns_sd::{IfKind, ResolvedService, ServiceDaemon, ServiceEvent, ServiceInfo};

use crate::cluster::ClusterName;
use crate::error::Error;
use crate::host::Host;

/// The DNS-SD service of a Render Job.
const SERVICE: &str = "_umbra._udp.local.";

/// The TXT key of the Cluster Name.
const CLUSTER_KEY: &str = "cluster";

/// How long withdrawing an announcement waits for its goodbye to go out.
const GOODBYE_WAIT: Duration = Duration::from_secs(1);

/// The network interfaces a Coordinator listens and announces on
/// (`--interface`, ADR 0029).
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub enum Interface {
    /// Every interface, on the unspecified address.
    #[default]
    All,
    /// The one interface of this OS name, such as `enp5s0`, on its IPv4
    /// address alone.
    Named(String),
}

impl Interface {
    /// The address the Coordinator binds: the unspecified one, or the
    /// named interface's IPv4 address.
    ///
    /// # Errors
    ///
    /// [`Error::UnknownInterface`] for a name no interface has, listing
    /// those that exist; [`Error::NoIpv4`] for one without an IPv4
    /// address.
    pub(crate) fn address(&self) -> Result<IpAddr, Error> {
        let Self::Named(name) = self else {
            return Ok(Ipv4Addr::UNSPECIFIED.into());
        };
        let interfaces = if_addrs::get_if_addrs().map_err(Error::Interfaces)?;
        if !interfaces.iter().any(|interface| &interface.name == name) {
            let mut known: Vec<String> = interfaces.into_iter().map(|i| i.name).collect();
            known.sort();
            known.dedup();
            return Err(Error::UnknownInterface {
                name: name.clone(),
                known,
            });
        }
        interfaces
            .into_iter()
            .filter(|interface| &interface.name == name)
            .find_map(|interface| match interface.addr {
                IfAddr::V4(v4) => Some(IpAddr::V4(v4.ip)),
                IfAddr::V6(_) => None,
            })
            .ok_or_else(|| Error::NoIpv4(name.clone()))
    }
}

/// A Render Job, as its announcement names it: a random identifier.
#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub(crate) struct JobId(String);

impl Display for JobId {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let instance = self.0.strip_suffix(&format!(".{SERVICE}"));
        f.write_str(instance.unwrap_or(&self.0))
    }
}

/// A Render Job announced on the LAN, while its Coordinator announces it.
pub(crate) struct Announcement {
    daemon: ServiceDaemon,
    id: JobId,
}

impl fmt::Debug for Announcement {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Announcement")
            .field("id", &self.id)
            .finish_non_exhaustive()
    }
}

impl Announcement {
    /// Announces a Render Job of `cluster`, listening on `port` of
    /// `address`, on `interface`, under a random identifier.
    ///
    /// # Errors
    ///
    /// [`Error::Mdns`] if the multicast DNS socket cannot open.
    pub(crate) fn new(
        cluster: &ClusterName,
        interface: &Interface,
        address: IpAddr,
        port: u16,
    ) -> Result<Self, Error> {
        let daemon = ServiceDaemon::new()?;
        let host = format!("{}.local.", Host::this().name());
        let properties = [(CLUSTER_KEY, cluster.to_string())];
        let instance = random_id();
        let info = match interface {
            Interface::All => {
                ServiceInfo::new(SERVICE, &instance, &host, (), port, &properties[..])?
                    .enable_addr_auto()
            }
            Interface::Named(name) => {
                daemon.disable_interface(IfKind::All)?;
                daemon.enable_interface(IfKind::Name(name.clone()))?;
                ServiceInfo::new(SERVICE, &instance, &host, address, port, &properties[..])?
            }
        };
        let id = JobId(info.get_fullname().to_owned());
        daemon.register(info)?;
        Ok(Self { daemon, id })
    }

    /// Its Render Job's identifier.
    pub(crate) fn id(&self) -> &JobId {
        &self.id
    }

    /// Withdraws it, waiting a moment for the goodbye to go out, so that
    /// no Worker joins a Render Job that is ending.
    pub(crate) fn withdraw(self) {
        if let Ok(goodbye) = self.daemon.unregister(&self.id.0) {
            let _ = goodbye.recv_timeout(GOODBYE_WAIT);
        }
        let _ = self.daemon.shutdown();
    }
}

/// A random Render Job identifier: 16 hexadecimal digits.
fn random_id() -> String {
    let mut hasher = RandomState::new().build_hasher();
    hasher.write_u32(std::process::id());
    format!("{:016x}", hasher.finish())
}

/// What an idle Worker learns as it browses.
#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) enum Discovery {
    /// A Render Job of its Cluster is announced, its Coordinator at this
    /// address.
    Announced { job: JobId, address: SocketAddr },
    /// A Render Job is no longer announced.
    Withdrawn { job: JobId },
}

/// A Worker's browsing for the Render Jobs of its Cluster.
pub(crate) struct Browser {
    daemon: ServiceDaemon,
    events: mdns_sd::Receiver<ServiceEvent>,
    cluster: ClusterName,
}

impl fmt::Debug for Browser {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Browser")
            .field("cluster", &self.cluster)
            .finish_non_exhaustive()
    }
}

impl Browser {
    /// Starts browsing for the Render Jobs of `cluster`, on every
    /// interface.
    ///
    /// # Errors
    ///
    /// [`Error::Mdns`] if the multicast DNS socket cannot open.
    pub(crate) fn new(cluster: ClusterName) -> Result<Self, Error> {
        let daemon = ServiceDaemon::new()?;
        let events = daemon.browse(SERVICE)?;
        Ok(Self {
            daemon,
            events,
            cluster,
        })
    }

    /// The next Render Job of its Cluster announced, or withdrawn; those of
    /// other Clusters are skipped.
    ///
    /// # Errors
    ///
    /// [`Error::Mdns`] if the browsing stopped.
    pub(crate) async fn next(&mut self) -> Result<Discovery, Error> {
        let mut discovery = None;
        while discovery.is_none() {
            let event = self
                .events
                .recv_async()
                .await
                .map_err(|_| Error::Mdns(mdns_sd::Error::DaemonShutdown))?;
            discovery = self.discovery(event);
        }
        discovery.ok_or(Error::Mdns(mdns_sd::Error::DaemonShutdown))
    }

    /// What `event` says of a Render Job of its Cluster, if anything: the
    /// other events, those `mdns-sd` may add among them, say nothing of
    /// one.
    #[expect(
        clippy::wildcard_enum_match_arm,
        reason = "`ServiceEvent` is `#[non_exhaustive]`"
    )]
    fn discovery(&self, event: ServiceEvent) -> Option<Discovery> {
        match event {
            ServiceEvent::ServiceResolved(service) => self.announced(&service),
            ServiceEvent::ServiceRemoved(_, fullname) => Some(Discovery::Withdrawn {
                job: JobId(fullname),
            }),
            _ => None,
        }
    }

    /// The Render Job `service` announces, if it is of this Cluster and
    /// has an IPv4 address.
    fn announced(&self, service: &ResolvedService) -> Option<Discovery> {
        let cluster = service.txt_properties.get_property_val_str(CLUSTER_KEY)?;
        if cluster != self.cluster.to_string() {
            return None;
        }
        let ip = service
            .addresses
            .iter()
            .filter_map(|address| match address.to_ip_addr() {
                IpAddr::V4(v4) => Some(v4),
                IpAddr::V6(_) => None,
            })
            .min_by_key(|v4| (v4.is_loopback(), *v4))?;
        Some(Discovery::Announced {
            job: JobId(service.fullname.clone()),
            address: SocketAddr::from((ip, service.port)),
        })
    }
}

impl Drop for Browser {
    fn drop(&mut self) {
        let _ = self.daemon.shutdown();
    }
}
