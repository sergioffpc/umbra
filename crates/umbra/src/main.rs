#![doc = include_str!("../README.md")]

mod build_identity;
mod cli;
mod error;
mod logs;
mod render;
mod worker;

use std::io::{self, Write};
use std::panic;
use std::process::{self, ExitCode};
use std::sync::Arc;

use clap::Parser;
use clap::error::ErrorKind;
use tracing::warn;
use umbra_render::Cancellation;

use crate::build_identity::{version_line, version_notice};
use crate::cli::{Cli, Command};
use crate::error::Error;

/// How `umbra` exits (arc42 §8.13). Invalid usage exits 2 from `clap`
/// itself.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Exit {
    /// Success.
    Success = 0,
    /// A failure: an I/O error, or a panic.
    Failure = 1,
    /// Refused where a retry cannot help: an invalid Scene File.
    Refused = 3,
    /// Cancelled by Ctrl-C, as a shell reports SIGINT.
    Cancelled = 130,
}

impl From<Exit> for ExitCode {
    fn from(exit: Exit) -> Self {
        Self::from(exit as u8)
    }
}

fn main() -> ExitCode {
    exit_on_panic();
    // Not locked: `tracing` writes to stderr from other threads too.
    let mut stderr = io::stderr();
    let cli = match Cli::try_parse() {
        Ok(cli) => cli,
        // `clap` would write the version to stdout; Umbra writes it to
        // stderr, as the version line at the start of every command.
        Err(error) if error.kind() == ErrorKind::DisplayVersion => {
            let _ = writeln!(stderr, "{}", version_notice());
            return Exit::Success.into();
        }
        Err(error) => error.exit(),
    };
    logs::init(cli.log_format, &cli.log);
    let cancel = cancel_on_ctrl_c();
    // Informative: a closed stderr does not stop the command.
    let _ = writeln!(stderr, "{}", version_line());
    let log_args = cli.log_args();
    let done = match cli.command {
        Command::Render(args) => {
            render::render(&args, &log_args, &cancel, &mut io::stdout(), &mut stderr)
        }
        Command::Worker(args) => worker::work(&args, &cancel),
    };
    let exit = match done {
        Ok(()) => Exit::Success,
        Err(error) => report(&error, &mut stderr),
    };
    exit.into()
}

/// Writes `error` to `stderr`, and how `umbra` exits for it.
fn report(error: &Error, stderr: &mut dyn Write) -> Exit {
    // Nothing is left to tell the operator with if stderr is closed.
    let _ = writeln!(stderr, "{error}");
    match error {
        Error::Refused(_) => Exit::Refused,
        Error::Unreadable(_) | Error::Io(_) | Error::Cluster(_) => Exit::Failure,
        Error::Cancelled => Exit::Cancelled,
    }
}

/// A cancellation that Ctrl-C makes; a second Ctrl-C exits at once, with
/// no report.
fn cancel_on_ctrl_c() -> Arc<Cancellation> {
    let cancel = Arc::new(Cancellation::new());
    let handler = Arc::clone(&cancel);
    let installed = ctrlc::set_handler(move || {
        if handler.is_cancelled() {
            process::exit(Exit::Cancelled as i32);
        }
        handler.cancel();
    });
    if let Err(error) = installed {
        warn!("Ctrl-C will not cancel cleanly: {error}");
    }
    cancel
}

/// Makes a panic on any thread exit 1 once reported, as a failure (arc42
/// §8.13), whether the profile unwinds or aborts.
fn exit_on_panic() {
    let report = panic::take_hook();
    panic::set_hook(Box::new(move |info| {
        report(info);
        process::exit(Exit::Failure as i32);
    }));
}
