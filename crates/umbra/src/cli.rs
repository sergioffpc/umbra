//! The command line, with `clap`. Each argument's doc comment is its help:
//! the first line is the one-line summary `-h` shows, and the paragraphs
//! after it are the behaviour `--help` adds. The options of each command
//! are in alphabetical order, as `--help` lists them.

use std::net::SocketAddr;
use std::num::{NonZeroU32, NonZeroUsize};
use std::path::PathBuf;

use clap::{Args, Parser, Subcommand, ValueEnum};
use umbra_cluster::{ClusterName, DirtyBuilds, RenderDevice, RenderDevices};

use crate::logs::{LogFormat, parse_filter};

/// The examples of `umbra --help`.
const UMBRA_EXAMPLES: &str = "\
Examples:
    Render a Scene File on this machine alone:
        umbra render killeroo-simple.pbrt

    Keep a Worker running on another machine, and render with it too:
        umbra worker                          (on the other machine)
        umbra render killeroo-simple.pbrt     (on this one)";

/// The examples of `umbra render --help`.
const RENDER_EXAMPLES: &str = "\
Examples:
    Render on every Render Device of this machine, and on every Worker of the
    default Cluster that is up:
        umbra render killeroo-simple.pbrt

    Render a quick preview, at 16 Pixel Samples per pixel, to a file of your own:
        umbra render killeroo-simple.pbrt --spp 16 --outfile preview.exr

    Render on the CPU alone, with 8 threads:
        umbra render killeroo-simple.pbrt --render-devices cpu --nthreads 8

    Render with the Workers of the Cluster `r630`, on a fixed port that the
    firewall lets through, on the wired interface only:
        umbra render killeroo-simple.pbrt --cluster r630 --port 7470 --interface enp5s0";

/// The examples of `umbra worker --help`.
const WORKER_EXAMPLES: &str = "\
Examples:
    Join the Render Jobs of the default Cluster on every core of this machine:
        umbra worker

    Join those of the Cluster `r630`, on NUMA node 1 alone:
        numactl --cpunodebind=1 --membind=1 umbra worker --cluster r630

    Join them on the first GPU, from the release image:
        docker run -d --restart unless-stopped --network host --gpus device=0 \\
            -e NVIDIA_DRIVER_CAPABILITIES=compute,graphics \\
            ghcr.io/sergioffpc/umbra:X.Y.Z worker --render-device gpu:0";

/// Umbra: a physically based renderer of pbrt-v4 Scene Files.
#[derive(Debug, Parser)]
#[command(
    name = "umbra",
    version,
    about,
    after_long_help = UMBRA_EXAMPLES,
    next_display_order = None
)]
pub(crate) struct Cli {
    #[command(subcommand)]
    pub(crate) command: Command,
    /// The events to log on stderr, as a `RUST_LOG`-style filter.
    ///
    /// A level (`error`, `warn`, `info`, `debug`, `trace`), or one per
    /// module, such as `info,umbra_cluster=debug`. Local Workers log with
    /// the same filter, and their events are logged again by `umbra render`,
    /// prefixed with their Render Device.
    #[arg(long, global = true, default_value = "info", value_parser = parse_filter)]
    pub(crate) log: String,
    /// The format of the logs: `human` or `json`.
    ///
    /// Human-readable text on a terminal, one JSON object per line
    /// otherwise, unless this option chooses.
    #[arg(long, global = true, value_enum)]
    pub(crate) log_format: Option<LogFormat>,
}

