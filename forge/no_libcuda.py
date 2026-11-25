# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
"""No ELF file may need the CUDA driver, but the modules loaded at run time.

Fails when an ELF file under the given files or directories has the CUDA
driver, libcuda.so.1, among its NEEDED entries, unless it is a module loaded
at run time (RUNTIME_MODULES): umbra and its dependencies run on hosts
without an NVIDIA driver, and only those modules, loaded with dlopen when a
usable GPU exists, may need it. Also fails when there is no ELF file at all
(a wrong path). Needs llvm-readelf. ``python3 -m forge audit`` checks the
build trees with it, and the deps workflow the deps image.

Usage: python3 -m forge no-libcuda PATH...
"""

import argparse
from collections.abc import Sequence
import fnmatch
import os
import re

from forge import elf
from forge import github

# File name patterns of the modules loaded at run time: umbra's GPU module
# and OIDN's GPU devices.
RUNTIME_MODULES = (
    "libumbra-gpu.so",
    "libOpenImageDenoise_device_*.so*",
)
LIBCUDA = re.compile(r"\[libcuda\.so(\.[0-9]+)*\]")


def is_runtime_module(path: str) -> bool:
    """Returns whether a file is a module loaded at run time, by its name."""
    name = os.path.basename(path)
    return any(
        fnmatch.fnmatchcase(name, pattern) for pattern in RUNTIME_MODULES
    )


def check(paths: Sequence[str]) -> int:
    """Checks the ELF files under the given paths, reporting each error.

    Args:
        paths: Files or directories.

    Returns:
        0 when only modules loaded at run time need libcuda.so.1 and there
        is an ELF file; 1 otherwise.

    Raises:
        StepError: llvm-readelf cannot start.
    """
    status = 0
    count = 0
    for path in elf.files(paths):
        count += 1
        if not any(LIBCUDA.search(line) for line in elf.needed(path)):
            continue
        if is_runtime_module(path):
            print(f"{path} needs libcuda.so.1 (a module loaded at run time)")
        else:
            github.error(
                f"{path} needs libcuda.so.1; only modules loaded at run time"
                " may"
            )
            status = 1
    shown = " ".join(paths)
    if not count:
        github.error(f"no ELF file under {shown}")
        status = 1
    print(f"Checked {count} ELF files under {shown} for libcuda.so.1.")
    return status


def main(argv: Sequence[str] | None = None) -> int:
    """Checks that no ELF file under the given paths links the CUDA driver.

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
