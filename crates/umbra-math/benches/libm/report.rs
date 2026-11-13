//! The libm benchmarks' report: one record per machine and device, which the
//! math guide's "What libm costs" plots. Shared by the CPU and GPU benches,
//! which include it by `#[path]`.
//!
//! With `UMBRA_BENCH_MACHINE` set to the machine's name (`p620`, `r630`,
//! `m2`), a bench writes its record to `docs/libm-results/<machine>-<device>.js`
//! of `umbra-math`, a script the guide loads; without it, the bench only
//! prints its table. Each machine and device has a file of its own, so runs
//! on different machines never touch the same file.

use std::ffi::{CStr, c_char};
use std::fmt::Write as _;
use std::path::{Path, PathBuf};
use std::process::{Command, ExitCode};
use std::time::{SystemTime, UNIX_EPOCH};
use std::{env, fs, io};

use crate::funcs::UlpError;

/// The variable that names the machine, and asks for the report.
const MACHINE: &str = "UMBRA_BENCH_MACHINE";

/// The three columns of the table.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Column {
    /// glibc's scalar `libm` on the CPU, `libdevice` on the GPU.
    Baseline,
    /// umbra-math as it is.
    Umbra,
    /// The fastest form: `libmvec` or the vector instruction on the CPU, the
    /// approximate intrinsics on the GPU.
    Best,
}

impl Column {
    pub(crate) const ALL: [Self; 3] = [Self::Baseline, Self::Umbra, Self::Best];

    /// The column's key in the guide's records.
    fn key(self) -> &'static str {
        match self {
            Self::Baseline => "base",
            Self::Umbra => "umbra",
            Self::Best => "best",
        }
    }
}

/// What a record measures.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Device {
    /// One core: the time is a call's latency.
    Cpu,
    /// The whole GPU: the time is its throughput.
    Gpu,
}

impl Device {
    /// The device of the bench being built: the GPU in `umbra-gpu`, the CPU
    /// in `umbra-math`.
    pub(crate) fn of_this_bench() -> Self {
        match env!("CARGO_PKG_NAME") {
            "umbra-gpu" => Self::Gpu,
            _ => Self::Cpu,
        }
    }

    fn key(self) -> &'static str {
        match self {
            Self::Cpu => "cpu",
            Self::Gpu => "gpu",
        }
    }
}

/// One measured cell: median time per element, largest error, and what ran.
pub(crate) struct Cell {
    pub(crate) column: Column,
    pub(crate) ns: f64,
    pub(crate) ulps: UlpError,
    pub(crate) note: String,
}

impl Cell {
    /// Prints the cell as one line of the table, as soon as it is measured.
    #[expect(clippy::print_stdout, reason = "the table is the bench's output")]
    pub(crate) fn print(&self, func: &str, device: Device) {
        let time = match device {
            Device::Cpu => format!("{:9.3} ns", self.ns),
            Device::Gpu => format!("{:9.3} ps", self.ns * 1e3),
        };
        println!(
            "{func:8} {:5} {time}  {:>5} ULP  {}",
            self.column.key(),
            self.ulps.to_string(),
            self.note
        );
    }

    /// The cell as `[ns, ulps, note]`, `ulps` null for a NaN mismatch.
    fn js(&self) -> String {
        let ulps = match self.ulps {
            UlpError::Ulps(n) => n.to_string(),
            UlpError::Nan => "null".to_owned(),
        };
        format!("[{:.6}, {ulps}, {}]", self.ns, js(&self.note))
    }
}

/// One function's cells, those of the columns it has on the device.
pub(crate) struct Row {
    pub(crate) func: &'static str,
    pub(crate) cells: Vec<Cell>,
}

impl Row {
    /// The row as `{ f, base, umbra, best }`, a column it lacks as `null`.
    fn js(&self) -> String {
        let cells = Column::ALL.map(|column| {
            let cell = self.cells.iter().find(|c| c.column == column);
            format!(
                "{}: {}",
                column.key(),
                cell.map_or_else(|| "null".to_owned(), Cell::js)
            )
        });
        format!("{{ f: {}, {} }}", js(self.func), cells.join(", "))
    }
}

/// Where the report went.
#[derive(Debug)]
enum Published {
    /// Written to this file.
    Written(PathBuf),
    /// Not asked for: `UMBRA_BENCH_MACHINE` is not set.
    NotAsked,
}

/// A device's results, and what they were measured on.
pub(crate) struct Report {
    /// [`Device::of_this_bench`].
    pub(crate) device: Device,
    /// The CPU's model or the GPU's name.
    pub(crate) model: String,
    pub(crate) rows: Vec<Row>,
}

