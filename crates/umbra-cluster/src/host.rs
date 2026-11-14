//! The machine a Worker runs on, as its report names it (arc42 §8.10):
//! its host name and its CPU, read from `/proc` (ADR 0034).

use std::fmt::{self, Display};
use std::fs;

/// A machine's host name and CPU model, each `None` where `/proc` does not
/// say.
#[derive(Debug, Clone, PartialEq, Eq)]
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
}

impl Display for Host {
    /// `host p620, CPU AMD Ryzen Threadripper PRO 3975WX 32-Cores`, with
    /// `unknown` for what `/proc` did not say.
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let name = self.name.as_deref().unwrap_or("unknown");
        let cpu = self.cpu.as_deref().unwrap_or("unknown");
        write!(f, "host {name}, CPU {cpu}")
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
