//! `work`: a Worker in its own process (arc42 §8.13, ADR 0021). Given its
//! Coordinator's address, it serves that one Render Job, as each Local
//! Worker does; otherwise it runs until it is stopped, joining the Render
//! Jobs of its Cluster one after another as they are announced.

use std::collections::HashSet;
use std::net::SocketAddr;
use std::num::NonZeroUsize;
use std::sync::Arc;
use std::time::Duration;

use rayon::{ThreadPool, ThreadPoolBuilder};
use tokio::time;
use tracing::{info, warn};
use umbra_render::{BuildIdentity, Cancellation};

use crate::children::ProcessId;
use crate::cluster::ClusterName;
use crate::device::{GpuOrdinal, RenderDevice};
use crate::discovery::{Browser, Discovery, JobId};
use crate::error::Error;
use crate::host::{Host, NumaNode};
use crate::join::{self, Attempt, Ending};
use crate::runtime;
use crate::wire::Hello;

/// How long a Worker waits before each rejoin of a Render Job it failed
/// in: 2 s, then 10 s, then 30 s each time after (ADR 0021).
const BACKOFF: [Duration; 3] = [
    Duration::from_secs(2),
    Duration::from_secs(10),
    Duration::from_secs(30),
];

/// How many threads a CPU Worker renders with.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum Threads {
    /// One per logical CPU.
    #[default]
    All,
    /// Exactly this many.
    Count(NonZeroUsize),
}

/// Where a Worker finds its Coordinator.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum CoordinatorAddress {
    /// By DNS-SD: it joins each Render Job of its Cluster announced, one
    /// after another, until it is stopped.
    #[default]
    Discovered,
    /// At this address: it serves that one Render Job, then returns, as a
    /// Local Worker does.
    Given(SocketAddr),
}

/// How a Worker serves Render Jobs.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct WorkerOptions {
    /// The Render Device it renders on.
    pub render_device: RenderDevice,
    /// Its threads, on the CPU.
    pub threads: Threads,
    /// Where it finds its Coordinator.
    pub coordinator: CoordinatorAddress,
    /// The Cluster it belongs to.
    pub cluster: ClusterName,
    /// What it was built from, which its Coordinator must share.
    pub build_identity: BuildIdentity,
}

/// A Render Device, ready to render: the CPU's thread pool, or a GPU.
#[derive(Debug)]
pub(crate) enum Device {
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

    /// Its CPU threads: none on a GPU.
    fn threads(&self) -> u32 {
        match self {
            Self::Cpu(pool) => u32::try_from(pool.current_num_threads()).unwrap_or(u32::MAX),
            Self::Gpu(_) => 0,
        }
    }
}

/// Serves Render Jobs as `options` say: the one of the Coordinator at a
/// given address, or each of its Cluster as they are announced, until
/// `stop` is made. It blocks; the runtime it needs is its own. Once `stop`
/// is made, it closes its connection, if it has one, with
/// `WORKER_STOPPED`, and returns.
///
/// # Errors
///
/// [`Error`] if the runtime, the CPU's threads or discovery cannot start;
/// with a given address, also if the Worker is refused, fails, or the
/// Coordinator breaks the wire protocol.
pub fn work(options: &WorkerOptions, stop: &Cancellation) -> Result<(), Error> {
    let device = Arc::new(Device::new(options)?);
    let hello = Hello {
        cluster: options.cluster.clone(),
        build_identity: options.build_identity.clone(),
        host: Host::this(),
        render_device: options.render_device,
        numa_node: NumaNode::this(),
        threads: device.threads(),
        process_id: ProcessId::this(),
    };
    runtime::runtime()?.block_on(async {
        match options.coordinator {
            CoordinatorAddress::Given(address) => {
                let attempt = join::join(address, &device, &hello, stop).await;
                match attempt.ending {
                    Ending::Finished | Ending::Cancelled | Ending::Stopped => Ok(()),
                    Ending::Rejected(reason) => Err(Error::Rejected(reason)),
                    Ending::ProtocolError(error) | Ending::Failed(error) => Err(error),
                }
            }
            CoordinatorAddress::Discovered => {
                let browser = Browser::new(options.cluster.clone())?;
                Idle::new(browser, device, hello).serve(stop).await
            }
        }
    })
}

/// A Worker between Render Jobs: the Render Jobs of its Cluster announced,
/// in the order it discovered them, and those it is done with.
struct Idle {
    browser: Browser,
    device: Arc<Device>,
    hello: Hello,
    announced: Vec<(JobId, SocketAddr)>,
    /// The Render Jobs that ended for it: finished, cancelled, or given up
    /// on. It never joins them again.
    done: HashSet<JobId>,
}

/// What a Worker does after a failure in a Render Job.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Retry {
    /// Rejoin it.
    Rejoin,
    /// It was withdrawn meanwhile: be idle.
    Withdrawn,
    /// The Worker was stopped.
    Stopped,
}

impl Idle {
    fn new(browser: Browser, device: Arc<Device>, hello: Hello) -> Self {
        Self {
            browser,
            device,
            hello,
            announced: Vec::new(),
            done: HashSet::new(),
        }
    }

