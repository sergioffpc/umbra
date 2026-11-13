//! Compiles `umbra-gpu-programs` for `nvptx64-nvidia-cuda` into a target
//! directory of its own and copies its PTX to `OUT_DIR`, which
//! `src/tracer.rs` embeds (ADR 0028, ADR 0033).
//!
//! The programs are built by a nested `cargo rustc --crate-type cdylib`,
//! which the `nvptx64` target links into one PTX module, in release, without
//! debug information, and with CUDA's `libdevice.10.bc` linked in by the
//! toolchain's `llvm-bitcode-linker`, which inlines the `__nv_*` functions
//! that `umbra-math`'s `nvptx64` backend declares (#146). The bitcode is
//! looked for at `UMBRA_LIBDEVICE`, then under `CUDA_PATH` and `CUDA_HOME`,
//! then in `/usr/local/cuda`. Without it, as on CI, which has no CUDA, the
//! programs still build, with one `.extern .func` per `libdevice` function,
//! and a warning; `Tracer::new` then refuses the PTX.
//!
//! With this crate's `math-program` feature, which the GPU tests turn on,
//! the programs are built with theirs, and the PTX has the math program.
//! With its `libm-bench` feature, the script also builds a second PTX,
//! `libm_bench.ptx`, of the libm bench's plain CUDA kernels, for
//! `benches/libm.rs`.
//!
//! Building the programs on every host build is also the check that no core
//! crate pulls `std`: `nvptx64` has none.

use std::env;
use std::error::Error;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;

/// The GPU target (ADR 0028).
const TARGET: &str = "nvptx64-nvidia-cuda";

/// Where `libdevice` lives under a CUDA installation.
const LIBDEVICE: &str = "nvvm/libdevice/libdevice.10.bc";

/// The warning of a build without `libdevice`.
const NO_LIBDEVICE: &str = "libdevice.10.bc not found (set UMBRA_LIBDEVICE or CUDA_PATH): \
                            the GPU programs build, but the GPU cannot run them";

/// A PTX module built from `umbra-gpu-programs`.
#[derive(Clone, Copy, Debug)]
enum Programs {
    /// The OptiX programs the renderer runs, with the math program under
    /// this crate's `math-program` feature: embedded by `src/tracer.rs`.
    Tracer,
    /// The plain CUDA kernels of the libm bench, under `--cfg
    /// umbra_libm_bench` and without the OptiX programs, which a CUDA module
    /// cannot load: built under this crate's `libm-bench` feature for
    /// `benches/libm.rs`.
    LibmBench,
}

impl Programs {
    /// The build's own target directory under `OUT_DIR`: the two take
    /// different flags, so sharing one would rebuild both every time.
    fn target_dir(self) -> &'static str {
        match self {
            Self::Tracer => "programs",
            Self::LibmBench => "libm-bench",
        }
    }

    /// The PTX's name in `OUT_DIR`.
    fn ptx(self) -> &'static str {
        match self {
            Self::Tracer => "umbra_gpu_programs.ptx",
            Self::LibmBench => "libm_bench.ptx",
        }
    }
}

fn main() -> Result<(), Box<dyn Error>> {
    let out_dir = PathBuf::from(env::var_os("OUT_DIR").ok_or("cargo sets OUT_DIR")?);
    let manifest_dir =
        PathBuf::from(env::var_os("CARGO_MANIFEST_DIR").ok_or("cargo sets CARGO_MANIFEST_DIR")?);
    let manifest = manifest_dir.join("../umbra-gpu-programs/Cargo.toml");
    let libdevice = find_libdevice();
    if libdevice.is_none() {
        println!("cargo::warning={NO_LIBDEVICE}");
    }

    build(Programs::Tracer, &manifest, &out_dir, libdevice.as_deref())?;
    if env::var_os("CARGO_FEATURE_LIBM_BENCH").is_some() {
        build(
            Programs::LibmBench,
            &manifest,
            &out_dir,
            libdevice.as_deref(),
        )?;
    }
    println!("cargo::rerun-if-changed={}", manifest.display());
    println!(
        "cargo::rerun-if-changed={}",
        manifest_dir.join("../../Cargo.lock").display()
    );
    for var in ["UMBRA_LIBDEVICE", "CUDA_PATH", "CUDA_HOME"] {
        println!("cargo::rerun-if-env-changed={var}");
    }
    Ok(())
}

