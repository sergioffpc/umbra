# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
"""The forge command line.

Every step is echoed to stderr before it runs, and the command stops at the
first step that fails, with its exit status.

Usage: python3 -m forge COMMAND [OPTION...]
"""

# Python fixes the name of a package's entry point, which is not a valid
# module name.
# pylint: disable=invalid-name

import argparse
from collections.abc import Sequence
import sys

from forge import commands


def option_parsers() -> dict[commands.Option, argparse.ArgumentParser]:
    """Returns a parent parser for each option a command may take."""
    preset = argparse.ArgumentParser(add_help=False)
    preset.add_argument(
        "--preset",
        choices=[preset.value for preset in commands.BUILD_PRESETS],
        default=commands.Preset.DEBUG.value,
        help="the CMake preset: debug (the default) or release (LTO)",
    )
    label = argparse.ArgumentParser(add_help=False)
    label.add_argument(
        "--label",
        metavar="REGEX",
        help="run only the tests with a matching CTest label, e.g. gpu",
    )
    verbose = argparse.ArgumentParser(add_help=False)
    verbose.add_argument(
        "-v",
        "--verbose",
        action="store_true",
        help=(
            "print as much as each tool can: the configure's messages down"
            " to TRACE, every command the build runs, and the full output of"
            " every test"
        ),
    )
    return {
        commands.Option.PRESET: preset,
        commands.Option.LABEL: label,
        commands.Option.VERBOSE: verbose,
    }


def parser() -> argparse.ArgumentParser:
    """Returns the parser of the forge command line."""
    root = argparse.ArgumentParser(
        prog="python3 -m forge",
        description=(
            "Builds, tests and checks Umbra through the CMake presets and"
            " the shared checks."
        ),
    )
    subcommands = root.add_subparsers(
        dest="name", metavar="COMMAND", required=True
    )
    parents = option_parsers()
    for command in commands.COMMANDS:
        subcommands.add_parser(
            command.name,
            help=command.summary,
            description=command.summary,
            parents=[parents[option] for option in command.options],
        ).set_defaults(command=command)
    return root


def main(argv: Sequence[str] | None = None) -> int:
    """Runs the forge command given on the command line.

    Args:
        argv: The command and its options; ``sys.argv[1:]`` when None.

    Returns:
        The process exit status: 0 when every step passed, otherwise the
        status of the step that failed.
    """
    args = parser().parse_args(argv)
    options = commands.Options(
        preset=commands.Preset(getattr(args, "preset", commands.Preset.DEBUG)),
        label=getattr(args, "label", None),
        verbose=getattr(args, "verbose", False),
    )
    return commands.execute(args.command.steps(options))


if __name__ == "__main__":
    sys.exit(main())