    /// Joins each Render Job of its Cluster as it is announced, until
    /// `stop` is made.
    async fn serve(mut self, stop: &Cancellation) -> Result<(), Error> {
        let mut retry = Retry::Withdrawn;
        while retry != Retry::Stopped
            && let Some((job, address)) = self.wait_for_job(stop).await?
        {
            retry = self.serve_job(&job, address, stop).await;
        }
        Ok(())
    }

    /// Waits for a Render Job of its Cluster that it is not done with;
    /// `None` once `stop` is made.
    async fn wait_for_job(
        &mut self,
        stop: &Cancellation,
    ) -> Result<Option<(JobId, SocketAddr)>, Error> {
        info!(
            "idle: waiting for a Render Job of Cluster {}",
            self.hello.cluster
        );
        let mut ticker = runtime::ticker();
        while self.next_job().is_none() && !stop.is_cancelled() {
            tokio::select! {
                discovery = self.browser.next() => self.discovered(discovery?),
                _ = ticker.tick() => (),
            }
        }
        Ok(self.next_job().filter(|_| !stop.is_cancelled()))
    }

    /// The first Render Job announced that it is not done with.
    fn next_job(&self) -> Option<(JobId, SocketAddr)> {
        self.announced
            .iter()
            .find(|(job, _)| !self.done.contains(job))
            .cloned()
    }

    /// Takes in `discovery`.
    fn discovered(&mut self, discovery: Discovery) {
        match discovery {
            Discovery::Announced { job, address } => self.announce(job, address),
            Discovery::Withdrawn { job } => self.announced.retain(|(known, _)| *known != job),
        }
    }

    /// `job` is announced at `address`: new, or at a new address.
    fn announce(&mut self, job: JobId, address: SocketAddr) {
        if let Some(known) = self.announced.iter_mut().find(|(known, _)| *known == job) {
            known.1 = address;
            return;
        }
        info!("Render Job {job} discovered at {address}");
        self.announced.push((job, address));
    }

    /// Serves `job`, at `address`, rejoining it after each failure for as
    /// long as it is announced, until it ends for this Worker.
    async fn serve_job(&mut self, job: &JobId, address: SocketAddr, stop: &Cancellation) -> Retry {
        let mut failures = 0;
        let mut retry = Retry::Rejoin;
        while retry == Retry::Rejoin {
            retry = self.attempt(job, address, stop, &mut failures).await;
        }
        retry
    }

    /// Serves `job` once, and waits before the next attempt if it failed,
    /// counting its `failures` in a row for the backoff.
    async fn attempt(
        &mut self,
        job: &JobId,
        address: SocketAddr,
        stop: &Cancellation,
        failures: &mut usize,
    ) -> Retry {
        let Attempt { ending, delivered } =
            join::join(address, &self.device, &self.hello, stop).await;
        info!("left Render Job {job}: {ending}");
        if let Some(retry) = self.after(job, &ending) {
            return retry;
        }
        if delivered > 0 {
            *failures = 0;
        }
        let delay = BACKOFF[(*failures).min(BACKOFF.len() - 1)];
        *failures += 1;
        self.wait(job, delay, stop).await
    }

    /// What the Worker does after `ending` in `job`: `None` to rejoin it.
    fn after(&mut self, job: &JobId, ending: &Ending) -> Option<Retry> {
        match ending {
            Ending::Stopped => Some(Retry::Stopped),
            Ending::Finished | Ending::Cancelled => {
                self.done.insert(job.clone());
                Some(Retry::Withdrawn)
            }
            Ending::Rejected(_) | Ending::ProtocolError(_) => {
                warn!("never rejoining Render Job {job}");
                self.done.insert(job.clone());
                Some(Retry::Withdrawn)
            }
            Ending::Failed(_) => None,
        }
    }

    /// Waits `delay` before rejoining `job`, taking in what it discovers
    /// meanwhile.
    async fn wait(&mut self, job: &JobId, delay: Duration, stop: &Cancellation) -> Retry {
        info!("rejoining Render Job {job} in {} s", delay.as_secs());
        let deadline = time::sleep(delay);
        tokio::pin!(deadline);
        let mut ticker = runtime::ticker();
        let mut retry = None;
        while retry.is_none() {
            retry = tokio::select! {
                () = &mut deadline => Some(Retry::Rejoin),
                discovery = self.browser.next() => self.withdrawn(job, discovery),
                _ = ticker.tick() => stop.is_cancelled().then_some(Retry::Stopped),
            };
        }
        retry.unwrap_or(Retry::Rejoin)
    }

    /// Takes in `discovery`, made while waiting to rejoin `job`: whether
    /// `job` was withdrawn meanwhile. If browsing stopped, it rejoins now.
    fn withdrawn(&mut self, job: &JobId, discovery: Result<Discovery, Error>) -> Option<Retry> {
        let Ok(discovery) = discovery else {
            return Some(Retry::Rejoin);
        };
        self.discovered(discovery);
        let announced = self.announced.iter().any(|(known, _)| known == job);
        (!announced).then(|| {
            info!("Render Job {job} withdrawn");
            Retry::Withdrawn
        })
    }
}
