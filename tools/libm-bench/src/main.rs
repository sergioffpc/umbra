//! The libm benchmark: umbra-math's `libm` functions on the CPU and the GPU,
//! each against its baseline and the best SIMD or fast form there is. Run by
//! `scripts/bench-libm.sh`; see `tools/libm-bench/README.md`.
//!
//! ```text
//! libm-bench cpu <id> <machine> <commit> <date>         one result record
//! libm-bench gpu <id> <machine> <commit> <date> <ptx>   one result record
//! libm-bench guide <html> <record file>...              splice into the guide
//! ```
//!
//! A record is one line of JavaScript, an object literal, so that each
//! machine's results are one line of the guide: running on another machine
//! touches another line, and the branches merge without conflict.

#![expect(
    clippy::disallowed_types,
    reason = "a host tool: glibc's f64 routines are the reference, and times are f64"
)]

mod cpu;
mod funcs;
#[cfg(feature = "gpu")]
mod gpu;

use std::ffi::{CStr, c_char};
use std::fmt::Write as _;
use std::time::{Duration, Instant};
use std::{env, fs, process};

use funcs::{BATCH, FUNCS, Inputs, UlpError};

/// Maps a batch: inputs, first output, second output (`sincosf`'s cosine).
pub type Run = Box<dyn Fn(&Inputs, &mut [f32], &mut [f32])>;

/// The three columns of the table.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Column {
    /// glibc's scalar `libm` on the CPU, `libdevice` on the GPU.
    Baseline,
    /// umbra-math as it is.
    Umbra,
    /// The fastest form: `libmvec` or the vector instruction on the CPU, the
    /// approximate intrinsics on the GPU.
    Best,
}

impl Column {
    const ALL: [Self; 3] = [Self::Baseline, Self::Umbra, Self::Best];

    fn key(self) -> &'static str {
        match self {
            Self::Baseline => "base",
            Self::Umbra => "umbra",
            Self::Best => "best",
        }
    }
}

/// One implementation of a function.
pub struct Implementation {
    pub column: Column,
    /// What it is, for the table: a symbol or a short description.
    pub note: String,
    pub run: Run,
}

/// One measured cell: median time per element, largest error, and what ran.
pub struct Cell {
    pub column: Column,
    pub ns: f64,
    pub ulps: UlpError,
    pub note: String,
}

/// The median of 201 timings of `f`, each repeated to at least 200 µs.
pub fn median_ns(mut f: impl FnMut()) -> f64 {
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

/// The machine's description, written by the script into a record.
struct Header<'a> {
    id: &'a str,
    machine: &'a str,
    commit: &'a str,
    date: &'a str,
}

impl<'a> Header<'a> {
    fn from_args(args: &'a [String]) -> Option<Self> {
        match args {
            [id, machine, commit, date, ..] => Some(Self {
                id,
                machine,
                commit,
                date,
            }),
            _ => None,
        }
    }
}

