# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
"""Lints the repository: clang-tidy, the clang-query rules, pylint, actionlint.

clang-tidy (through LLVM's run-clang-tidy, in parallel) and the clang-query
rules analyse the translation units of the compilation database: all of them,
or only those the given files affect. pylint lints the Python files and
actionlint (with shellcheck) the workflows, all of them or those given.

The compilation database comes from the debug preset (``cmake --preset
debug``).

Usage: python3 -m checks.lint [-p BUILD_DIR] [PATH...]
"""

import argparse
from collections.abc import Sequence
import os
import pathlib
import re
import shutil
import subprocess
import sys

from checks import affected
from checks import clang_query
from checks import git
from checks import report
from checks import sources

TOOLS = ("run-clang-tidy", "clang-tidy", "clang-query", "pylint", "actionlint")


def missing_tools() -> list[str]:
    """Returns the lint tools that are not on PATH."""
    return [tool for tool in TOOLS if shutil.which(tool) is None]


def call(args: Sequence[str]) -> int:
    """Runs a command from the repository root and returns its exit status."""
    return subprocess.run(args, cwd=git.REPO_ROOT, check=False).returncode


def clang_tidy(
    translation_units: Sequence[str], build_dir: pathlib.Path
) -> int:
    """Runs clang-tidy over translation units, in parallel.

    Args:
        translation_units: The sources, relative to the repository root.
        build_dir: The build tree holding compile_commands.json.

    Returns:
        The process exit status: 0 when clang-tidy reports nothing.
    """
    # run-clang-tidy selects the database entries whose path matches.
    patterns = [
        f"^{re.escape(str(git.REPO_ROOT / path))}$"
        for path in translation_units
    ]
    status = call(
        [
            "run-clang-tidy",
            "-quiet",
            "-p",
            str(build_dir),
            "-j",
            str(os.cpu_count() or 1),
            *patterns,
        ]
    )
    return report.fail(
        "clang-tidy found problems",
        [f"run-clang-tidy exited with status {status}"] if status else [],
    )


def pylint(paths: Sequence[str]) -> int:
    """Runs pylint (configured by .pylintrc) over Python files."""
    status = call(["pylint", "--score=n", *paths])
    return report.fail(
        "pylint found problems",
        [f"pylint exited with status {status}"] if status else [],
    )


def actionlint(paths: Sequence[str]) -> int:
    """Runs actionlint (and shellcheck through it) over workflows."""
    status = call(["actionlint", *paths])
    return report.fail(
        "actionlint found problems",
        [f"actionlint exited with status {status}"] if status else [],
    )


def run(paths: Sequence[str] | None, build_dir: pathlib.Path) -> int:
    """Lints the given files, or the whole repository.

    Args:
        paths: The changed files, relative to the repository root; None for
            every file.
        build_dir: The build tree holding compile_commands.json.

    Returns:
        The process exit status: 0 when every linter passes, 1 otherwise.
    """
    missing = missing_tools()
    if missing:
        return report.fail(
            "cannot lint",
            [
                "not found on PATH: " + ", ".join(missing),
                "lint in the devcontainer, the only supported environment",
            ],
        )
    if paths is None:
        files = sources.tracked()
        units = affected.database_sources(build_dir)
    else:
        files = list(paths)
        units = affected.translation_units(files, build_dir)
    status = 0
    if units:
        status = clang_tidy(units, build_dir) or status
        status = clang_query.run(units, build_dir) or status
    python = [path for path in files if sources.is_python(path)]
    if python:
        status = pylint(python) or status
    workflows = [path for path in files if sources.is_workflow(path)]
    if workflows:
        status = actionlint(workflows) or status
    return status


def main(argv: Sequence[str] | None = None) -> int:
    """Lints the files given on the command line, or the whole repository.

    Args:
        argv: The arguments without the program name; ``sys.argv[1:]`` when
            None.

    Returns:
        The process exit status: 0 when every linter passes, 1 otherwise.
    """
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "-p",
        dest="build_dir",
        type=pathlib.Path,
        default=affected.DEFAULT_BUILD_DIR,
        help="build tree with compile_commands.json (default: build/debug)",
    )
    parser.add_argument(
        "paths",
        nargs="*",
        metavar="PATH",
        help="changed file relative to the repository root (default: all)",
    )
    args = parser.parse_args(argv)
    return run(args.paths or None, args.build_dir)


if __name__ == "__main__":
    sys.exit(main())
