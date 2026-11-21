# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
"""Formats the sources: clang-format for C++, pyink and isort for Python.

Formats the given files in place, or every tracked C++ and Python file when
none is given. ``check_python`` checks a Python file's content without
changing it (the pre-commit hook passes the staged content).

Usage: python3 -m checks.formatting [PATH...]
"""

from collections.abc import Sequence
import subprocess
import sys

from checks import git
from checks import sources


def call(args: Sequence[str], stdin: str | None = None) -> int:
    """Runs a command from the repository root and returns its exit status."""
    return subprocess.run(
        args, cwd=git.REPO_ROOT, input=stdin, text=True, check=False
    ).returncode


def format_files(paths: Sequence[str]) -> int:
    """Formats C++ and Python files in place.

    Args:
        paths: The files, relative to the repository root; other kinds of
            files are skipped.

    Returns:
        The process exit status: 0 when every formatter succeeded.
    """
    status = 0
    cpp = [path for path in paths if sources.is_cpp(path)]
    if cpp:
        status = call(["clang-format", "-i", *cpp]) or status
    python = [path for path in paths if sources.is_python(path)]
    if python:
        status = call(["isort", "--quiet", *python]) or status
        status = call(["pyink", "--quiet", *python]) or status
    return status


def check_python(path: str, text: str) -> list[str]:
    """Checks that Python source is formatted, without changing any file.

    Args:
        path: The file, relative to the repository root; it selects the
            configuration and appears in the messages.
        text: The content to check.

    Returns:
        The human-readable errors; empty when the content is formatted.
    """
    errors = []
    if call(["isort", "--check-only", "--diff", "--filename", path, "-"], text):
        errors.append(f"{path}: imports not sorted (run make format)")
    if call(
        ["pyink", "--check", "--diff", "--stdin-filename", path, "-"], text
    ):
        errors.append(f"{path}: not formatted (run make format)")
    return errors


def main(argv: Sequence[str] | None = None) -> int:
    """Formats the files given on the command line, or every tracked file.

    Args:
        argv: The file paths without the program name; ``sys.argv[1:]`` when
            None.

    Returns:
        The process exit status: 0 when every formatter succeeded.
    """
    paths = list(sys.argv[1:] if argv is None else argv)
    return format_files(paths or sources.tracked())


if __name__ == "__main__":
    sys.exit(main())