/// The subcommands.
#[derive(Debug, Subcommand)]
pub(crate) enum Command {
    /// Render a Scene File into its final image and Render Job Report.
    ///
    /// Parses and checks the Scene File, announces the Render Job to the
    /// Workers of its Cluster on the LAN, starts a Local Worker for each
    /// Render Device of this machine, and renders on all of them. The image
    /// goes to the Film's file, and the Render Job Report to stdout. With no
    /// Worker left, the Render Job waits for one to join. Ctrl-C cancels
    /// it: the report is written, the image is not, and `umbra` exits 130.
    #[command(after_long_help = RENDER_EXAMPLES)]
    Render(RenderArgs),
    /// Join the Render Jobs of a Cluster, one after another, until stopped.
    ///
    /// Waits for a Render Job of its Cluster to be announced on the LAN,
    /// joins it, renders the Work Units it is given, and waits for the next
    /// once it ends. A Worker that fails, or loses its Coordinator, joins
    /// the same Render Job again after 2 s, 10 s, then every 30 s, while it
    /// is announced; a refused one never joins it again. Ctrl-C or SIGTERM
    /// stops it, and it exits 0.
    #[command(after_long_help = WORKER_EXAMPLES)]
    Worker(WorkerArgs),
}

// `umbra render`: pbrt-v4's overrides keep pbrt-v4's names (ADR 0003), and
// Umbra's own `--spp-per-pass` is named after `--spp`. The Coordinator's
// options follow ADR 0012, ADR 0021, ADR 0026 and ADR 0029.
#[derive(Debug, Args)]
#[command(next_display_order = None)]
pub(crate) struct RenderArgs {
    /// The pbrt-v4 Scene File to render.
    pub(crate) scene_file: PathBuf,
    /// Admit remote Workers built from a tree with uncommitted changes.
    ///
    /// Without it, a remote Worker whose build was dirty is refused, since
    /// it may not render as this one does. The Local Workers, this very
    /// executable, are always admitted. The report records the choice.
    #[arg(
        long = "allow-dirty",
        num_args = 0,
        default_value = "refused",
        default_missing_value = "allowed",
        value_parser = parse_dirty_builds
    )]
    pub(crate) dirty_builds: DirtyBuilds,
    /// The Cluster whose Workers render this Render Job.
    ///
    /// A DNS label: lowercase letters, digits and `-`, at most 63
    /// characters. Only the Workers started with the same Cluster Name join
    /// the Render Job, so that two Clusters can share a LAN. A name, not a
    /// secret.
    #[arg(long, env = "UMBRA_CLUSTER", default_value_t)]
    pub(crate) cluster: ClusterName,
    /// The network interface to listen and announce on, such as `enp5s0`.
    ///
    /// By default, every interface. With one named, the Render Job listens
    /// on that interface's IPv4 address alone and is announced on it alone.
    /// An interface that does not exist, or has no IPv4 address, is an error
    /// before anything is announced, which lists the interfaces.
    #[arg(long, env = "UMBRA_INTERFACE")]
    pub(crate) interface: Option<String>,
    /// Threads of the CPU's Local Worker; one per logical CPU by default.
    ///
    /// Remote Workers choose their own threads.
    #[arg(long)]
    pub(crate) nthreads: Option<NonZeroUsize>,
    /// The final image, instead of the Film's file; its extension names the
    /// format.
    ///
    /// A relative path is taken from the working directory, while the
    /// Film's own file name is taken from the Scene File's directory.
    #[arg(long)]
    pub(crate) outfile: Option<PathBuf>,
    /// The UDP port remote Workers connect to; one the OS picks by default.
    ///
    /// The Workers learn it from the announcement, so it matters only to a
    /// firewall: on a machine that drops incoming UDP, choose a port and let
    /// it through. A port already taken is an error before anything is
    /// announced. 0 asks for one the OS picks.
    #[arg(long, env = "UMBRA_PORT", default_value = "0")]
    pub(crate) port: u16,
    /// The Render Devices of this machine to render on: `cpu`, `gpu`, or
    /// both.
    ///
    /// Comma-separated. By default, the CPU and every GPU, or the CPU alone,
    /// with a warning, when no GPU is usable; `gpu` chosen with no GPU
    /// usable is an error. Each gets a Local Worker. It does not change the
    /// expected image.
    #[arg(long, value_enum, value_delimiter = ',')]
    pub(crate) render_devices: Vec<DeviceKind>,
    /// The samplers' seed, instead of the Scene File's; 0 by default.
    ///
    /// The same seed and options give a statistically equal image, and on
    /// one machine the same Film, bit for bit.
    #[arg(long, allow_negative_numbers = true)]
    pub(crate) seed: Option<i32>,
    /// Pixel Samples per pixel, instead of the Scene File's.
    ///
    /// More Pixel Samples give less noise, and take longer.
    #[arg(long)]
    pub(crate) spp: Option<NonZeroU32>,
    /// Pixel Samples per pixel in one Pass.
    ///
    /// How the work is cut into Work Units, not its quality: a Work Unit is
    /// one Tile for one Pass. Smaller Passes lose less work when a Worker
    /// fails; larger ones cost less to schedule. The Film is the same for
    /// the same value.
    #[arg(long, default_value = "64")]
    pub(crate) spp_per_pass: NonZeroU32,
    /// The side of a square Tile, in pixels.
    ///
    /// How the image is cut into Work Units, not its quality. The Film is
    /// the same for the same value.
    #[arg(long, default_value = "32")]
    pub(crate) tile_size: NonZeroU32,
}

