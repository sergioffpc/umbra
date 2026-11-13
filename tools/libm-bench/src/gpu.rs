//! The GPU's three implementations of each function, from the kernels in
//! `kernels/`: `libdevice`, umbra-math's `nvptx64` backend, and the
//! approximate intrinsics. The time is the device's throughput: kernel time
//! over every evaluation of a launch that fills the GPU.

use std::sync::Arc;
use std::time::Instant;

use cudarc::driver::{
    CudaContext, CudaFunction, CudaModule, CudaSlice, CudaStream, LaunchConfig, PushKernelArg,
};
use cudarc::nvrtc::Ptx;

use crate::funcs::{BATCH, FUNCS, Func, Inputs, UlpError};
use crate::{Cell, Column, Header, record};

/// Threads per launch: enough to fill any current GPU several times over.
const THREADS: u32 = 1 << 20;
const BLOCK: u32 = 256;
/// Evaluations per thread: a launch of 2³¹ evaluations takes milliseconds,
/// so the launch overhead is lost in it.
const ITERS: u32 = 2048;
const SAMPLES: usize = 21;

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

fn err(e: impl std::fmt::Debug) -> String {
    format!("CUDA: {e:?}")
}

fn config(threads: u32) -> LaunchConfig {
    LaunchConfig {
        grid_dim: (threads.div_ceil(BLOCK), 1, 1),
        block_dim: (BLOCK, 1, 1),
        shared_mem_bytes: 0,
    }
}

/// The largest error of `eval_*` on the batch.
fn error(
    stream: &Arc<CudaStream>,
    eval: &CudaFunction,
    args: &Args,
    func: &Func,
    inputs: &Inputs,
) -> Result<UlpError, String> {
    let mut o1 = stream.alloc_zeros::<f32>(BATCH).map_err(err)?;
    let mut o2 = stream.alloc_zeros::<f32>(BATCH).map_err(err)?;
    #[expect(clippy::cast_possible_truncation, reason = "BATCH is 1024")]
    let launch = config(BATCH as u32);
    // SAFETY: the kernel reads `BATCH` floats of each input and writes
    // `BATCH` of each output, all allocated at that length.
    unsafe {
        stream
            .launch_builder(eval)
            .arg(&args.a)
            .arg(&args.b)
            .arg(&args.c)
            .arg(&mut o1)
            .arg(&mut o2)
            .launch(launch)
    }
    .map_err(err)?;
    let (h1, h2) = (
        stream.clone_dtoh(&o1).map_err(err)?,
        stream.clone_dtoh(&o2).map_err(err)?,
    );
    Ok(UlpError::max(func, inputs, &h1, &h2))
}

/// The median time per evaluation of `bench_*`, in nanoseconds.
fn time(
    stream: &Arc<CudaStream>,
    bench: &CudaFunction,
    args: &Args,
    acc: &mut CudaSlice<u32>,
) -> Result<f64, String> {
    let mut launch = || -> Result<f64, String> {
        let t = Instant::now();
        // SAFETY: the kernel reads the 1024-float inputs at indices masked
        // to 1023 and writes one `u32` per thread into `acc`, `THREADS` long.
        unsafe {
            stream
                .launch_builder(bench)
                .arg(&args.a)
                .arg(&args.b)
                .arg(&args.c)
                .arg(&mut *acc)
                .arg(&ITERS)
                .launch(config(THREADS))
        }
        .map_err(err)?;
        stream.synchronize().map_err(err)?;
        Ok(t.elapsed().as_secs_f64() * 1e9)
    };
    for _ in 0..3 {
        launch()?;
    }
    let mut samples = (0..SAMPLES)
        .map(|_| launch())
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
) -> Option<(Column, CudaFunction, CudaFunction)> {
    let kernel = format!("{}_{}", func.name, suffix(column));
    let bench = module.load_function(&format!("bench_{kernel}")).ok()?;
    let eval = module.load_function(&format!("eval_{kernel}")).ok()?;
    Some((column, bench, eval))
}

/// Measures every function on GPU 0, with the kernels of the PTX file.
pub fn run(header: &Header<'_>, ptx_path: &str) -> Result<String, String> {
    let ctx = CudaContext::new(0).map_err(err)?;
    let device = ctx.name().map_err(err)?;
    let ptx = std::fs::read_to_string(ptx_path).map_err(|e| format!("{ptx_path}: {e}"))?;
    let module = ctx.load_module(Ptx::from_src(ptx)).map_err(err)?;
    let stream = ctx.default_stream();
    let mut acc = stream.alloc_zeros::<u32>(THREADS as usize).map_err(err)?;

    let mut rows = Vec::new();
    for func in FUNCS {
        let inputs = (func.inputs)();
        let args = Args {
            a: stream.clone_htod(&inputs.a).map_err(err)?,
            b: stream.clone_htod(&inputs.b).map_err(err)?,
            c: stream.clone_htod(&inputs.c).map_err(err)?,
        };
        let mut cells = Vec::new();
        for (column, bench, eval) in Column::ALL
            .into_iter()
            .filter_map(|c| kernels(&module, func, c))
        {
            let ulps = error(&stream, &eval, &args, func, &inputs)?;
            let ns = time(&stream, &bench, &args, &mut acc)?;
            eprintln!(
                "{:8} {:8} {:9.3} ps  {ulps:>5} ULP",
                func.name,
                column.key(),
                ns * 1e3
            );
            cells.push(Cell {
                column,
                ns,
                ulps,
                note: note(func.name, column),
            });
        }
        rows.push((func.name, cells));
    }
    Ok(record(header, &device, "gpu", &rows))
}
