# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
"""Lints the repository: clang-tidy, the clang-query rules, pylint, actionlint.

clang-tidy (through LLVM's run-clang-tidy, in parallel) and the clang-query
rules analyse the translation units of the compilation database: all of them,
or only those the given files affect. pylint lints the Python files and
actionlint (with shellcheck) the workflows, all of them or those given. The
Python embedded in the workflows (see ``checks.embedded``) is checked for
formatting and linted with pylint too.

When the image provides ctcache, clang-tidy runs through it, by way of
``checks/clang_tidy_cache.py``: a translation unit whose inputs match those
of an earlier clean run is not analysed again. The inputs are the
preprocessed translation unit, with its comments (so a NOLINT counts) and
its #define and #include directives, clang-tidy's arguments and its
configuration (``--dump-config``). The cache lives under
``$XDG_CACHE_HOME/umbra/clang-tidy`` (``~/.cache`` by default), in a
directory per clang-tidy binary, so that another clang-tidy starts from an
empty cache; the directories of other binaries are removed. Only clean runs
are cached: a translation unit with findings is always analysed again.

The compilation database is this module's: it comes from the debug preset,
which ``run`` configures (``cmake --preset debug``) so the database is
current. Another build tree given with -p must already have one.

Usage: python3 -m checks.lint [-p BUILD_DIR] [PATH...]
"""

import argparse
from collections.abc import Sequence
import hashlib
import os
import pathlib
import re
import shutil
import sys
from typing import NamedTuple

from checks import affected
from checks import clang_query
from checks import embedded
from checks import formatting
from checks import git
from checks import report
from checks import sources

# ctcache's client, which the image installs.
CTCACHE_CLIENT = pathlib.Path("/opt/ctcache/clang_tidy_cache.py")
# The script through which run-clang-tidy runs clang-tidy and ctcache.
CLANG_TIDY_CACHE = git.PACKAGE_ROOT / "checks" / "clang_tidy_cache.py"

TOOLS = (
    "cmake",
    "run-clang-tidy",
    "clang-tidy",
    "clang-query",
    "pylint",
    "actionlint",
)


def missing_tools() -> list[str]:
    """Returns the lint tools that are not on PATH."""
    return [tool for tool in TOOLS if shutil.which(tool) is None]


class ClangTidyCache(NamedTuple):
    """How run-clang-tidy runs clang-tidy through ctcache.

    Attributes:
        arguments: The arguments of run-clang-tidy that select
            ``checks/clang_tidy_cache.py`` as its clang-tidy.
        environment: The variables that script and ctcache read.
    """

    arguments: list[str]
    environment: dict[str, str]


def clang_tidy_cache_root() -> pathlib.Path:
    """Returns the directory that holds clang-tidy's caches."""
    base = os.environ.get("XDG_CACHE_HOME") or str(
        pathlib.Path.home() / ".cache"
    )
    return pathlib.Path(base) / "umbra" / "clang-tidy"


