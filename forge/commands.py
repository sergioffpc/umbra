# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
"""The steps of each forge command, and how they run.

A command is a list of steps, run in order until the first that fails. A
step is a program run as a subprocess from the repository root (cmake, ctest,
compute-sanitizer, a script) or a module of ``checks`` or ``forge`` imported
and called with an explicit argv. Each step is echoed to stderr before it
runs, prefixed with ``+ ``: a program as its command line, preceded by the
environment variables it sets, and a call as its module and arguments.
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
from forge import github

BUILD_DIR = git.REPO_ROOT / "build"
# The exit status of a program that cannot start.
NOT_FOUND_STATUS = 127


class Preset(enum.StrEnum):
    """The CMake presets that the commands run."""

    DEBUG = "debug"
    RELEASE = "release"
    ASAN = "asan"
    TSAN = "tsan"
    RTSAN = "rtsan"
    COVERAGE = "coverage"
    # A build preset only: it writes the coverage report.
    COVERAGE_REPORT = "coverage-report"


# The presets that build and check accept: debug, or release (LTO).
BUILD_PRESETS = (Preset.DEBUG, Preset.RELEASE)
# The build trees that audit accepts.
AUDIT_PRESETS = (
    Preset.DEBUG,
    Preset.RELEASE,
    Preset.ASAN,
    Preset.TSAN,
    Preset.RTSAN,
)
# The build trees that have the GPU module: the sanitizer presets leave it out.
GPU_PRESETS = (Preset.DEBUG, Preset.RELEASE)
# The checks of the ELF files of a build tree, shared with the deps workflow.
ELF_CHECKS = ("forge.no_gcc_runtime", "forge.no_libcuda")


class Option(enum.Enum):
    """The command-line options that a command may take."""

    PRESET = enum.auto()
    AUDIT_PRESET = enum.auto()
    LABEL = enum.auto()
    VERBOSE = enum.auto()
    CHECK = enum.auto()
    REF = enum.auto()
    BRANCH = enum.auto()
    IMAGE = enum.auto()
    TAG = enum.auto()
    OUTPUT = enum.auto()
    PUSHED = enum.auto()
    TAGS = enum.auto()
    LAYER = enum.auto()
    LAYER_OPTION = enum.auto()
    TOUCH = enum.auto()
    PATHS = enum.auto()
    PUBLISHED_REF = enum.auto()
    BUILDKIT_CACHE = enum.auto()
    PIN_TAG = enum.auto()
    PIN_BRANCH = enum.auto()
    BASE = enum.auto()
    PR_LABEL = enum.auto()


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
        check: Whether format only checks, failing when it would change a
            file.
        ref: An image: the deps image that the current inputs hash to, for
            pin and pin-devcontainer; the image looked up, for published.
        branch: The release or hotfix branch, for release, None for that of
            the GitHub Actions run; the branch to pin, for pin-devcontainer.
        image: The release image, in docker or in its registry, for
            release-image and sbom.
        tag: The release tag, for release-tag; None for that of the
            GitHub Actions run.
        output: The file that sbom writes.
        pushed: The pushed image by digest, IMAGE@DIGEST, for sign and
            tag-image.
        tags: The image's references to create, comma-separated, for
            tag-image.
        layer: The image layer, for layer-image and checkout-submodules.
        touch: Whether checkout-submodules gives the files of each
            submodule its commit time.
        paths: The files or directories that no-gcc-runtime and no-libcuda
            check.
        cache_action: load or save, for buildkit-cache.
        builder: The buildx builder, for buildkit-cache.
        cache_id: The id of the cache mount, for buildkit-cache.
        cache_target: Where the Dockerfile mounts it, for buildkit-cache.
        from_dir: The directory that buildkit-cache loads, if any.
        from_package: The package whose image buildkit-cache loads, if any.
        to_dir: The directory that buildkit-cache saves to, if any.
        to_package: The package whose image buildkit-cache saves to, if
            any.
        pin_tag: The tag of the image, for pin-devcontainer.
        base: The base branch of the pull requests, for bump-submodules.
        pr_label: The label of the pull requests, for bump-submodules.
    """

    preset: Preset = Preset.DEBUG
    label: str | None = None
    verbose: bool = False
    check: bool = False
    ref: str = ""
    branch: str | None = None
    image: str = ""
    tag: str | None = None
    output: str = ""
    pushed: str = ""
    tags: str = ""
    layer: str = ""
    touch: bool = False
    paths: tuple[str, ...] = ()
    cache_action: str = ""
    builder: str = ""
    cache_id: str = ""
    cache_target: str = ""
    from_dir: str | None = None
    from_package: str | None = None
    to_dir: str | None = None
    to_package: str | None = None
    pin_tag: str = ""
    base: str = ""
    pr_label: str = ""

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
class Call:
    """A module of ``checks`` or ``forge`` called in this process.

    Attributes:
        module: The module's name; its ``main(argv)`` returns an exit status.
        argv: The command-line arguments passed to ``main``.
    """

    module: str
    argv: Sequence[str] = ()

    def __str__(self) -> str:
        return shlex.join([self.module, *self.argv])

    def run(self) -> int:
        """Calls the module's ``main`` and returns its exit status.

        A step that cannot run (``github.StepError``) is reported as an
        error, with exit status 1.
        """
        try:
            status = importlib.import_module(self.module).main(list(self.argv))
        except github.StepError as error:
            github.error(str(error))
            status = 1
        sys.stdout.flush()
        return status


