# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
"""Lints the repository: clang-tidy, the clang-query rules, pylint, actionlint.

clang-tidy (through LLVM's run-clang-tidy, in parallel) and the clang-query
rules analyse the translation units of the compilation database: all of them,
or only those the given files affect. pylint lints the Python files and
actionlint (with shellcheck) the workflows, all of them or those given. The
Python embedded in the workflows (see ``checks.embedded``) is checked for
formatting and linted with pylint too.

The compilation database is this module's: it comes from the debug preset,
which ``run`` configures (``cmake --preset debug``) so the database is
current. Another build tree given with -p must already have one.

Usage: python3 -m checks.lint [-p BUILD_DIR] [PATH...]
"""

import argparse
from collections.abc import Sequence
import os
import pathlib
import re
import shutil
import sys

from checks import affected
from checks import clang_query
from checks import embedded
from checks import formatting
from checks import git
from checks import report
from checks import sources

TOOLS = (
    "cmake",
    "run-clang-tidy",
    "clang-tidy",
    "clang-query",
    "pylint",
    "actionlint",
)


def missing_tools() -> list[str]:
    """Returns the lint tools that are not on PATH."""
    return [tool for tool in TOOLS if shutil.which(tool) is None]


def ensure_compile_database(build_dir: pathlib.Path) -> int:
    """Makes the compilation database of a build tree current.

    Configures the debug preset when the build tree is its own; any other
    build tree must already hold compile_commands.json.

    Args:
        build_dir: The build tree holding compile_commands.json.

    Returns:
        The process exit status: 0 when the database is ready.
    """
    if build_dir.resolve() == affected.DEFAULT_BUILD_DIR:
        status = git.call(["cmake", "--preset", "debug"], quiet=True).returncode
        return report.exit_status(
            "cannot configure the debug preset", "cmake --preset debug", status
        )
    database = build_dir / "compile_commands.json"
    return report.fail(
        "cannot lint the C++ sources",
        [] if database.is_file() else [f"{database} does not exist"],
    )


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
    command = [
        "run-clang-tidy",
        "-quiet",
        "-p",
        str(build_dir),
        "-j",
        str(os.cpu_count() or 1),
        *patterns,
    ]
    status = git.call(command).returncode
    return report.exit_status(
        "clang-tidy found problems", "run-clang-tidy", status
    )


def pylint(paths: Sequence[str]) -> int:
    """Runs pylint (configured by .pylintrc) over Python files."""
    status = git.call(["pylint", "--score=n", *paths]).returncode
    return report.exit_status("pylint found problems", "pylint", status)


def embedded_python(scripts: Sequence[embedded.Script]) -> int:
    """Checks the formatting of embedded Python scripts and runs pylint.

    pylint reads each script from standard input under its workflow's name,
    so its messages carry the workflow's path and line numbers.

    Args:
        scripts: The scripts.

    Returns:
        The process exit status: 0 when every script passes.
    """
    errors = [
        error
        for script in scripts
        for error in formatting.check_python(
            script.path,
            script.code,
            "format it as pyink and isort would",
            script.name,
        )
    ]
    status = report.fail("embedded Python is not formatted", errors)
    for script in scripts:
        result = git.call(
            ["pylint", "--score=n", "--from-stdin", script.path],
            stdin=script.aligned(),
        )
        status = (
            report.exit_status(
                f"pylint found problems in {script.name}",
                "pylint",
                result.returncode,
            )
            or status
        )
    return status


def actionlint(paths: Sequence[str]) -> int:
    """Runs actionlint (and shellcheck through it) over workflows."""
    status = git.call(["actionlint", *paths]).returncode
    return report.exit_status("actionlint found problems", "actionlint", status)


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
    if ensure_compile_database(build_dir):
        return 1
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
        scripts = embedded.scripts_of(workflows)
        if scripts:
            status = embedded_python(scripts) or status
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
