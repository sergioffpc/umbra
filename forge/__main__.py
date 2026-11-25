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
    audit_preset = argparse.ArgumentParser(add_help=False)
    audit_preset.add_argument(
        "--preset",
        choices=[preset.value for preset in commands.AUDIT_PRESETS],
        default=commands.Preset.DEBUG.value,
        help="the build tree, build/PRESET: debug (the default), release, asan"
        " or tsan",
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
    check = argparse.ArgumentParser(add_help=False)
    check.add_argument(
        "--check",
        action="store_true",
        help="change no file: fail when one is not formatted",
    )
    ref = argparse.ArgumentParser(add_help=False)
    ref.add_argument(
        "--ref",
        required=True,
        metavar="IMAGE",
        help="the deps image that the current inputs hash to",
    )
    branch = argparse.ArgumentParser(add_help=False)
    branch.add_argument(
        "--branch",
        metavar="NAME",
        help="the release or hotfix branch; by default, that of the GitHub"
        " Actions run",
    )
    image = argparse.ArgumentParser(add_help=False)
    image.add_argument(
        "image", metavar="IMAGE", help="the image, loaded in docker"
    )
    tag = argparse.ArgumentParser(add_help=False)
    tag.add_argument(
        "--tag",
        metavar="NAME",
        help="the release tag, vX.Y.Z; by default, the tag of the GitHub"
        " Actions run",
    )
    output = argparse.ArgumentParser(add_help=False)
    output.add_argument(
        "--output", required=True, metavar="FILE", help="the file to write"
    )
    signed = argparse.ArgumentParser(add_help=False)
    signed.add_argument(
        "image",
        metavar="IMAGE@DIGEST",
        help="the image in its registry, by digest",
    )
    return {
        commands.Option.PRESET: preset,
        commands.Option.AUDIT_PRESET: audit_preset,
        commands.Option.LABEL: label,
        commands.Option.VERBOSE: verbose,
        commands.Option.CHECK: check,
        commands.Option.REF: ref,
        commands.Option.BRANCH: branch,
        commands.Option.IMAGE: image,
        commands.Option.TAG: tag,
        commands.Option.OUTPUT: output,
        commands.Option.SIGNED: signed,
    }


def parser() -> argparse.ArgumentParser:
    """Returns the parser of the forge command line."""
    root = argparse.ArgumentParser(
        prog="python3 -m forge",
        description=(
            "Builds, tests and checks Umbra through the CMake presets and"
            " the shared checks, locally and in CI."
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
        check=getattr(args, "check", False),
        ref=getattr(args, "ref", ""),
        branch=getattr(args, "branch", None),
        image=getattr(args, "image", ""),
        tag=getattr(args, "tag", None),
        output=getattr(args, "output", ""),
    )
    return commands.execute(args.command.steps(options))


if __name__ == "__main__":
    sys.exit(main())
