//! The `umbra` command: everything it does is in the library, `umbra::run`.

use std::process::ExitCode;

fn main() -> ExitCode {
    umbra::run()
}