/// glibc's version, as `gnu_get_libc_version` gives it.
fn glibc_version() -> String {
    unsafe extern "C" {
        fn gnu_get_libc_version() -> *const c_char;
    }
    // SAFETY: glibc returns a static C string.
    unsafe { CStr::from_ptr(gnu_get_libc_version()) }
        .to_string_lossy()
        .into_owned()
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

/// `s` as a JavaScript string literal.
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

/// A cell as `[ns, ulps, note]`, `ulps` null for a NaN mismatch, or `null`
/// for a column the function does not have.
fn cell_js(cell: Option<&Cell>) -> String {
    cell.map_or_else(
        || "null".to_owned(),
        |c| {
            let ulps = match c.ulps {
                UlpError::Ulps(n) => n.to_string(),
                UlpError::Nan => "null".to_owned(),
            };
            format!("[{:.6}, {ulps}, {}]", c.ns, js(&c.note))
        },
    )
}

/// One record: the header and a row per function, each column as
/// `[ns, ulps, note]` or `null`.
fn record(header: &Header<'_>, device: &str, kind: &str, rows: &[(&str, Vec<Cell>)]) -> String {
    let mut out = format!(
        "{{ id: {}, machine: {}, kind: {}, device: {}, arch: {}, libc: {}, commit: {}, date: {}, rows: [",
        js(header.id),
        js(header.machine),
        js(kind),
        js(device),
        js(env::consts::ARCH),
        js(&format!("glibc {}", glibc_version())),
        js(header.commit),
        js(header.date),
    );
    for (name, cells) in rows {
        let _ = write!(out, "{{ f: {}", js(name));
        for column in Column::ALL {
            let value = cell_js(cells.iter().find(|c| c.column == column));
            let _ = write!(out, ", {}: {value}", column.key());
        }
        out.push_str(" }, ");
    }
    out.push_str("] },");
    out
}

/// Measures every function on the CPU.
fn run_cpu(header: &Header<'_>) -> String {
    let rows: Vec<(&str, Vec<Cell>)> = FUNCS
        .iter()
        .map(|func| {
            let inputs = (func.inputs)();
            let cells = cpu::implementations(func.name)
                .into_iter()
                .map(|imp| {
                    let (mut o1, mut o2) = (vec![0.0; BATCH], vec![0.0; BATCH]);
                    (imp.run)(&inputs, &mut o1, &mut o2);
                    let ulps = UlpError::max(func, &inputs, &o1, &o2);
                    #[expect(clippy::cast_precision_loss, reason = "BATCH is small")]
                    let ns = median_ns(|| (imp.run)(&inputs, &mut o1, &mut o2)) / BATCH as f64;
                    eprintln!(
                        "{:8} {:8} {ns:9.3} ns  {ulps:>4} ULP  {}",
                        func.name,
                        imp.column.key(),
                        imp.note
                    );
                    Cell {
                        column: imp.column,
                        ns,
                        ulps,
                        note: imp.note,
                    }
                })
                .collect();
            (func.name, cells)
        })
        .collect();
    record(header, &cpu_model(), "cpu", &rows)
}

/// The markers around the records in the guide.
const BEGIN: &str = "// libm-bench results: begin";
const END: &str = "// libm-bench results: end";

/// The record's id, from its `id: "…"` field.
fn record_id(line: &str) -> Option<&str> {
    let rest = line.trim_start().strip_prefix("{ id: \"")?;
    rest.split_once('"').map(|(id, _)| id)
}

/// Replaces the records of the same ids in the guide, or appends them.
fn splice(html: &str, records: &[String]) -> Result<String, String> {
    let begin = html.find(BEGIN).ok_or("the guide has no results block")?;
    let body = begin
        + html[begin..]
            .find('\n')
            .ok_or("no newline after the block start")?
        + 1;
    let marker = html[body..]
        .find(END)
        .ok_or("the results block has no end")?
        + body;
    // The end marker's line, with its indentation, which the records take.
    let end = html[..marker].rfind('\n').map_or(body, |n| n + 1).max(body);
    let indent = &html[end..marker];
    let mut lines: Vec<String> = html[body..end]
        .lines()
        .map(str::to_owned)
        .filter(|l| !l.trim().is_empty())
        .collect();
    for record in records {
        let id = record_id(record).ok_or("a record without an id")?;
        let line = format!("{indent}{record}");
        match lines.iter_mut().find(|l| record_id(l) == Some(id)) {
            Some(existing) => *existing = line,
            None => lines.push(line),
        }
    }
    Ok(format!(
        "{}{}\n{}",
        &html[..body],
        lines.join("\n"),
        &html[end..]
    ))
}

fn guide(args: &[String]) -> Result<(), String> {
    let [html_path, record_paths @ ..] = args else {
        return Err("usage: libm-bench guide <html> <record file>...".to_owned());
    };
    let records = record_paths
        .iter()
        .map(|p| {
            fs::read_to_string(p)
                .map(|s| s.trim().to_owned())
                .map_err(|e| format!("{p}: {e}"))
        })
        .collect::<Result<Vec<_>, _>>()?;
    let html = fs::read_to_string(html_path).map_err(|e| format!("{html_path}: {e}"))?;
    fs::write(html_path, splice(&html, &records)?).map_err(|e| format!("{html_path}: {e}"))
}

const USAGE: &str = concat!(
    "usage: libm-bench cpu <id> <machine> <commit> <date>\n",
    "       libm-bench gpu <id> <machine> <commit> <date> <ptx>\n",
    "       libm-bench guide <html> <record file>...",
);

fn main() {
    let args: Vec<String> = env::args().skip(1).collect();
    let result = match args.split_first() {
        Some((command, rest)) if command == "cpu" => Header::from_args(rest)
            .map(|h| println!("{}", run_cpu(&h)))
            .ok_or_else(|| USAGE.to_owned()),
        #[cfg(feature = "gpu")]
        Some((command, rest)) if command == "gpu" => match (Header::from_args(rest), rest.get(4)) {
            (Some(h), Some(ptx)) => gpu::run(&h, ptx).map(|r| println!("{r}")),
            _ => Err(USAGE.to_owned()),
        },
        Some((command, rest)) if command == "guide" => guide(rest),
        _ => Err(USAGE.to_owned()),
    };
    if let Err(message) = result {
        eprintln!("libm-bench: {message}");
        process::exit(1);
    }
}
