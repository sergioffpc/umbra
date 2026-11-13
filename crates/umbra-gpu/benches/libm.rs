//! What `libm` costs on the GPU: umbra-math's 19 `f32` functions of the C
//! library, each three ways (CUDA's `libdevice`, umbra-math's `nvptx64`
//! backend, and the approximate intrinsics), with the time per evaluation
//! and the largest error in ULPs. The counterpart of `umbra-math`'s
//! `benches/libm.rs`, whose table, inputs and report it shares.
//!
//! ```sh
//! cargo bench -p umbra-gpu --features libm-bench --bench libm
//! UMBRA_BENCH_MACHINE=p620 cargo bench -p umbra-gpu --features libm-bench --bench libm
//! ```
//!
//! It needs an NVIDIA GPU and CUDA's `libdevice`, which the build script
//! links into the kernels' PTX (`umbra-gpu-programs`' `libm_bench.rs`).
//! The time is throughput, not latency: 2²⁰ threads evaluate 2048 inputs
//! each from the 1024-entry inputs, which stay in L1, and XOR the results'
//! bits so none is dead code; the median of 21 launches, divided by their
//! 2³¹ evaluations.

#[path = "../../umbra-math/benches/libm/funcs.rs"]
mod funcs;
#[path = "../../umbra-math/benches/libm/report.rs"]
mod report;

use std::process::ExitCode;
use std::sync::Arc;
use std::time::Instant;

use cudarc::driver::{
    CudaContext, CudaFunction, CudaModule, CudaSlice, CudaStream, DriverError, LaunchConfig,
    PushKernelArg,
};
use cudarc::nvrtc::Ptx;

use crate::funcs::{BATCH, FUNCS, Func, Inputs, UlpError};
use crate::report::{Cell, Column, Device, Report, Row};

/// The kernels, compiled by the build script under the `libm-bench` feature.
const PTX: &str = include_str!(concat!(env!("OUT_DIR"), "/libm_bench.ptx"));

/// Threads per launch: enough to fill any current GPU several times over.
const THREADS: u32 = 1 << 20;
const BLOCK: u32 = 256;
/// Evaluations per thread: a launch of 2³¹ evaluations takes milliseconds,
/// so the launch overhead is lost in it.
const ITERS: u32 = 2048;
const SAMPLES: usize = 21;

/// Why the bench cannot run.
#[derive(Debug, thiserror::Error)]
enum Error {
    #[error(
        "the kernels were built without CUDA's libdevice: set UMBRA_LIBDEVICE or CUDA_PATH and rebuild"
    )]
    NoLibdevice,
    #[error("CUDA: {0}")]
    Cuda(#[from] DriverError),
}

/// The kernels' suffix for each column.
fn suffix(column: Column) -> &'static str {
    match column {
        Column::Baseline => "libdevice",
        Column::Umbra => "umbra",
        Column::Best => "fast",
    }
}

/// What each kernel evaluates, for the table.
fn note(func: &str, column: Column) -> String {
    match (column, func) {
        (Column::Baseline, "sqrtf") => "__nv_fsqrt_rn".to_owned(),
        (Column::Baseline, _) => format!("__nv_{func}"),
        (Column::Best, "exp2f") => "ex2.approx.ftz".to_owned(),
        (Column::Best, "sqrtf") => "sqrt.approx.ftz".to_owned(),
        (Column::Best, _) => format!("__nv_fast_{func}"),
        (Column::Umbra, "sincosf") => "own sin_cos: fast sin, cos after own reduction".to_owned(),
        (Column::Umbra, "atan2f") => "atan2: __nv_atan2f, subnormals scaled".to_owned(),
        (Column::Umbra, "erff") => "erf: __nv_erff".to_owned(),
        (Column::Umbra, "expf") => "Vec4::exp: __nv_expf per lane".to_owned(),
        (Column::Umbra, "powf") => "Vec4::pow: __nv_powf per lane".to_owned(),
        (Column::Umbra, "sqrtf" | "fmaf" | "floorf" | "ceilf") => {
            format!("{}, the instruction", func.trim_end_matches('f'))
        }
        (Column::Umbra, _) => format!("wrapper {func}: __nv_{func}"),
    }
}

/// A function's inputs on the device.
struct Args {
    a: CudaSlice<f32>,
    b: CudaSlice<f32>,
    c: CudaSlice<f32>,
}

fn config(threads: u32) -> LaunchConfig {
    LaunchConfig {
        grid_dim: (threads.div_ceil(BLOCK), 1, 1),
        block_dim: (BLOCK, 1, 1),
        shared_mem_bytes: 0,
    }
}

