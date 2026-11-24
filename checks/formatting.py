# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
"""Formats the sources: clang-format for C++, pyink and isort for Python.

Formats the given files in place, or every tracked C++ and Python file when
none is given. With --check it changes no file and fails when formatting
would change one (``make checks`` and CI). The ``check_*`` functions check
content without changing any file: the pre-commit hook checks the staged
changes, and the linter checks the Python embedded in the workflows.

Usage: python3 -m checks.formatting [--check] [PATH...]
"""

from collections.abc import Sequence
import shutil
import sys

from checks import git
from checks import report
from checks import sources

TOOLS = ("clang-format", "git-clang-format", "pyink", "isort")
# The C++ suffixes as git-clang-format matches them: by the last extension,
# so ".cc.in" becomes "in". Without the list it also formats its defaults,
# such as JSON, which the C++-only .clang-format cannot style.
CLANG_FORMAT_EXTENSIONS = ",".join(
    sorted({suffix.rsplit(".", 1)[-1] for suffix in sources.CPP_SUFFIXES})
)
# What git-clang-format prints when the staged changes are formatted.
CLEAN_OUTPUTS = (
    "",
    "no modified files to format",
    "clang-format did not modify any files",
)


def missing_tools() -> list[str]:
    """Returns the formatting tools that are not on PATH."""
    return [tool for tool in TOOLS if shutil.which(tool) is None]


def format_files(paths: Sequence[str]) -> int:
    """Formats C++ and Python files in place.

    Args:
        paths: The files, relative to the repository root; other kinds of
            files are skipped.

    Returns:
        The process exit status: 0 when every formatter succeeded.
    """
    status = 0
    commands = []
    cpp = [path for path in paths if sources.is_cpp(path)]
    if cpp:
        commands = [["clang-format", "-i", *cpp]]
    python = [path for path in paths if sources.is_python(path)]
    if python:
        commands += [
            ["isort", "--quiet", *python],
            ["pyink", "--quiet", *python],
        ]
    for command in commands:
        status = git.call(command).returncode or status
    return status


def check_files(paths: Sequence[str]) -> int:
    """Checks that C++ and Python files are formatted, without changing them.

    Args:
        paths: The files, relative to the repository root; other kinds of
            files are skipped.

    Returns:
        The process exit status: 0 when no formatter would change a file.
    """
    checks = []
    cpp = [path for path in paths if sources.is_cpp(path)]
    if cpp:
        checks = [("C++", ["clang-format", "--dry-run", "-Werror", *cpp])]
    python = [path for path in paths if sources.is_python(path)]
    if python:
        checks += [
            ("Python imports", ["isort", "--check-only", "--diff", *python]),
            ("Python", ["pyink", "--check", "--diff", "--quiet", *python]),
        ]
    errors = [
        f"{kind} sources are not formatted (run make format)"
        for kind, command in checks
        if git.call(command).returncode
    ]
    return report.fail("sources are not formatted", errors)


def check_python(
    path: str,
    text: str,
    remedy: str = "run make format",
    label: str | None = None,
) -> list[str]:
    """Checks that Python source is formatted, without changing any file.

    Args:
        path: An existing file, relative to the repository root; it selects
            the configuration (isort skips files that do not exist).
        text: The content to check.
        remedy: How to fix the content, appended to each error.
        label: What the messages call the content; ``path`` when None.

    Returns:
        The human-readable errors; empty when the content is formatted.
    """
    checks = (
        (
            "imports not sorted",
            ["isort", "--check-only", "--diff", "--filename", path, "-"],
        ),
        (
            "not formatted",
            [
                "pyink",
                "--check",
                "--diff",
                "--quiet",
                "--stdin-filename",
                path,
                "-",
            ],
        ),
    )
    return [
        f"{label or path}: {problem} ({remedy})"
        for problem, command in checks
        if git.call(command, stdin=text).returncode
    ]


def check_staged_cpp() -> int:
    """Checks the formatting of the staged C++ changes.

    Returns:
        The process exit status: 0 when git-clang-format changes nothing.
    """
    result = git.call(
        [
            "git",
            "clang-format",
            "--staged",
            "--diff",
            "--quiet",
            "--extensions",
            CLANG_FORMAT_EXTENSIONS,
        ],
        capture=True,
    )
    output = result.stdout.strip()
    if output in CLEAN_OUTPUTS and not result.returncode:
        return 0
    print(output, file=sys.stderr)
    return report.fail(
        "staged C++ changes are not formatted",
        ["run make format (or git clang-format) and stage the result"],
    )


def check_staged_python(paths: Sequence[str]) -> int:
    """Checks the formatting of the staged content of Python files.

    Args:
        paths: The staged files; other kinds of files are skipped.

    Returns:
        The process exit status: 0 when every Python file is formatted.
    """
    errors = [
        error
        for path in paths
        if sources.is_python(path)
        for error in check_python(path, sources.read_staged(path))
    ]
    return report.fail("staged Python files are not formatted", errors)


def main(argv: Sequence[str] | None = None) -> int:
    """Formats, or with --check checks, the given files or every tracked file.

    Args:
        argv: The arguments without the program name; ``sys.argv[1:]`` when
            None.

    Returns:
        The process exit status: 0 when every formatter succeeded, or with
        --check when every file is formatted.
    """
    paths = list(sys.argv[1:] if argv is None else argv)
    check = "--check" in paths
    paths = [path for path in paths if path != "--check"] or sources.tracked()
    return check_files(paths) if check else format_files(paths)


if __name__ == "__main__":
    sys.exit(main())
