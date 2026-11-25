# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
"""The steps of each forge command, and how they run.

A command is a list of steps, run in order until the first that fails. A
step is a program run as a subprocess from the repository root (cmake, ctest,
compute-sanitizer) or a module of ``checks`` imported and called with an
explicit argv. Each step is echoed to stderr before it runs, prefixed with
``+ ``: a program as its command line, preceded by the environment variables
it sets, and a check as its module and arguments.
"""

from collections.abc import Callable, Mapping, Sequence
import dataclasses
import enum
import importlib
import os
import pathlib
import shlex
import subprocess
import sys

from checks import git

BUILD_DIR = git.REPO_ROOT / "build"
# The exit status of a program that cannot start.
NOT_FOUND_STATUS = 127


class Preset(enum.StrEnum):
    """The CMake presets that the commands run."""

    DEBUG = "debug"
    RELEASE = "release"
    ASAN = "asan"
    TSAN = "tsan"
    COVERAGE = "coverage"
    # A build preset only: it writes the coverage report.
    COVERAGE_REPORT = "coverage-report"


# The presets that build and check accept: debug, or release (LTO).
BUILD_PRESETS = (Preset.DEBUG, Preset.RELEASE)


class Option(enum.Enum):
    """The command-line options that a command may take."""

    PRESET = enum.auto()
    LABEL = enum.auto()
    VERBOSE = enum.auto()


@dataclasses.dataclass(frozen=True)
class Options:
    """The options of a command.

    Attributes:
        preset: The preset that the command configures, builds and tests.
        label: A regular expression. When given, only the tests with a
            matching CTest label run, from the build tree; otherwise the test
            preset runs, which leaves out the tests that need a GPU.
        verbose: Whether the tools print as much as they can: the
            configure's messages down to TRACE, every command the build
            runs, and the full output of every test.
    """

    preset: Preset = Preset.DEBUG
    label: str | None = None
    verbose: bool = False

    def with_preset(self, preset: Preset) -> "Options":
        """Returns these options with another preset."""
        return dataclasses.replace(self, preset=preset)

    def configure_flags(self) -> list[str]:
        """Returns the flags of the configure."""
        return ["--log-level=TRACE"] if self.verbose else []

    def build_flags(self) -> list[str]:
        """Returns the flags of a build."""
        return ["--verbose"] if self.verbose else []

    def test_flags(self) -> list[str]:
        """Returns the flags of ctest."""
        return ["--extra-verbose"] if self.verbose else []


@dataclasses.dataclass(frozen=True)
class Program:
    """A program run as a subprocess from the repository root.

    Attributes:
        args: The program and its arguments.
        env: Environment variables set on top of the inherited environment.
    """

    args: Sequence[str]
    env: Mapping[str, str] = dataclasses.field(default_factory=dict)

    def __str__(self) -> str:
        assignments = [
            f"{name}={shlex.quote(value)}" for name, value in self.env.items()
        ]
        return " ".join([*assignments, shlex.join(self.args)])

    def run(self) -> int:
        """Runs the program.

        Returns:
            Its exit status; 128 plus the signal number when a signal
            killed it, and 127 when it cannot start.
        """
        sys.stdout.flush()
        env = {**os.environ, **self.env} if self.env else None
        try:
            status = subprocess.run(
                self.args, cwd=git.REPO_ROOT, env=env, check=False
            ).returncode
        except OSError as error:
            print(f"forge: {self.args[0]}: {error.strerror}", file=sys.stderr)
            return NOT_FOUND_STATUS
        return 128 - status if status < 0 else status


@dataclasses.dataclass(frozen=True)
class Check:
    """A module of ``checks`` called in this process.

    Attributes:
        module: The module's name; its ``main(argv)`` returns an exit status.
        argv: The command-line arguments passed to ``main``.
    """

    module: str
    argv: Sequence[str] = ()

    def __str__(self) -> str:
        return shlex.join([self.module, *self.argv])

    def run(self) -> int:
        """Calls the module's ``main`` and returns its exit status."""
        status = importlib.import_module(self.module).main(list(self.argv))
        sys.stdout.flush()
        return status


Step = Program | Check