/// The largest error of `eval_*` on the batch.
#[expect(unsafe_code, reason = "launches a CUDA kernel")]
fn error(
    stream: &Arc<CudaStream>,
    eval: &CudaFunction,
    args: &Args,
    func: &Func,
    inputs: &Inputs,
) -> Result<UlpError, Error> {
    let mut o1 = stream.alloc_zeros::<f32>(BATCH)?;
    let mut o2 = stream.alloc_zeros::<f32>(BATCH)?;
    #[expect(clippy::cast_possible_truncation, reason = "BATCH is 1024")]
    let launch = config(BATCH as u32);
    let mut builder = stream.launch_builder(eval);
    builder
        .arg(&args.a)
        .arg(&args.b)
        .arg(&args.c)
        .arg(&mut o1)
        .arg(&mut o2);
    // SAFETY: the kernel reads `BATCH` floats of each input and writes
    // `BATCH` of each output, all allocated at that length.
    unsafe { builder.launch(launch) }?;
    let (h1, h2) = (stream.clone_dtoh(&o1)?, stream.clone_dtoh(&o2)?);
    Ok(UlpError::max(func, inputs, &h1, &h2))
}

/// The time of one launch of `bench_*`, in nanoseconds.
#[expect(unsafe_code, reason = "launches a CUDA kernel")]
fn launch(
    stream: &Arc<CudaStream>,
    bench: &CudaFunction,
    args: &Args,
    acc: &mut CudaSlice<u32>,
) -> Result<f64, Error> {
    let t = Instant::now();
    let mut builder = stream.launch_builder(bench);
    builder
        .arg(&args.a)
        .arg(&args.b)
        .arg(&args.c)
        .arg(&mut *acc)
        .arg(&ITERS);
    // SAFETY: the kernel reads the 1024-float inputs at indices masked to
    // 1023 and writes one `u32` per thread into `acc`, `THREADS` long.
    unsafe { builder.launch(config(THREADS)) }?;
    stream.synchronize()?;
    Ok(t.elapsed().as_secs_f64() * 1e9)
}

/// The median time per evaluation of `bench_*`, in nanoseconds, after three
/// launches to warm up.
fn time(
    stream: &Arc<CudaStream>,
    bench: &CudaFunction,
    args: &Args,
    acc: &mut CudaSlice<u32>,
) -> Result<f64, Error> {
    for _ in 0..3 {
        launch(stream, bench, args, acc)?;
    }
    let mut samples = (0..SAMPLES)
        .map(|_| launch(stream, bench, args, acc))
        .collect::<Result<Vec<_>, _>>()?;
    samples.sort_by(f64::total_cmp);
    Ok(samples[SAMPLES / 2] / (f64::from(THREADS) * f64::from(ITERS)))
}

/// The `bench_*` and `eval_*` kernels of a function's column, if it has
/// that column on the GPU.
fn kernels(
    module: &Arc<CudaModule>,
    func: &Func,
    column: Column,
) -> Option<(CudaFunction, CudaFunction)> {
    let kernel = format!("{}_{}", func.name, suffix(column));
    let bench = module.load_function(&format!("bench_{kernel}")).ok()?;
    let eval = module.load_function(&format!("eval_{kernel}")).ok()?;
    Some((bench, eval))
}

/// The GPU, its kernels and the accumulator every launch writes.
struct Gpu {
    stream: Arc<CudaStream>,
    module: Arc<CudaModule>,
    acc: CudaSlice<u32>,
}

impl Gpu {
    /// Every column of `func` the GPU has, measured.
    fn measure(&mut self, func: &Func) -> Result<Row, Error> {
        let inputs = (func.inputs)();
        let args = Args {
            a: self.stream.clone_htod(&inputs.a)?,
            b: self.stream.clone_htod(&inputs.b)?,
            c: self.stream.clone_htod(&inputs.c)?,
        };
        let cells = Column::ALL
            .into_iter()
            .filter_map(|column| Some((column, kernels(&self.module, func, column)?)))
            .map(|(column, (bench, eval))| {
                let cell = Cell {
                    column,
                    ulps: error(&self.stream, &eval, &args, func, &inputs)?,
                    ns: time(&self.stream, &bench, &args, &mut self.acc)?,
                    note: note(func.name, column),
                };
                cell.print(func.name, Device::Gpu);
                Ok(cell)
            })
            .collect::<Result<_, Error>>()?;
        Ok(Row {
            func: func.name,
            cells,
        })
    }
}

/// Measures every function on GPU 0.
fn run() -> Result<Report, Error> {
    if PTX.contains(".extern .func") {
        return Err(Error::NoLibdevice);
    }
    let ctx = CudaContext::new(0)?;
    let model = ctx.name()?;
    let stream = ctx.default_stream();
    let mut gpu = Gpu {
        module: ctx.load_module(Ptx::from_src(PTX))?,
        acc: stream.alloc_zeros::<u32>(THREADS as usize)?,
        stream,
    };
    let rows = FUNCS
        .iter()
        .map(|func| gpu.measure(func))
        .collect::<Result<_, _>>()?;
    Ok(Report {
        device: Device::of_this_bench(),
        model,
        rows,
    })
}

#[expect(
    clippy::print_stderr,
    reason = "why the bench cannot run is its output"
)]
fn main() -> ExitCode {
    match run() {
        Ok(report) => report.finish(),
        Err(e) => {
            eprintln!("libm: {e}");
            ExitCode::FAILURE
        }
    }
}
