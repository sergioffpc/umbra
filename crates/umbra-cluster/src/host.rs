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

#[cfg(test)]
mod tests {
    use super::cpu_model;

    #[test]
    fn an_x86_64_cpu_is_named_by_its_model_name() {
        let cpuinfo = concat!(
            "processor\t: 0\n",
            "vendor_id\t: GenuineIntel\n",
            "model name\t: Intel(R) Xeon(R) CPU E5-2680 v4 @ 2.40GHz\n",
            "processor\t: 1\n",
            "model name\t: Intel(R) Xeon(R) CPU E5-2680 v4 @ 2.40GHz\n",
        );
        assert_eq!(
            cpu_model(cpuinfo).as_deref(),
            Some("Intel(R) Xeon(R) CPU E5-2680 v4 @ 2.40GHz")
        );
    }

    #[test]
    fn an_arm64_cpu_is_named_by_its_implementer_and_part() {
        let cpuinfo = concat!(
            "processor\t: 0\n",
            "BogoMIPS\t: 48.00\n",
            "CPU implementer\t: 0x61\n",
            "CPU architecture: 8\n",
            "CPU variant\t: 0x0\n",
            "CPU part\t: 0x000\n",
        );
        assert_eq!(
            cpu_model(cpuinfo).as_deref(),
            Some("arm64 implementer 0x61 part 0x000")
        );
    }

    #[test]
    fn a_cpuinfo_without_either_names_no_cpu() {
        assert_eq!(cpu_model("processor\t: 0\n"), None);
    }
}