Step = Program | Call


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


def check_rtsan(options: Options) -> list[Step]:
    """Returns the steps of the RTSan build and tests."""
    return check(options.with_preset(Preset.RTSAN))


def lint(options: Options) -> list[Step]:
    """Returns the steps that check without changing any file.

    The validators shared with the git hooks (the file validators on every
    tracked file, the current branch name, the commit messages since
    develop), the formatting, then clang-tidy, the clang-query rules, pylint,
    actionlint and the Python embedded in the workflows.
    """
    del options  # Unused: lint takes no options.
    return [
        Call("checks.conventions"),
        Call("checks.branch_name", ["--current"]),
        Call("checks.commit_message", ["--since", "develop"]),
        Call("checks.formatting", ["--check"]),
        Call("checks.lint"),
    ]


def check_all(options: Options) -> list[Step]:
    """Returns the steps of lint, check and the sanitizer checks."""
    return [
        *lint(options),
        *check(options),
        *check_asan(options),
        *check_tsan(options),
        *check_rtsan(options),
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
    """Returns the step that formats, or with --check checks, the sources."""
    return [Call("checks.formatting", ["--check"] if options.check else [])]


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


def audit(options: Options) -> list[Step]:
    """Returns the steps that check the build tree of a preset.

    The debug and release trees must have the GPU module. No ELF file of the
    tree may need a GCC runtime, or the CUDA driver unless it is a module
    loaded at run time.
    """
    tree = f"build/{options.preset}"
    gpu_module = [Call("forge.gpu_module", [tree])]
    return [
        *(gpu_module if options.preset in GPU_PRESETS else []),
        *(Call(module, [tree]) for module in ELF_CHECKS),
    ]


def pin(options: Options) -> list[Step]:
    """Returns the step that checks the devcontainer's deps image pin."""
    return [Call("forge.pin", ["--ref", options.ref])]


def pull_request(options: Options) -> list[Step]:
    """Returns the step that checks the pull request of the run."""
    del options  # Unused: pull-request takes no options.
    return [Call("forge.pull_request")]


def third_party(options: Options) -> list[Step]:
    """Returns the step that checks the third-party versions manifest."""
    del options  # Unused: third-party takes no options.
    return [Call("forge.third_party")]


def release(options: Options) -> list[Step]:
    """Returns the step that checks a release or hotfix branch."""
    branch = [] if options.branch is None else ["--branch", options.branch]
    return [Call("forge.release", branch)]


def changes(options: Options) -> list[Step]:
    """Returns the step that tells whether a pull request changes code."""
    del options  # Unused: changes takes no options.
    return [Call("forge.changes")]


def image(options: Options) -> list[Step]:
    """Returns the step that resolves the pinned deps image."""
    del options  # Unused: image takes no options.
    return [Call("forge.image")]


def release_image(options: Options) -> list[Step]:
    """Returns the step that checks the release image and starts umbra."""
    return [Call("forge.release_image", [options.image])]


def release_tag(options: Options) -> list[Step]:
    """Returns the step that checks a release tag and names the image tags."""
    tag = [] if options.tag is None else ["--tag", options.tag]
    return [Call("forge.release_tag", tag)]


def sbom(options: Options) -> list[Step]:
    """Returns the step that writes the SPDX SBOM of the release image."""
    return [Call("forge.sbom", [options.image, "--output", options.output])]


def sign(options: Options) -> list[Step]:
    """Returns the step that signs a pushed image with cosign, keyless.

    cosign signs under the identity of the OIDC token of the GitHub Actions
    job, and records the signature in Sigstore's transparency log.
    """
    return [Program(["cosign", "sign", "--yes", options.pushed])]


def tag_image(options: Options) -> list[Step]:
    """Returns the step that tags a pushed image in its registry.

    Each tag points to the pushed manifest itself, so that the image checked,
    signed and attested by digest is the one the tags name.
    """
    tags = [tag for tag in options.tags.split(",") if tag]
    flags = [flag for tag in tags for flag in ("--tag", tag)]
    return [
        Program(
            ["docker", "buildx", "imagetools", "create", *flags, options.pushed]
        )
    ]


def layer_image(options: Options) -> list[Step]:
    """Returns the step that computes the reference of a layer image."""
    return [Call("forge.layer_image", [options.layer])]


def checkout_submodules(options: Options) -> list[Step]:
    """Returns the step that checks out the submodules of a layer image."""
    touch = ["--touch"] if options.touch else []
    return [
        Call("forge.checkout_submodules", ["--layer", options.layer, *touch])
    ]


def no_gcc_runtime(options: Options) -> list[Step]:
    """Returns the step that checks ELF files for a GCC runtime."""
    return [Call("forge.no_gcc_runtime", options.paths)]


def no_libcuda(options: Options) -> list[Step]:
    """Returns the step that checks ELF files for the CUDA driver."""
    return [Call("forge.no_libcuda", options.paths)]


def published(options: Options) -> list[Step]:
    """Returns the step that tells whether an image is published."""
    return [Call("forge.published", [options.ref])]


def buildkit_cache(options: Options) -> list[Step]:
    """Returns the step that loads or saves a BuildKit cache mount."""
    places = {
        "--from-dir": options.from_dir,
        "--from-package": options.from_package,
        "--to-dir": options.to_dir,
        "--to-package": options.to_package,
    }
    return [
        Call(
            "forge.buildkit_cache",
            [
                options.cache_action,
                "--builder",
                options.builder,
                "--id",
                options.cache_id,
                "--target",
                options.cache_target,
                *(
                    argument
                    for flag, value in places.items()
                    if value is not None
                    for argument in (flag, value)
                ),
            ],
        )
    ]


def verify_toolchain(options: Options) -> list[Step]:
    """Returns the step that verifies the toolchain image it runs in."""
    del options  # Unused: verify-toolchain takes no options.
    return [Call("forge.verify_toolchain")]


def pin_devcontainer(options: Options) -> list[Step]:
    """Returns the step that pins the deps image on a branch."""
    return [
        Call(
            "forge.pin_devcontainer",
            [
                "--ref",
                options.ref,
                "--tag",
                options.pin_tag,
                "--branch",
                options.branch or "",
            ],
        )
    ]


def bump_submodules(options: Options) -> list[Step]:
    """Returns the step that opens the submodule bump pull requests."""
    return [
        Call(
            "forge.bump_submodules",
            ["--base", options.base, "--label", options.pr_label],
        )
    ]


def modules(options: Options) -> list[Step]:
    """Returns the step that checks that the target graph is acyclic."""
    del options  # Unused: modules takes no options.
    return [Call("forge.modules")]


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
        "check-rtsan",
        "Build the RTSan preset and run its tests: the functions declared"
        " nonblocking neither allocate, lock nor block.",
        check_rtsan,
        (Option.VERBOSE,),
    ),
    Command(
        "check-all",
        "Run lint, check, check-asan, check-tsan and check-rtsan: everything"
        " before a pull request.",
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
    Command(
        "format",
        "Format the C++ and Python sources; with --check, fail when a file"
        " is not formatted instead.",
        format_sources,
        (Option.CHECK,),
    ),
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
    Command(
        "audit",
        "Check a build tree: the GPU module (debug and release), no GCC"
        " runtime and no CUDA driver.",
        audit,
        (Option.AUDIT_PRESET,),
    ),
    Command(
        "pin",
        "Check that the devcontainer pins the deps image of the current"
        " inputs.",
        pin,
        (Option.REF,),
    ),
    Command(
        "pull-request",
        "Check the branch name, title and commits of the run's pull request"
        " (CI).",
        pull_request,
    ),
    Command(
        "third-party",
        "Check the third-party versions manifest against the submodules, the"
        " license allow-list and the upstream tags.",
        third_party,
    ),
    Command(
        "release",
        "Check the version, changelog and tags of a release or hotfix"
        " branch.",
        release,
        (Option.BRANCH,),
    ),
    Command(
        "changes",
        "Tell whether the run's pull request changes more than documentation"
        " (CI).",
        changes,
    ),
    Command(
        "image",
        "Resolve the pinned deps image and check that it is published (CI).",
        image,
    ),
    Command("modules", "Check that the target graph is acyclic.", modules),
    Command(
        "release-image",
        "Check the contract of the release image and start umbra in it"
        " (CI).",
        release_image,
        (Option.IMAGE,),
    ),
    Command(
        "release-tag",
        "Check that the release tag is signed, vX.Y.Z and on main, and name"
        " the release image's tags (CI).",
        release_tag,
        (Option.TAG,),
    ),
    Command(
        "sbom",
        "Write the SPDX SBOM of the release image: its Ubuntu packages,"
        " umbra and the submodules in it.",
        sbom,
        (Option.IMAGE, Option.OUTPUT),
    ),
    Command(
        "sign",
        "Sign a pushed image with cosign, keyless, under the identity of the"
        " GitHub Actions job (CI).",
        sign,
        (Option.PUSHED,),
    ),
    Command(
        "tag-image",
        "Tag a pushed image in its registry with every given reference.",
        tag_image,
        (Option.PUSHED, Option.TAGS),
    ),
    Command(
        "layer-image",
        "Compute the content-addressed reference of a container image layer"
        " (CI).",
        layer_image,
        (Option.LAYER,),
    ),
    Command(
        "published",
        "Tell whether an image is published in its registry (CI).",
        published,
        (Option.PUBLISHED_REF,),
    ),
    Command(
        "checkout-submodules",
        "Check out the submodules of a layer image; with --touch, give their"
        " files their commit time (CI).",
        checkout_submodules,
        (Option.LAYER_OPTION, Option.TOUCH),
    ),
    Command(
        "buildkit-cache",
        "Load a directory or a package image into a BuildKit cache mount, or"
        " save the mount into one (CI).",
        buildkit_cache,
        (Option.BUILDKIT_CACHE,),
    ),
    Command(
        "no-gcc-runtime",
        "Check that no ELF file under some paths needs a GCC runtime.",
        no_gcc_runtime,
        (Option.PATHS,),
    ),
    Command(
        "no-libcuda",
        "Check that no ELF file under some paths needs the CUDA driver,"
        " except the modules loaded at run time.",
        no_libcuda,
        (Option.PATHS,),
    ),
    Command(
        "verify-toolchain",
        "Verify the tools and runtimes of the toolchain image it runs in"
        " (CI).",
        verify_toolchain,
    ),
    Command(
        "pin-devcontainer",
        "Pin the deps image in the devcontainer configuration of a branch,"
        " then dispatch CI on it (CI).",
        pin_devcontainer,
        (Option.REF, Option.PIN_TAG, Option.PIN_BRANCH),
    ),
    Command(
        "bump-submodules",
        "Open a pull request per submodule with a newer upstream release tag"
        " (CI).",
        bump_submodules,
        (Option.BASE, Option.PR_LABEL),
    ),
)
