# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
"""The repository's files: which ones there are and what kind each one is.

Paths are POSIX paths relative to the repository root, as git lists them.
Submodules are never included.
"""

import argparse
from collections.abc import Callable, Iterable, Sequence
import pathlib

from checks import git
from checks import report

# .cu is CUDA device code (CUDA kernels and OptiX programs), compiled by nvcc.
DEVICE_SUFFIXES = (".cu",)
CPP_SUFFIXES = (".h", ".cc", ".h.in", ".cc.in") + DEVICE_SUFFIXES
HEADER_SUFFIXES = (".h", ".h.in")
# Files that carry an SPDX header: code, build and tool configuration.
SOURCE_SUFFIXES = CPP_SUFFIXES + (
    ".py",
    ".cmake",
    ".sh",
    ".toml",
    ".query",
    ".Dockerfile",
)
SOURCE_NAMES = (
    "CMakeLists.txt",
    ".clang-format",
    ".clang-tidy",
    ".clangd",
)
SOURCE_DIRS = (".githooks/", ".github/workflows/")
# Every file in .githooks/ is a Python 3 script without an extension.
HOOKS_DIR = ".githooks/"
WORKFLOWS_DIR = ".github/workflows/"

# Reads the content of a file given its path.
Reader = Callable[[str], str]


def tracked() -> list[str]:
    """Returns the tracked files of the working tree."""
    paths = git.run("ls-files", "-z").split("\0")
    return [path for path in paths if (git.REPO_ROOT / path).is_file()]


def staged() -> list[str]:
    """Returns the files added, copied, modified or renamed in the index."""
    output = git.run(
        "diff",
        "--cached",
        "--name-only",
        "-z",
        "--diff-filter=ACMR",
        "--ignore-submodules=all",
    )
    return [path for path in output.split("\0") if path]


def read_working_tree(path: str) -> str:
    """Returns the content of a file of the working tree."""
    return (git.REPO_ROOT / path).read_text(encoding="utf-8")


def read_staged(path: str) -> str:
    """Returns the content of a file in the index."""
    return git.run("show", f":{path}")


def is_cpp(path: str) -> bool:
    """Returns whether the file is C++ source (including templates)."""
    return path.endswith(CPP_SUFFIXES)


def is_device_code(path: str) -> bool:
    """Returns whether the file is CUDA device code, compiled by nvcc."""
    return path.endswith(DEVICE_SUFFIXES)


def is_header(path: str) -> bool:
    """Returns whether the file is a C++ header."""
    return path.endswith(HEADER_SUFFIXES)


def is_python(path: str) -> bool:
    """Returns whether the file is a Python script or module."""
    return path.endswith(".py") or path.startswith(HOOKS_DIR)


def is_workflow(path: str) -> bool:
    """Returns whether the file is a GitHub Actions workflow."""
    return path.startswith(WORKFLOWS_DIR) and path.endswith((".yml", ".yaml"))


def is_source(path: str) -> bool:
    """Returns whether the file is source that must carry an SPDX header."""
    return (
        path.endswith(SOURCE_SUFFIXES)
        or pathlib.PurePosixPath(path).name in SOURCE_NAMES
        or path.startswith(SOURCE_DIRS)
    )


def validate_files(
    paths: Iterable[str],
    applies_to: Callable[[str], bool],
    validate: Callable[[str, str], list[str]],
    read: Reader = read_working_tree,
) -> list[str]:
    """Runs a file validator over the files it applies to.

    Args:
        paths: The candidate files.
        applies_to: Returns whether the validator applies to a file.
        validate: The validator: returns the errors of a file given its path
            and content.
        read: Reads the content of a file.

    Returns:
        The errors of every file.
    """
    return [
        error
        for path in paths
        if applies_to(path)
        for error in validate(path, read(path))
    ]


def validator_main(
    subject: str,
    applies_to: Callable[[str], bool],
    validate: Callable[[str, str], list[str]],
    argv: Sequence[str] | None = None,
) -> int:
    """The command-line entry point of a file validator.

    Validates the files given on the command line (all tracked files when
    none is given), or the staged content of the staged files.

    Args:
        subject: What the errors are about, for example ``"missing SPDX
            header"``.
        applies_to: Returns whether the validator applies to a file.
        validate: The validator: returns the errors of a file given its path
            and content.
        argv: The arguments without the program name; ``sys.argv[1:]`` when
            None.

    Returns:
        The process exit status: 0 when every file is valid, 1 otherwise.
    """
    paths, read = parse_file_arguments(argv)
    return report.fail(
        subject, validate_files(paths, applies_to, validate, read)
    )


def parse_file_arguments(
    argv: Sequence[str] | None,
) -> tuple[list[str], Reader]:
    """Parses the ``[--staged] [PATH...]`` arguments of a file validator.

    Args:
        argv: The arguments without the program name; ``sys.argv[1:]`` when
            None.

    Returns:
        The files to validate and the function that reads their content.
    """
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--staged",
        action="store_true",
        help="validate the staged content of the staged files",
    )
    parser.add_argument(
        "paths",
        nargs="*",
        metavar="PATH",
        help="file relative to the repository root (default: all tracked)",
    )
    args = parser.parse_args(argv)
    if args.staged:
        return args.paths or staged(), read_staged
    return args.paths or tracked(), read_working_tree