def binary_digest(path: pathlib.Path) -> str:
    """Returns a short digest of the contents of a file."""
    digest = hashlib.sha256()
    with path.open("rb") as binary:
        for chunk in iter(lambda: binary.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()[:16]


def prepare_clang_tidy_cache() -> ClangTidyCache:
    """Prepares the cache of the clang-tidy on PATH, which ctcache runs.

    Creates the cache directory of that binary and removes those of other
    binaries.

    Returns:
        How run-clang-tidy runs clang-tidy through the cache; empty when the
        image has no ctcache, and clang-tidy then runs uncached.
    """
    found = shutil.which("clang-tidy")
    if not CTCACHE_CLIENT.is_file() or found is None:
        return ClangTidyCache([], {})
    binary = pathlib.Path(found).resolve()
    root = clang_tidy_cache_root()
    directory = root / binary_digest(binary)
    if root.is_dir():
        for other in root.iterdir():
            if other != directory and other.is_dir():
                shutil.rmtree(other, ignore_errors=True)
    directory.mkdir(parents=True, exist_ok=True)
    return ClangTidyCache(
        ["-clang-tidy-binary", str(CLANG_TIDY_CACHE)],
        {
            "CTCACHE_CLIENT": str(CTCACHE_CLIENT),
            "CTCACHE_CLANG_TIDY": str(binary),
            "CTCACHE_DIR": str(directory),
            "CTCACHE_KEEP_COMMENTS": "1",
        },
    )


def ensure_compile_database(build_dir: pathlib.Path) -> int:
    """Makes the compilation database of a build tree current.

    Configures the debug preset when the build tree is its own; any other
    build tree must already hold compile_commands.json.

    Args:
        build_dir: The build tree holding compile_commands.json.

    Returns:
        The process exit status: 0 when the database is ready.
    """
    if build_dir.resolve() == affected.DEFAULT_BUILD_DIR:
        status = git.call(["cmake", "--preset", "debug"], quiet=True).returncode
        return report.exit_status(
            "cannot configure the debug preset", "cmake --preset debug", status
        )
    database = build_dir / "compile_commands.json"
    return report.fail(
        "cannot lint the C++ sources",
        [] if database.is_file() else [f"{database} does not exist"],
    )


def clang_tidy(
    translation_units: Sequence[str], build_dir: pathlib.Path
) -> int:
    """Runs clang-tidy over translation units, in parallel.

    It runs through the cache when the image has ctcache.

    Args:
        translation_units: The sources, relative to the repository root.
        build_dir: The build tree holding compile_commands.json.

    Returns:
        The process exit status: 0 when clang-tidy reports nothing.
    """
    cache = prepare_clang_tidy_cache()
    # run-clang-tidy selects the database entries whose path matches.
    patterns = [
        f"^{re.escape(str(git.REPO_ROOT / path))}$"
        for path in translation_units
    ]
    command = [
        "run-clang-tidy",
        "-quiet",
        "-p",
        str(build_dir),
        "-j",
        str(os.cpu_count() or 1),
        *cache.arguments,
        *patterns,
    ]
    status = git.call(command, env=cache.environment).returncode
    return report.exit_status(
        "clang-tidy found problems", "run-clang-tidy", status
    )


def pylint(paths: Sequence[str]) -> int:
    """Runs pylint (configured by .pylintrc) over Python files."""
    status = git.call(["pylint", "--score=n", *paths]).returncode
    return report.exit_status("pylint found problems", "pylint", status)


def embedded_python(scripts: Sequence[embedded.Script]) -> int:
    """Checks the formatting of embedded Python scripts and runs pylint.

    pylint reads each script from standard input under its workflow's name,
    so its messages carry the workflow's path and line numbers.

    Args:
        scripts: The scripts.

    Returns:
        The process exit status: 0 when every script passes.
    """
    errors = [
        error
        for script in scripts
        for error in formatting.check_python(
            script.path,
            script.code,
            "format it as pyink and isort would",
            script.name,
        )
    ]
    status = report.fail("embedded Python is not formatted", errors)
    for script in scripts:
        result = git.call(
            ["pylint", "--score=n", "--from-stdin", script.path],
            stdin=script.aligned(),
        )
        status = (
            report.exit_status(
                f"pylint found problems in {script.name}",
                "pylint",
                result.returncode,
            )
            or status
        )
    return status


def actionlint(paths: Sequence[str]) -> int:
    """Runs actionlint (and shellcheck through it) over workflows."""
    status = git.call(["actionlint", *paths]).returncode
    return report.exit_status("actionlint found problems", "actionlint", status)


def run(paths: Sequence[str] | None, build_dir: pathlib.Path) -> int:
    """Lints the given files, or the whole repository.

    Args:
        paths: The changed files, relative to the repository root; None for
            every file.
        build_dir: The build tree holding compile_commands.json.

    Returns:
        The process exit status: 0 when every linter passes, 1 otherwise.
    """
    missing = missing_tools()
    if missing:
        return report.fail(
            "cannot lint",
            [
                "not found on PATH: " + ", ".join(missing),
                "lint in the devcontainer, the only supported environment",
            ],
        )
    if ensure_compile_database(build_dir):
        return 1
    if paths is None:
        files = sources.tracked()
        units = affected.database_sources(build_dir)
    else:
        files = list(paths)
        units = affected.translation_units(files, build_dir)
    status = 0
    if units:
        status = clang_tidy(units, build_dir) or status
        status = clang_query.run(units, build_dir) or status
    python = [path for path in files if sources.is_python(path)]
    if python:
        status = pylint(python) or status
    workflows = [path for path in files if sources.is_workflow(path)]
    if workflows:
        status = actionlint(workflows) or status
        scripts = embedded.scripts_of(workflows)
        if scripts:
            status = embedded_python(scripts) or status
    return status


def main(argv: Sequence[str] | None = None) -> int:
    """Lints the files given on the command line, or the whole repository.

    Args:
        argv: The arguments without the program name; ``sys.argv[1:]`` when
            None.

    Returns:
        The process exit status: 0 when every linter passes, 1 otherwise.
    """
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "-p",
        dest="build_dir",
        type=pathlib.Path,
        default=affected.DEFAULT_BUILD_DIR,
        help="build tree with compile_commands.json (default: build/debug)",
    )
    parser.add_argument(
        "paths",
        nargs="*",
        metavar="PATH",
        help="changed file relative to the repository root (default: all)",
    )
    args = parser.parse_args(argv)
    return run(args.paths or None, args.build_dir)


if __name__ == "__main__":
    sys.exit(main())