/// Builds `programs` and copies their PTX to `OUT_DIR`, with `libdevice`
/// linked in where there is one.
fn build(
    programs: Programs,
    manifest: &Path,
    out_dir: &Path,
    libdevice: Option<&Path>,
) -> Result<(), Box<dyn Error>> {
    let target_dir = out_dir.join(programs.target_dir());
    let mut cargo = programs_build(manifest, &target_dir, programs);
    let link = libdevice.map(|path| format!("-Clink-arg={}", path.display()));
    let cfg = match programs {
        Programs::Tracer => None,
        Programs::LibmBench => Some("--cfg=umbra_libm_bench".to_owned()),
    };
    let rustc_args: Vec<String> = cfg.into_iter().chain(link).collect();
    if !rustc_args.is_empty() {
        cargo.arg("--").args(rustc_args);
    }
    let status = cargo.status()?;
    if !status.success() {
        return Err(format!("building umbra-gpu-programs for {TARGET} failed: {status}").into());
    }

    let built = target_dir.join(TARGET).join("release");
    let ptx = fs::read_to_string(built.join("umbra_gpu_programs.ptx"))?;
    if libdevice.is_some() && ptx.contains(".extern .func") {
        return Err("libdevice was linked, but the PTX still has an .extern .func".into());
    }
    fs::write(out_dir.join(programs.ptx()), ptx)?;
    rerun_if_sources_change(&built.join("umbra_gpu_programs.d"))
}

/// The nested `cargo rustc` that builds the programs' PTX: their library as
/// a `cdylib`, which is an `rlib` in their manifest for this crate's
/// dependency on their layout.
///
/// The outer build's flags are for the host and are dropped: `RUSTFLAGS`
/// carries `-C target-cpu=x86-64-v3` in CI, which `nvptx64` does not have,
/// and wrappers such as `clippy-driver` or coverage instrumentation have no
/// business in the PTX. `-C target-cpu` stays at the target's default,
/// `sm_70`, whose PTX the driver compiles for any newer GPU; setting it would
/// mismatch the prebuilt `core`.
fn programs_build(manifest: &Path, target_dir: &Path, programs: Programs) -> Command {
    let mut cargo = Command::new(env::var_os("CARGO").unwrap_or_else(|| "cargo".into()));
    cargo
        .arg("rustc")
        .arg("--manifest-path")
        .arg(manifest)
        .args(["--release", "--locked", "--offline", "--target", TARGET])
        .args(["--lib", "--crate-type", "cdylib"])
        .arg("--target-dir")
        .arg(target_dir)
        .args(["--config", "profile.release.debug=false"])
        .args(["--config", "profile.release.strip=\"debuginfo\""]);
    if let Programs::Tracer = programs
        && env::var_os("CARGO_FEATURE_MATH_PROGRAM").is_some()
    {
        cargo.args(["--features", "math-program"]);
    }
    for var in [
        "RUSTFLAGS",
        "CARGO_ENCODED_RUSTFLAGS",
        "CARGO_BUILD_RUSTFLAGS",
        "RUSTC_WRAPPER",
        "RUSTC_WORKSPACE_WRAPPER",
        "CARGO_TARGET_DIR",
        "CARGO_BUILD_TARGET",
    ] {
        cargo.env_remove(var);
    }
    cargo
}

/// `libdevice.10.bc`: at `UMBRA_LIBDEVICE`, or under `CUDA_PATH`,
/// `CUDA_HOME` or `/usr/local/cuda`, whichever exists first.
fn find_libdevice() -> Option<PathBuf> {
    if let Some(path) = env::var_os("UMBRA_LIBDEVICE") {
        return Some(PathBuf::from(path));
    }
    ["CUDA_PATH", "CUDA_HOME"]
        .iter()
        .filter_map(env::var_os)
        .map(PathBuf::from)
        .chain([PathBuf::from("/usr/local/cuda")])
        .map(|root| root.join(LIBDEVICE))
        .find(|path| path.is_file())
}

/// Tells cargo to rerun this script when any source file of the programs'
/// build changes: those of `umbra-gpu-programs` and of the core crates it uses,
/// as listed by the dep-info file of the build.
fn rerun_if_sources_change(dep_info: &Path) -> Result<(), Box<dyn Error>> {
    let dep_info = fs::read_to_string(dep_info)?;
    let (_, sources) = dep_info
        .split_once(": ")
        .ok_or("the programs' dep-info file has no target")?;
    // Paths with spaces are escaped as `\ `; the workspace has none.
    for source in sources.split_whitespace() {
        println!("cargo::rerun-if-changed={source}");
    }
    Ok(())
}