def execute(steps: Sequence[Step]) -> int:
    """Runs the steps in order, echoing each, until the first that fails.

    Args:
        steps: The steps of a command.

    Returns:
        The exit status of the step that failed, or 0 when every step passed.
    """
    for step in steps:
        print(f"+ {step}", file=sys.stderr, flush=True)
        status = step.run()
        if status:
            return status
    return 0


@dataclasses.dataclass(frozen=True)
class SanitizerRun:
    """A run of the selected tests under NVIDIA's compute-sanitizer.

    Attributes:
        tool: The compute-sanitizer tool and its options.
        env: Environment variables that the tests need under the tool.
    """

    tool: Sequence[str]
    env: Mapping[str, str] = dataclasses.field(default_factory=dict)


# compute-sanitizer fails the run when it finds an error, and follows the
# test processes that ctest starts.
GPU_SANITIZER = (
    "compute-sanitizer",
    "--error-exitcode",
    "1",
    "--target-processes",
    "all",
)
# AddressSanitizer keeps the shadow gap of the address space unmapped, where
# the CUDA driver maps device memory, so that cudaMalloc would fail with out
# of memory: protect_shadow_gap=0 lets the driver map it.
ASAN_GPU_ENV = {"ASAN_OPTIONS": "protect_shadow_gap=0"}
# memcheck: out-of-bounds and misaligned device accesses, device memory
# leaks; initcheck: reads of uninitialized device memory.
ASAN_GPU_RUNS = (
    SanitizerRun(("--tool", "memcheck", "--leak-check", "full"), ASAN_GPU_ENV),
    SanitizerRun(("--tool", "initcheck"), ASAN_GPU_ENV),
)
# racecheck: shared-memory races; synccheck: misused barriers.
TSAN_GPU_RUNS = (
    SanitizerRun(("--tool", "racecheck")),
    SanitizerRun(("--tool", "synccheck")),
)


def ctest_args(options: Options) -> list[str]:
    """Returns the ctest command line that runs the tests of a preset.

    Args:
        options: The preset, whose build tree is ``build/<preset>``, the
            label that selects the tests, and the verbosity.
    """
    if options.label is None:
        return ["ctest", "--preset", options.preset, *options.test_flags()]
    return [
        "ctest",
        "--test-dir",
        f"build/{options.preset}",
        "--output-on-failure",
        "-L",
        options.label,
        *options.test_flags(),
    ]


def build(options: Options) -> list[Step]:
    """Returns the steps that configure and build a preset."""
    return [
        Program(
            ["cmake", "--preset", options.preset, *options.configure_flags()]
        ),
        Program(
            [
                "cmake",
                "--build",
                "--preset",
                options.preset,
                *options.build_flags(),
            ]
        ),
    ]


def check(options: Options = Options()) -> list[Step]:
    """Returns the steps that build a preset and run its tests."""
    return [*build(options), Program(ctest_args(options))]


def sanitizer_check(
    options: Options, gpu_runs: Sequence[SanitizerRun]
) -> list[Step]:
    """Returns the steps that build a sanitizer preset and run its tests.

    With a label, the selected tests run under each of the compute-sanitizer
    runs instead, which check the kernels the tests launch.

    Args:
        options: The sanitizer preset, the label and the verbosity.
        gpu_runs: The compute-sanitizer runs of the labelled tests.
    """
    if options.label is None:
        return check(options)
    tests = ctest_args(options)
    return [
        *build(options),
        *(
            Program([*GPU_SANITIZER, *run.tool, *tests], run.env)
            for run in gpu_runs
        ),
    ]


def check_asan(options: Options) -> list[Step]:
    """Returns the steps of the ASan + UBSan build and tests."""
    return sanitizer_check(options.with_preset(Preset.ASAN), ASAN_GPU_RUNS)


def check_tsan(options: Options) -> list[Step]:
    """Returns the steps of the TSan + UBSan build and tests."""
    return sanitizer_check(options.with_preset(Preset.TSAN), TSAN_GPU_RUNS)


