//! The machine a Worker runs on, as its `Hello` and the report name it
//! (arc42 §8.10, §8.13): its host name and its CPU, and the NUMA node its
//! threads are bound to, read from `/proc` and `/sys` (ADR 0034).

use std::collections::BTreeSet;
use std::fmt::{self, Display};
use std::fs;

use serde::{Deserialize, Serialize};

/// A machine's host name and CPU model, each `None` where `/proc` does not
/// say.
#[derive(Debug, Clone, PartialEq, Eq, PartialOrd, Ord, Hash, Serialize, Deserialize)]
pub(crate) struct Host {
    name: Option<String>,
    cpu: Option<String>,
}

impl Host {
    /// The machine this process runs on.
    pub(crate) fn this() -> Self {
        let read = |path| fs::read_to_string(path).ok();
        Self {
            name: read("/proc/sys/kernel/hostname").map(|name| name.trim().to_owned()),
            cpu: read("/proc/cpuinfo").as_deref().and_then(cpu_model),
        }
    }

    /// Its host name, `unknown` where `/proc` did not say.
    pub(crate) fn name(&self) -> &str {
        self.name.as_deref().unwrap_or("unknown")
    }
}

impl Display for Host {
    /// `host p620, CPU AMD Ryzen Threadripper PRO 3975WX 32-Cores`, with
    /// `unknown` for what `/proc` did not say.
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let cpu = self.cpu.as_deref().unwrap_or("unknown");
        write!(f, "host {}, CPU {cpu}", self.name())
    }
}

/// The CPU's model in `cpuinfo`, the text of `/proc/cpuinfo`: its model
/// name on x86-64, its implementer and part on arm64, which has none.
fn cpu_model(cpuinfo: &str) -> Option<String> {
    let field = |key: &str| {
        cpuinfo
            .lines()
            .filter_map(|line| line.split_once(':'))
            .find(|(name, _)| name.trim() == key)
            .map(|(_, value)| value.trim())
    };
    field("model name").map(str::to_owned).or_else(|| {
        let part = field("CPU part")?;
        let implementer = field("CPU implementer")?;
        Some(format!("arm64 implementer {implementer} part {part}"))
    })
}

/// A NUMA node of a machine, by the kernel's number.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Serialize, Deserialize)]
pub(crate) struct NumaNode(pub(crate) u32);

impl NumaNode {
    /// The node this process's CPU affinity is bound to: the one whose CPUs
    /// hold every CPU it may run on, or node 0 when it is unbound, spans
    /// several nodes, or the kernel does not say (arc42 §8.13).
    pub(crate) fn this() -> Self {
        allowed_cpus()
            .and_then(|allowed| node_holding(&allowed))
            .unwrap_or(Self(0))
    }
}

impl Display for NumaNode {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}", self.0)
    }
}

/// The CPUs this process may run on, from `/proc/self/status`.
fn allowed_cpus() -> Option<BTreeSet<u32>> {
    let status = fs::read_to_string("/proc/self/status").ok()?;
    let list = status
        .lines()
        .find_map(|line| line.strip_prefix("Cpus_allowed_list:"))?;
    cpu_list(list)
}

/// The node whose CPUs hold every one of `cpus`, if one does.
fn node_holding(cpus: &BTreeSet<u32>) -> Option<NumaNode> {
    fs::read_dir("/sys/devices/system/node")
        .ok()?
        .filter_map(Result::ok)
        .filter_map(|entry| {
            let name = entry.file_name();
            let node = name.to_str()?.strip_prefix("node")?.parse().ok()?;
            let list = fs::read_to_string(entry.path().join("cpulist")).ok()?;
            Some((NumaNode(node), cpu_list(&list)?))
        })
        .find_map(|(node, node_cpus)| cpus.is_subset(&node_cpus).then_some(node))
}

/// The CPUs of a kernel CPU list such as `0-3,8,10-11`.
fn cpu_list(list: &str) -> Option<BTreeSet<u32>> {
    let mut cpus = BTreeSet::new();
    for range in list.trim().split(',').filter(|range| !range.is_empty()) {
        let (low, high) = range.split_once('-').unwrap_or((range, range));
        cpus.extend(low.trim().parse::<u32>().ok()?..=high.trim().parse().ok()?);
    }
    Some(cpus)
}
