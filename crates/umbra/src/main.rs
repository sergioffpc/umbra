#![doc = include_str!("../README.md")]

mod build_identity;
mod cli;
mod error;
mod render;

use std::io::{self, Write};
use std::panic;
use std::process::ExitCode;

use clap::Parser;

use crate::cli::{Cli, Command};
use crate::error::Error;

/// How `umbra` exits (arc42 §8.13). Invalid usage exits 2 from `clap`
/// itself; Ctrl-C's 130 arrives with
/// [#86](https://github.com/sergioffpc/umbra/issues/86).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Exit {
    /// Success.
    Success = 0,
    /// A failure: an I/O error, or a panic.
    Failure = 1,
    /// Refused where a retry cannot help: an invalid Scene File.
    Refused = 3,
}

impl From<Exit> for ExitCode {
    fn from(exit: Exit) -> Self {
        Self::from(exit as u8)
    }
}

fn main() -> ExitCode {
    exit_on_panic();
    let cli = Cli::parse();
    let mut stderr = io::stderr().lock();
    let exit = match cli.command {
        Command::Render(args) => match render::render(&args, &mut stderr) {
            Ok(()) => Exit::Success,
            Err(error) => report(&error, &mut stderr),
        },
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
    }
}

/// Makes a panic on any thread exit 1 once reported, as a failure (arc42
/// §8.13), whether the profile unwinds or aborts.
fn exit_on_panic() {
    let report = panic::take_hook();
    panic::set_hook(Box::new(move |info| {
        report(info);
        std::process::exit(Exit::Failure as i32);
    }));
}
