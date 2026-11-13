//! What `libm` costs on this CPU: umbra-math's 19 `f32` functions of the C
//! library, each three ways (glibc's scalar function, umbra-math as it is,
//! and the best SIMD form there is), with the time per element and the
//! largest error in ULPs. The GPU's counterpart is `umbra-gpu`'s
//! `benches/libm.rs`.
//!
//! ```sh
//! cargo bench -p umbra-math --bench libm                            # print the table
//! UMBRA_BENCH_MACHINE=p620 cargo bench -p umbra-math --bench libm   # and write the guide's report
//! ```
//!
//! The bench pins itself to CPU 2, so the scheduler does not move it. Each
//! function maps 1024 inputs one call at a time (one element, one `Vec4` or
//! one vector), each result through `black_box`, so LLVM cannot vectorise a
//! scalar implementation across calls. The time is the median of 201 runs of
//! at least 200 µs, per element. The error is the largest over the inputs,
//! in ULPs from the correctly rounded result, which is glibc's `f64` routine
//! (or the exact `f32` operation) rounded to `f32`. `report.rs` says where the
//! results go; `crates/umbra-math/README.md` how the guide uses them.

#[path = "libm/cpu.rs"]
mod cpu;
#[path = "libm/funcs.rs"]
mod funcs;
#[path = "libm/report.rs"]
mod report;

use std::ffi::c_int;
use std::fs;
use std::process::ExitCode;
use std::time::{Duration, Instant};

use crate::funcs::{BATCH, FUNCS, Func, UlpError};
use crate::report::{Cell, Device, Report, Row};

/// The core the bench runs on: not 0, which takes most interrupts.
const CORE: usize = 2;

/// The median of 201 timings of `f`, each repeated to at least 200 µs, in ns.
fn median_ns(mut f: impl FnMut()) -> f64 {
    const SAMPLES: usize = 201;
    const MIN_SAMPLE: Duration = Duration::from_micros(200);
    let mut reps = 1_u32;
    loop {
        let t = Instant::now();
        (0..reps).for_each(|_| f());
        if t.elapsed() >= MIN_SAMPLE {
            break;
        }
        reps *= 2;
    }
    let mut samples: Vec<f64> = (0..SAMPLES)
        .map(|_| {
            let t = Instant::now();
            (0..reps).for_each(|_| f());
            t.elapsed().as_secs_f64() * 1e9 / f64::from(reps)
        })
        .collect();
    samples.sort_by(f64::total_cmp);
    samples[SAMPLES / 2]
}

/// Every implementation of `func` on this CPU, measured.
fn measure(func: &Func) -> Row {
    let inputs = (func.inputs)();
    let cells = cpu::implementations(func.name)
        .into_iter()
        .map(|imp| {
            let (mut o1, mut o2) = (vec![0.0; BATCH], vec![0.0; BATCH]);
            (imp.run)(&inputs, &mut o1, &mut o2);
            let ulps = UlpError::max(func, &inputs, &o1, &o2);
            #[expect(clippy::cast_precision_loss, reason = "BATCH is small")]
            let ns = median_ns(|| (imp.run)(&inputs, &mut o1, &mut o2)) / BATCH as f64;
            let cell = Cell {
                column: imp.column,
                ns,
                ulps,
                note: imp.note,
            };
            cell.print(func.name, Device::Cpu);
            cell
        })
        .collect();
    Row {
        func: func.name,
        cells,
    }
}

/// Whether the bench is held to one core.
#[derive(Debug)]
enum Pinning {
    Pinned,
    /// The kernel refused, or the machine has no such core.
    Free,
}

/// Holds this thread to CPU `core`.
#[expect(unsafe_code, reason = "sched_setaffinity, through FFI")]
fn pin_to(core: usize) -> Pinning {
    unsafe extern "C" {
        fn sched_setaffinity(pid: c_int, size: usize, mask: *const u64) -> c_int;
    }
    // glibc's `cpu_set_t`: 1024 bits.
    let mut mask = [0_u64; 16];
    mask[core / 64] |= 1 << (core % 64);
    // SAFETY: `mask` is a live `cpu_set_t` of the size passed; pid 0 is this
    // thread.
    match unsafe { sched_setaffinity(0, size_of_val(&mask), mask.as_ptr()) } {
        0 => Pinning::Pinned,
        _ => Pinning::Free,
    }
}

/// The CPU's model name, or its implementer and part on arm64.
fn cpu_model() -> String {
    let info = fs::read_to_string("/proc/cpuinfo").unwrap_or_default();
    let field = |key: &str| {
        info.lines()
            .find(|l| l.starts_with(key))
            .and_then(|l| l.split_once(':'))
            .map(|(_, v)| v.trim().to_owned())
    };
    field("model name")
        .or_else(|| {
            let part = field("CPU part")?;
            let implementer = field("CPU implementer").unwrap_or_default();
            Some(format!("arm64 implementer {implementer} part {part}"))
        })
        .unwrap_or_else(|| "unknown CPU".to_owned())
}

fn main() -> ExitCode {
    if let Pinning::Free = pin_to(CORE) {
        print_unpinned();
    }
    Report {
        device: Device::of_this_bench(),
        model: cpu_model(),
        rows: FUNCS.iter().map(measure).collect(),
    }
    .finish()
}

#[expect(clippy::print_stderr, reason = "a warning about the measurement")]
fn print_unpinned() {
    eprintln!("libm: cannot pin the bench to CPU {CORE}, so the scheduler may move it");
}
