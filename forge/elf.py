# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
"""The ELF files under some paths, and the libraries they need.

The helpers of the ELF rules ``no_gcc_runtime`` and ``no_libcuda``. An ELF
file is a regular file, not a symbolic link, that starts with the ELF magic
number. Its NEEDED entries are read with llvm-readelf, and the libraries it
loads, through its dependencies too, with ldd.
"""

from collections.abc import Iterator, Sequence
import os
import subprocess
import sys

from forge import github

MAGIC = b"\x7fELF"


def files(paths: Sequence[str]) -> Iterator[str]:
    """Yields the ELF files under the given files or directories.

    A path that does not exist is reported on stderr and skipped; symbolic
    links are not followed.

    Args:
        paths: Files or directories.
    """
    for top in paths:
        if not os.path.lexists(top):
            print(f"forge: {top}: No such file or directory", file=sys.stderr)
            continue
        if os.path.islink(top):
            continue
        if not os.path.isdir(top):
            if is_elf(top):
                yield top
            continue
        for directory, _, names in os.walk(top):
            for name in sorted(names):
                path = os.path.join(directory, name)
                if not os.path.islink(path) and is_elf(path):
                    yield path


def is_elf(path: str) -> bool:
    """Returns whether a regular file starts with the ELF magic number."""
    if not os.path.isfile(path):
        return False
    try:
        with open(path, "rb") as file:
            return file.read(len(MAGIC)) == MAGIC
    except OSError:
        return False


def run(args: Sequence[str]) -> subprocess.CompletedProcess[str]:
    """Runs a tool, capturing its standard output and discarding its errors.

    Raises:
        StepError: The tool cannot start.
    """
    try:
        return subprocess.run(
            args,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            errors="replace",
            check=False,
        )
    except OSError as error:
        raise github.StepError(f"{args[0]}: {error.strerror}") from error


def needed(path: str) -> list[str]:
    """Returns the NEEDED lines of an ELF file's dynamic section.

    A file without a dynamic section has none.

    Raises:
        StepError: llvm-readelf cannot start.
    """
    output = run(["llvm-readelf", "--dynamic", path]).stdout
    return [line for line in output.splitlines() if "(NEEDED)" in line]


def loaded(path: str) -> list[str] | None:
    """Returns the lines of ldd for an ELF file.

    Returns:
        The libraries that the file loads, one line each, or None when ldd
        fails, as on a file that is not dynamically linked (an object, a
        static program).

    Raises:
        StepError: ldd cannot start.
    """
    result = run(["ldd", path])
    return None if result.returncode else result.stdout.splitlines()


def joined(lines: Sequence[str]) -> str:
    """Returns lines as one, their words separated by single spaces."""
    return " ".join(" ".join(lines).split())
