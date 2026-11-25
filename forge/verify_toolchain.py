# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
"""The toolchain image has the tools and runtimes the builds need.

Runs inside the toolchain image, from a read-only checkout: the tools of the
built toolchain resolve from /usr/local/bin, the system and Python tools are
on PATH, clang has the pinned version, opt-viewer runs, the compiler-rt and
BOLT runtimes exist, no LLVM package comes from apt (the stage-1 compiler is
discarded), and the default toolchain builds a program, plain and sanitized,
that runs and uses only the LLVM runtimes (see ``no_gcc_runtime``).

Usage: python3 -m forge verify-toolchain
"""

import argparse
from collections.abc import Sequence
import glob
import os
import pathlib
import re
import shutil
import subprocess
import tempfile

from forge import github
from forge import no_gcc_runtime

TOOLCHAIN_BIN = "/usr/local/bin/"
# The tools of the built toolchain, which must resolve from TOOLCHAIN_BIN.
TOOLCHAIN_TOOLS = (
    "clang",
    "clang++",
    "cc",
    "c++",
    "clang-tidy",
    "clang-format",
    "clang-query",
    "clangd",
    "clangd-indexer",
    "clang-apply-replacements",
    "git-clang-format",
    "run-clang-tidy",
    "lld",
    "ld.lld",
    "lldb",
    "lldb-dap",
    "llvm-bolt",
    "perf2bolt",
    "merge-fdata",
    "llvm-mca",
    "llvm-xray",
    "llvm-cov",
    "llvm-profdata",
    "llvm-symbolizer",
    "llvm-ar",
    "llvm-readelf",
    "llvm-objcopy",
    "llvm-config",
    "FileCheck",
    "git-cliff",
    "git-flow",
    "actionlint",
)
# The system and Python tools, from anywhere on PATH.
OTHER_TOOLS = (
    "cmake",
    "ninja",
    "make",
    "python3",
    "git",
    "pylint",
    "pyink",
    "isort",
)
CLANG_VERSION = "23.1.6"
OPT_VIEWER = "/usr/local/share/opt-viewer/opt-viewer.py"
COMPILER_RT = (
    "builtins",
    "atomic",
    "asan",
    "ubsan_standalone",
    "tsan",
    "msan",
    "lsan",
    "cfi",
    "cfi_diag",
    "xray",
    "fuzzer",
    "fuzzer_no_main",
    "profile",
)
BOLT_RUNTIME = "/usr/local/lib/libbolt_rt_instr.a"
APT_LLVM = re.compile(
    r"^(clang|lld|lldb|llvm|libllvm|libclang|libc\+\+|libunwind|libomp"
    r"|python3-lldb)"
)
HELLO = (
    "#include <iostream>\n"
    'int main() { std::cout << "hello\\n"; return 0; }\n'
)


class Verifier:
    """Runs the programs of the checks and collects the failures."""

    def __init__(self) -> None:
        self.status = 0

    def fail(self, message: str) -> None:
        """Reports a failed check."""
        github.error(message)
        self.status = 1

    def run(
        self, args: Sequence[str], cwd: str | None = None
    ) -> subprocess.CompletedProcess[str] | None:
        """Runs a program, capturing its standard output.

        Returns:
            The finished process, or None when the program cannot start.
        """
        try:
            return subprocess.run(
                args, cwd=cwd, stdout=subprocess.PIPE, text=True, check=False
            )
        except OSError:
            return None

    def output(self, args: Sequence[str], cwd: str | None = None) -> str:
        """Returns the standard output of a program; empty when it fails."""
        result = self.run(args, cwd)
        return result.stdout if result and not result.returncode else ""

    def succeeds(self, args: Sequence[str], cwd: str | None = None) -> bool:
        """Returns whether a program runs and exits with status 0."""
        result = self.run(args, cwd)
        return bool(result) and not result.returncode

    def tools(self) -> None:
        """Checks the tools on PATH, clang's version and opt-viewer."""
        for tool in TOOLCHAIN_TOOLS:
            path = shutil.which(tool)
            if path is None:
                self.fail(f"{tool} is not on PATH")
            elif not path.startswith(TOOLCHAIN_BIN):
                self.fail(f"{tool} resolves to {path}")
        for tool in OTHER_TOOLS:
            if shutil.which(tool) is None:
                self.fail(f"{tool} is not on PATH")
        version = f"version {CLANG_VERSION}"
        if version not in self.output(["clang", "--version"]):
            self.fail(f"clang is not {CLANG_VERSION}")
        if not os.path.isfile(OPT_VIEWER):
            self.fail("opt-viewer is missing")
        if not self.succeeds(["python3", OPT_VIEWER, "--help"]):
            self.fail("opt-viewer does not run")

    def runtimes(self) -> None:
        """Checks the compiler-rt runtimes and the BOLT runtime."""
        runtime_dir = self.output(["clang", "-print-runtime-dir"]).strip()
        for lib in COMPILER_RT:
            pattern = f"{glob.escape(runtime_dir)}/libclang_rt.{lib}.*"
            if not glob.glob(pattern):
                self.fail(
                    f"compiler-rt runtime {lib} is missing from {runtime_dir}"
                )
        if not os.path.exists(BOLT_RUNTIME):
            self.fail("the BOLT instrumentation runtime is missing")

    def packages(self) -> None:
        """Checks that no LLVM package comes from apt."""
        listing = self.output(["dpkg-query", "-W", "-f", "${Package}\n"])
        llvm = [name for name in listing.split() if APT_LLVM.match(name)]
        if llvm:
            installed = " ".join(llvm)
            self.fail(f"apt LLVM packages installed: {installed}")

    def programs(self, directory: str) -> None:
        """Builds and runs the test programs in a directory.

        Raises:
            StepError: A test program does not build, or llvm-readelf or ldd
                cannot start.
        """
        (pathlib.Path(directory) / "hello.cpp").write_text(
            HELLO, encoding="utf-8"
        )
        builds = {
            "hello": ["-O2"],
            "hello-asan": ["-g", "-fsanitize=address,undefined"],
        }
        for program, flags in builds.items():
            command = ["clang++", "-std=c++23", *flags, "hello.cpp"]
            if not self.succeeds([*command, "-o", program], directory):
                raise github.StepError(f"clang++ cannot build {program}")
        hello = os.path.join(directory, "hello")
        if self.output([hello]).rstrip("\n") != "hello":
            self.fail("the test program does not run")
        if "libc++.so.1" not in self.output(["ldd", hello]):
            self.fail("the test program does not use libc++")
        hello_asan = os.path.join(directory, "hello-asan")
        if self.output([hello_asan]).rstrip("\n") != "hello":
            self.fail("the sanitized test program does not run")
        if no_gcc_runtime.check([hello, hello_asan]):
            self.fail("a test program needs a GCC runtime")


def main(argv: Sequence[str] | None = None) -> int:
    """Verifies the toolchain of the image it runs in.

    Args:
        argv: The arguments without the program name; there are none.

    Returns:
        The process exit status: 0 when every check passes, 1 otherwise.

    Raises:
        StepError: A test program does not build, or llvm-readelf or ldd
            cannot start.
    """
    argparse.ArgumentParser(description=__doc__.splitlines()[0]).parse_args(
        argv
    )
    verifier = Verifier()
    verifier.tools()
    verifier.runtimes()
    verifier.packages()
    with tempfile.TemporaryDirectory() as directory:
        verifier.programs(directory)
    return verifier.status
