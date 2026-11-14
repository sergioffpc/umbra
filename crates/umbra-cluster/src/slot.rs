//! The Worker Slot (ADR 0021, ADR 0026): a machine and one of its Render
//! Devices, as each Worker reports them in `Hello`. The Coordinator counts
//! the failures of the successive Workers of one slot.

use std::collections::BTreeMap;
use std::fmt::{self, Display};

use crate::device::RenderDevice;
use crate::host::NumaNode;

/// One machine, by its host name, and one of its Render Devices: the CPU of
/// one NUMA node, or a GPU.
#[derive(Debug, Clone, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub(crate) struct WorkerSlot {
    pub(crate) host: String,
    pub(crate) numa_node: NumaNode,
    pub(crate) render_device: RenderDevice,
}

impl Display for WorkerSlot {
    /// `p620 node 0 cpu`.
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "{} node {} {}",
            self.host, self.numa_node, self.render_device
        )
    }
}

/// How many failures in a row, without a Work Result in between, refuse a
/// Worker Slot for the rest of the Render Job (ADR 0021).
const FAILURES_TO_REFUSE: u32 = 3;

/// What happened to the Workers of each Worker Slot in one Render Job.
#[derive(Debug, Default)]
pub(crate) struct Slots(BTreeMap<WorkerSlot, SlotRecord>);

/// What happened to the Workers of one Worker Slot.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
struct SlotRecord {
    failures: u32,
    in_a_row: u32,
    refusals: u32,
}

impl Display for SlotRecord {
    /// `1 failures, 0 refusals`.
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{} failures, {} refusals", self.failures, self.refusals)
    }
}

impl Slots {
    /// A Worker of `slot` failed.
    pub(crate) fn failed(&mut self, slot: &WorkerSlot) {
        let record = self.0.entry(slot.clone()).or_default();
        record.failures += 1;
        record.in_a_row += 1;
    }

    /// A Worker of `slot` delivered a Work Result.
    pub(crate) fn delivered(&mut self, slot: &WorkerSlot) {
        if let Some(record) = self.0.get_mut(slot) {
            record.in_a_row = 0;
        }
    }

    /// A Worker of `slot` was refused.
    pub(crate) fn refused(&mut self, slot: &WorkerSlot) {
        self.0.entry(slot.clone()).or_default().refusals += 1;
    }

    /// Whether `slot` failed too often in a row to be admitted again.
    pub(crate) fn is_barred(&self, slot: &WorkerSlot) -> bool {
        self.0
            .get(slot)
            .is_some_and(|record| record.in_a_row >= FAILURES_TO_REFUSE)
    }

    /// The failures of the Workers of `slot`.
    pub(crate) fn failures(&self, slot: &WorkerSlot) -> u32 {
        self.0.get(slot).map_or(0, |record| record.failures)
    }

    /// Each Worker Slot that joined or was refused: `p620 node 0 cpu: 1
    /// failures, 0 refusals; r630 node 1 cpu: …`.
    pub(crate) fn summary(&self) -> String {
        let slots: Vec<String> = self
            .0
            .iter()
            .map(|(slot, record)| format!("{slot}: {record}"))
            .collect();
        slots.join("; ")
    }

    /// Adds `slot` with nothing happened yet, so that the summary lists it.
    pub(crate) fn joined(&mut self, slot: &WorkerSlot) {
        self.0.entry(slot.clone()).or_default();
    }

    /// The Workers refused, over every slot.
    pub(crate) fn refusals(&self) -> u32 {
        self.0.values().map(|record| record.refusals).sum()
    }
}
