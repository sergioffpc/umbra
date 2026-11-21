# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
"""The clang-query rules of checks/queries/ over translation units.

Each rule file checks/queries/umbra-<rule>.query binds its matches to the
rule's name, umbra-<rule>. A match in umbra's own code is an error unless the
line carries ``// NOLINT(umbra-<rule>): <reason>`` or the line before it
``// NOLINTNEXTLINE(umbra-<rule>): <reason>``; a suppression without a reason
does not count.

Usage: python3 -m checks.clang_query [-p BUILD_DIR] TU...
"""

import argparse
from collections.abc import Sequence
import concurrent.futures
import os
import pathlib
import re
import sys

from checks import git
from checks import nolint
from checks import report

RULES_DIR = git.REPO_ROOT / "checks" / "queries"
DEFAULT_BUILD_DIR = git.REPO_ROOT / "build" / "debug"
MATCH = re.compile(
    r"^(?P<file>.+?):(?P<line>\d+):(?P<column>\d+): "
    r'note: "(?P<rule>umbra-[\w-]+)" binds here$',
    re.MULTILINE,
)
MESSAGES = {
    "umbra-bool-parameter": "bool (or bool flag) parameter; use an enum class "
    "with an explicit underlying type",
    "umbra-bool-field": "bool (or bool flag) data member; use an enum class "
    "with an explicit underlying type",
    "umbra-predicate-name": "a function returning bool is a predicate named "
    "Is, Has, Can, Should, Contains or Equals...",
}
# Code that is not umbra's own: the build trees and the dependencies.
FOREIGN_DIRS = ("build/", "third_party/")


def rule_files() -> list[pathlib.Path]:
    """Returns the rule files, in a stable order."""
    return sorted(RULES_DIR.glob("umbra-*.query"))


def query(translation_unit: str, build_dir: pathlib.Path) -> tuple[str, str]:
    """Runs every rule over one translation unit.

    Args:
        translation_unit: The source file, relative to the repository root.
        build_dir: The build tree holding compile_commands.json.

    Returns:
        clang-query's standard output, and an error message when it failed
        (empty otherwise).
    """
    files = [arg for path in rule_files() for arg in ("-f", str(path))]
    result = git.call(
        ["clang-query", "-p", str(build_dir), *files, translation_unit],
        capture=True,
    )
    error = ""
    if result.returncode:
        error = (
            f"{translation_unit}: clang-query exited with status "
            f"{result.returncode}: {result.stderr.strip()}"
        )
    return result.stdout, error


def own_path(file: str) -> str | None:
    """Returns a matched file relative to the repository, if it is umbra's.

    Args:
        file: The file as clang-query printed it; headers may be reached
            through the build tree's links to include/.

    Returns:
        The file relative to the repository root, or None when it is outside
        the repository, in a build tree or in a dependency.
    """
    path = pathlib.Path(git.REPO_ROOT, file).resolve()
    if not path.is_relative_to(git.REPO_ROOT):
        return None
    relative = path.relative_to(git.REPO_ROOT).as_posix()
    return None if relative.startswith(FOREIGN_DIRS) else relative


def is_suppressed(path: str, line: int, rule: str) -> bool:
    """Returns whether a NOLINT with a reason suppresses a rule on a line."""
    lines = (git.REPO_ROOT / path).read_text(encoding="utf-8").splitlines()
    current = lines[line - 1] if line <= len(lines) else ""
    previous = lines[line - 2] if 2 <= line <= len(lines) + 1 else ""
    return nolint.suppresses(current, previous, rule)


def violations(output: str) -> set[str]:
    """Returns the unsuppressed matches in umbra's code of clang-query output.

    Args:
        output: clang-query's standard output.

    Returns:
        One ``path:line:column: message [rule]`` error per match.
    """
    errors = set()
    for match in MATCH.finditer(output):
        path = own_path(match.group("file"))
        line = int(match.group("line"))
        rule = match.group("rule")
        column = match.group("column")
        if path is None or is_suppressed(path, line, rule):
            continue
        message = MESSAGES.get(rule, "matched")
        errors.add(f"{path}:{line}:{column}: {message} [{rule}]")
    return errors


def run(translation_units: Sequence[str], build_dir: pathlib.Path) -> int:
    """Runs the rules over translation units, in parallel.

    Args:
        translation_units: The source files, relative to the repository root.
        build_dir: The build tree holding compile_commands.json.

    Returns:
        The process exit status: 0 when no rule matches, 1 otherwise.
    """
    errors: set[str] = set()
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count()) as pool:
        for output, error in pool.map(
            lambda tu: query(tu, build_dir), translation_units
        ):
            errors |= violations(output)
            if error:
                errors.add(error)
    return report.fail("clang-query rules violated", sorted(errors))


def main(argv: Sequence[str] | None = None) -> int:
    """Runs the rules over the translation units given on the command line.

    Args:
        argv: The arguments without the program name; ``sys.argv[1:]`` when
            None.

    Returns:
        The process exit status: 0 when no rule matches, 1 otherwise.
    """
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "-p",
        dest="build_dir",
        type=pathlib.Path,
        default=DEFAULT_BUILD_DIR,
        help="build tree with compile_commands.json (default: build/debug)",
    )
    parser.add_argument(
        "translation_units", nargs="+", metavar="TU", help="source file"
    )
    args = parser.parse_args(argv)
    return run(args.translation_units, args.build_dir)


if __name__ == "__main__":
    sys.exit(main())