/// `--allow-dirty`, which takes no value: `allowed` when it is given,
/// `refused` otherwise.
fn parse_dirty_builds(value: &str) -> Result<DirtyBuilds, String> {
    match value {
        "allowed" => Ok(DirtyBuilds::Allowed),
        "refused" => Ok(DirtyBuilds::Refused),
        _ => Err("`--allow-dirty` takes no value".to_owned()),
    }
}

/// A kind of Render Device, for `--render-devices`.
#[derive(Debug, Clone, Copy, PartialEq, Eq, ValueEnum)]
pub(crate) enum DeviceKind {
    /// The CPU's cores.
    Cpu,
    /// Every GPU.
    Gpu,
}

impl RenderArgs {
    /// The Render Devices `--render-devices` chooses.
    pub(crate) fn render_devices(&self) -> RenderDevices {
        let chosen = |kind| self.render_devices.contains(&kind);
        match (chosen(DeviceKind::Cpu), chosen(DeviceKind::Gpu)) {
            (false, false) => RenderDevices::Usable,
            (true, false) => RenderDevices::Cpu,
            (false, true) => RenderDevices::Gpu,
            (true, true) => RenderDevices::CpuAndGpu,
        }
    }
}

// `umbra worker`: a Worker that joins the Render Jobs of its Cluster as they
// are announced (ADR 0021); or, with `--coordinator`, the one Render Job of
// the Coordinator at that address, as a Local Worker does (ADR 0035).
#[derive(Debug, Args)]
#[command(next_display_order = None)]
pub(crate) struct WorkerArgs {
    /// The Cluster whose Render Jobs to join.
    ///
    /// A DNS label: lowercase letters, digits and `-`, at most 63
    /// characters. The Worker ignores the Render Jobs of other Clusters.
    #[arg(long, env = "UMBRA_CLUSTER", default_value_t)]
    pub(crate) cluster: ClusterName,
    /// Serve the one Render Job of the Coordinator at this address, then
    /// exit.
    ///
    /// How `umbra render` runs its Local Workers; not for the operator.
    #[arg(long, hide = true)]
    pub(crate) coordinator: Option<SocketAddr>,
    /// Threads to render with on the CPU; one per logical CPU by default.
    ///
    /// To bind a Worker to one NUMA node, or to leave a core to a GPU
    /// Worker, run it under `numactl` or `taskset`, or in a container with
    /// `--cpuset-cpus`, rather than lowering its threads.
    #[arg(long)]
    pub(crate) nthreads: Option<NonZeroUsize>,
    /// The Render Device to render on: `cpu`, or `gpu:N` for GPU `N`.
    ///
    /// Run one Worker per Render Device of a machine: one per NUMA node, and
    /// one per GPU. Each counts as its own Worker Slot.
    #[arg(long, default_value = "cpu")]
    pub(crate) render_device: RenderDevice,
}