def lint(options: Options) -> list[Step]:
    """Returns the steps that check without changing any file.

    The validators shared with the git hooks (the file validators on every
    tracked file, the current branch name, the commit messages since
    develop), the formatting, then clang-tidy, the clang-query rules, pylint,
    actionlint and the Python embedded in the workflows.
    """
    del options  # Unused: lint takes no options.
    return [
        Check("checks.conventions"),
        Check("checks.branch_name", ["--current"]),
        Check("checks.commit_message", ["--since", "develop"]),
        Check("checks.formatting", ["--check"]),
        Check("checks.lint"),
    ]


def check_all(options: Options) -> list[Step]:
    """Returns the steps of lint, check, check-asan and check-tsan."""
    return [
        *lint(options),
        *check(options),
        *check_asan(options),
        *check_tsan(options),
    ]


def coverage(options: Options) -> list[Step]:
    """Returns the steps of the coverage build, tests and report.

    The report is written to build/coverage/report.
    """
    report = options.with_preset(Preset.COVERAGE_REPORT)
    return [
        *check(options.with_preset(Preset.COVERAGE)),
        Program(
            [
                "cmake",
                "--build",
                "--preset",
                report.preset,
                *report.build_flags(),
            ]
        ),
    ]


def format_sources(options: Options) -> list[Step]:
    """Returns the step that formats the C++ and Python sources."""
    del options  # Unused: format takes no options.
    return [Check("checks.formatting")]


def relative(path: pathlib.Path) -> str:
    """Returns a path relative to the repository root, as a POSIX string."""
    return path.relative_to(git.REPO_ROOT).as_posix()


def clean(options: Options) -> list[Step]:
    """Returns the steps that remove what the builds built.

    Each configured build tree is cleaned, keeping its configuration; a tree
    that was never configured has nothing to clean.
    """
    if not BUILD_DIR.is_dir():
        return []
    trees = sorted(
        tree
        for tree in BUILD_DIR.iterdir()
        if (tree / "CMakeCache.txt").is_file()
    )
    return [
        Program(
            [
                "cmake",
                "--build",
                relative(tree),
                "--target",
                "clean",
                *options.build_flags(),
            ]
        )
        for tree in trees
    ]


def distclean(options: Options) -> list[Step]:
    """Returns the step that removes the build trees and their configuration."""
    del options  # Unused: distclean takes no options.
    return [Program(["rm", "-rf", relative(BUILD_DIR)])]


@dataclasses.dataclass(frozen=True)
class Command:
    """A forge command.

    Attributes:
        name: The name given on the command line.
        summary: What the command does, for its help.
        steps: Returns the steps of the command for its options.
        options: The command-line options it takes.
    """

    name: str
    summary: str
    steps: Callable[[Options], list[Step]]
    options: tuple[Option, ...] = ()


COMMANDS = (
    Command(
        "build",
        "Build a preset.",
        build,
        (Option.PRESET, Option.VERBOSE),
    ),
    Command(
        "check",
        "Build a preset and run its tests, except those that need a GPU.",
        check,
        (Option.PRESET, Option.LABEL, Option.VERBOSE),
    ),
    Command(
        "check-asan",
        "Build the ASan + UBSan preset and run its tests; with --label, also"
        " under compute-sanitizer memcheck and initcheck.",
        check_asan,
        (Option.LABEL, Option.VERBOSE),
    ),
    Command(
        "check-tsan",
        "Build the TSan + UBSan integer checks preset and run its tests; with"
        " --label, also under compute-sanitizer racecheck and synccheck.",
        check_tsan,
        (Option.LABEL, Option.VERBOSE),
    ),
    Command(
        "check-all",
        "Run lint, check, check-asan and check-tsan: everything before a pull"
        " request.",
        check_all,
        (Option.VERBOSE,),
    ),
    Command(
        "coverage",
        "Build the coverage preset, run its tests and write the report to"
        " build/coverage/report.",
        coverage,
        (Option.VERBOSE,),
    ),
    Command("format", "Format the C++ and Python sources.", format_sources),
    Command(
        "lint",
        "Check without changing any file: validators, formatting, static"
        " analysis.",
        lint,
    ),
    Command(
        "clean",
        "Remove what the builds built, keeping their configuration.",
        clean,
        (Option.VERBOSE,),
    ),
    Command(
        "distclean",
        "Remove the build trees, with their configuration.",
        distclean,
    ),
)
