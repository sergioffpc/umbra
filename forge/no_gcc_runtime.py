# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
"""No ELF file may need a GCC runtime: umbra uses the LLVM runtimes only.

Fails when an ELF file under the given files or directories needs a GCC
runtime (libstdc++, libgcc_s or GCC's libatomic), directly (NEEDED) or
through its dependencies (ldd), or has a dependency that does not resolve,
and when there is no ELF file at all (a wrong path). Needs llvm-readelf and
ldd. ``python3 -m forge audit`` checks the build trees with it, the deps
workflow the toolchain and deps images, and verify-toolchain its test
programs.

Usage: python3 -m forge no-gcc-runtime PATH...
"""

import argparse
from collections.abc import Sequence
import re

from forge import elf
from forge import github

# compiler-rt's own atomics library is libclang_rt.atomic.so, which this does
# not match.
GCC_RUNTIME = re.compile(
    r"libstdc\+\+\.so|libgcc_s\.so|(^|[^_.0-9A-Za-z])libatomic\.so"
)
UNRESOLVED = "not found"


def file_errors(path: str) -> list[str]:
    """Returns the errors of one ELF file.

    Raises:
        StepError: llvm-readelf or ldd cannot start.
    """
    errors = []
    if any(GCC_RUNTIME.search(line) for line in elf.needed(path)):
        errors.append(f"{path} needs a GCC runtime (NEEDED)")
    libraries = elf.loaded(path)
    if libraries is None:
        return errors
    runtimes = [line for line in libraries if GCC_RUNTIME.search(line)]
    if runtimes:
        errors.append(f"{path} loads a GCC runtime: {elf.joined(runtimes)}")
    unresolved = [line for line in libraries if UNRESOLVED in line]
    if unresolved:
        errors.append(
            f"{path} has unresolved libraries: {elf.joined(unresolved)}"
        )
    return errors


def check(paths: Sequence[str]) -> int:
    """Checks the ELF files under the given paths, reporting each error.

    Args:
        paths: Files or directories.

    Returns:
        0 when no file needs a GCC runtime, every dependency resolves and
        there is an ELF file; 1 otherwise.

    Raises:
        StepError: llvm-readelf or ldd cannot start.
    """
    status = 0
    count = 0
    for path in elf.files(paths):
        count += 1
        for message in file_errors(path):
            github.error(message)
            status = 1
    shown = " ".join(paths)
    if not count:
        github.error(f"no ELF file under {shown}")
        status = 1
    print(f"Checked {count} ELF files under {shown}.", flush=True)
    return status


def main(argv: Sequence[str] | None = None) -> int:
    """Checks that no ELF file under the given paths needs a GCC runtime.

    Args:
        argv: The arguments without the program name: the paths.

    Returns:
        The process exit status: 0 when the check passes, 1 otherwise.
    """
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "paths", nargs="+", metavar="PATH", help="a file or directory"
    )
    return check(parser.parse_args(argv).paths)
