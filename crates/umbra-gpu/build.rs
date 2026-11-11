//! Compiles `umbra-kernels` for `nvptx64-nvidia-cuda` into a target
//! directory of its own and copies its PTX to `OUT_DIR`, which
//! `src/tracer.rs` embeds (ADR 0028).
//!
//! The kernels are built by a nested `cargo rustc`, in release, without
//! debug information, and with CUDA's `libdevice.10.bc` linked in by the
//! toolchain's `llvm-bitcode-linker`, which inlines the `__nv_*` functions
//! that `umbra-math`'s `nvptx64` backend declares (#146). The bitcode is
//! looked for at `UMBRA_LIBDEVICE`, then under `CUDA_PATH` and `CUDA_HOME`,
//! then in `/usr/local/cuda`. Without it, as on CI, which has no CUDA, the
//! kernels still build, with one `.extern .func` per `libdevice` function,
//! and a warning; `Tracer::new` then refuses the PTX.
//!
//! Building the kernels on every host build is also the check that no core
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
                            the kernels build, but the GPU cannot run them";

fn main() -> Result<(), Box<dyn Error>> {
    let out_dir = PathBuf::from(env::var_os("OUT_DIR").ok_or("cargo sets OUT_DIR")?);
    let manifest_dir =
        PathBuf::from(env::var_os("CARGO_MANIFEST_DIR").ok_or("cargo sets CARGO_MANIFEST_DIR")?);
    let kernels = manifest_dir.join("../umbra-kernels/Cargo.toml");
    let target_dir = out_dir.join("kernels");
    let libdevice = find_libdevice();

    let mut cargo = kernels_build(&kernels, &target_dir);
    match &libdevice {
        Some(path) => {
            cargo
                .arg("--")
                .arg(format!("-Clink-arg={}", path.display()));
        }
        None => println!("cargo::warning={NO_LIBDEVICE}"),
    }
    let status = cargo.status()?;
    if !status.success() {
        return Err(format!("building umbra-kernels for {TARGET} failed: {status}").into());
    }

    let built = target_dir.join(TARGET).join("release");
    let ptx = fs::read_to_string(built.join("umbra_kernels.ptx"))?;
    if libdevice.is_some() && ptx.contains(".extern .func") {
        return Err("libdevice was linked, but the PTX still has an .extern .func".into());
    }
    fs::write(out_dir.join("umbra_kernels.ptx"), ptx)?;
    rerun_if_sources_change(&built.join("umbra_kernels.d"))?;
    println!("cargo::rerun-if-changed={}", kernels.display());
    println!(
        "cargo::rerun-if-changed={}",
        manifest_dir.join("../../Cargo.lock").display()
    );
    for var in ["UMBRA_LIBDEVICE", "CUDA_PATH", "CUDA_HOME"] {
        println!("cargo::rerun-if-env-changed={var}");
    }
    Ok(())
}

/// The nested `cargo rustc` that builds the kernels' PTX.
///
/// The outer build's flags are for the host and are dropped: `RUSTFLAGS`
/// carries `-C target-cpu=x86-64-v3` in CI, which `nvptx64` does not have,
/// and wrappers such as `clippy-driver` or coverage instrumentation have no
/// business in the PTX. `-C target-cpu` stays at the target's default,
/// `sm_70`, whose PTX the driver compiles for any newer GPU; setting it would
/// mismatch the prebuilt `core`.
fn kernels_build(manifest: &Path, target_dir: &Path) -> Command {
    let mut cargo = Command::new(env::var_os("CARGO").unwrap_or_else(|| "cargo".into()));
    cargo
        .arg("rustc")
        .arg("--manifest-path")
        .arg(manifest)
        .args(["--release", "--locked", "--offline", "--target", TARGET])
        .arg("--target-dir")
        .arg(target_dir)
        .args(["--config", "profile.release.debug=false"])
        .args(["--config", "profile.release.strip=\"debuginfo\""]);
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

/// Tells cargo to rerun this script when any source file of the kernels'
/// build changes: those of `umbra-kernels` and of the core crates it uses,
/// as listed by the dep-info file of the build.
fn rerun_if_sources_change(dep_info: &Path) -> Result<(), Box<dyn Error>> {
    let dep_info = fs::read_to_string(dep_info)?;
    let (_, sources) = dep_info
        .split_once(": ")
        .ok_or("the kernels' dep-info file has no target")?;
    // Paths with spaces are escaped as `\ `; the workspace has none.
    for source in sources.split_whitespace() {
        println!("cargo::rerun-if-changed={source}");
    }
    Ok(())
}