impl Report {
    /// Writes the report for the guide if `UMBRA_BENCH_MACHINE` names the
    /// machine, and says where it went: the bench's exit code.
    #[expect(
        clippy::print_stdout,
        clippy::print_stderr,
        reason = "where the report went is the bench's output"
    )]
    pub(crate) fn finish(&self) -> ExitCode {
        match self.publish() {
            Ok(Published::Written(path)) => println!("libm: wrote {}", path.display()),
            Ok(Published::NotAsked) => {
                println!("libm: set {MACHINE} to the machine's name to write the guide's report");
            }
            Err(e) => {
                eprintln!("libm: cannot write the report: {e}");
                return ExitCode::FAILURE;
            }
        }
        ExitCode::SUCCESS
    }

    /// Writes the report for the guide if `UMBRA_BENCH_MACHINE` names the
    /// machine.
    fn publish(&self) -> io::Result<Published> {
        let Some(machine) = env::var_os(MACHINE) else {
            return Ok(Published::NotAsked);
        };
        let machine = machine
            .into_string()
            .ok()
            .filter(|m| !m.is_empty() && m.bytes().all(|b| b.is_ascii_lowercase() || b.is_ascii_digit()))
            .ok_or_else(|| {
                io::Error::other(format!(
                    "{MACHINE} must be the machine's name in lowercase letters and digits, such as p620"
                ))
            })?;
        let id = format!("{machine}-{}", self.device.key());
        let dir = results_dir();
        fs::create_dir_all(&dir)?;
        let path = dir.join(format!("{id}.js"));
        fs::write(&path, self.script(&id, &machine))?;
        Ok(Published::Written(path))
    }

    /// The report as the script the guide loads: a comment and one record.
    fn script(&self, id: &str, machine: &str) -> String {
        let device = self.device.key();
        let record = self.record(id, &machine.to_uppercase());
        [
            format!("// The libm benchmark on {machine}'s {device}, written by `cargo bench --bench libm`"),
            format!("// with {MACHINE}={machine}: one record of the math guide's \"What libm costs\"."),
            format!("LIBM_RESULTS.push({record});"),
        ]
        .join("\n")
            + "\n"
    }

    /// The record: the header and a row per function, each column as
    /// `[ns, ulps, note]` or `null`.
    fn record(&self, id: &str, machine: &str) -> String {
        let mut out = format!(
            "{{ id: {}, machine: {}, kind: {}, device: {}, arch: {}, libc: {}, commit: {}, date: {}, rows: [",
            js(id),
            js(machine),
            js(self.device.key()),
            js(&self.model),
            js(env::consts::ARCH),
            js(&format!("glibc {}", glibc_version())),
            js(&commit()),
            js(&today()),
        );
        for row in &self.rows {
            let _ = write!(out, "{}, ", row.js());
        }
        out.push_str("] }");
        out
    }
}

/// `umbra-math`'s `docs/libm-results`, from either crate's benches.
fn results_dir() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../umbra-math/docs/libm-results")
}

/// The commit measured, abbreviated, or `unknown` outside a git checkout.
fn commit() -> String {
    Command::new("git")
        .args(["rev-parse", "--short", "HEAD"])
        .current_dir(env!("CARGO_MANIFEST_DIR"))
        .output()
        .ok()
        .filter(|out| out.status.success())
        .and_then(|out| String::from_utf8(out.stdout).ok())
        .map_or_else(|| "unknown".to_owned(), |s| s.trim().to_owned())
}

/// Today's date in UTC, as `YYYY-MM-DD`: the civil date of the day count
/// since 1970 (Howard Hinnant's `civil_from_days`).
fn today() -> String {
    let secs = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_or(0, |d| d.as_secs());
    let z = i64::try_from(secs / 86_400).unwrap_or_default() + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z.rem_euclid(146_097);
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let day = doy - (153 * mp + 2) / 5 + 1;
    let month = if mp < 10 { mp + 3 } else { mp - 9 };
    let year = yoe + era * 400 + i64::from(month <= 2);
    format!("{year:04}-{month:02}-{day:02}")
}

/// glibc's version, as `gnu_get_libc_version` gives it.
#[expect(unsafe_code, reason = "glibc's version string, through FFI")]
fn glibc_version() -> String {
    unsafe extern "C" {
        fn gnu_get_libc_version() -> *const c_char;
    }
    // SAFETY: a glibc function without arguments or preconditions.
    let version = unsafe { gnu_get_libc_version() };
    // SAFETY: glibc returns a static, NUL-terminated C string.
    unsafe { CStr::from_ptr(version) }
        .to_string_lossy()
        .into_owned()
}

/// `s` as a JavaScript string literal, safe inside a `<script>`.
fn js(s: &str) -> String {
    let mut out = String::from("\"");
    for ch in s.chars() {
        match ch {
            '"' | '\\' => {
                out.push('\\');
                out.push(ch);
            }
            '<' => out.push_str("\\u003c"),
            ch if ch.is_control() => {
                let _ = write!(out, "\\u{:04x}", u32::from(ch));
            }
            ch => out.push(ch),
        }
    }
    out.push('"');
    out
}
