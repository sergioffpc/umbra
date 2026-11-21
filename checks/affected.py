# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
"""The translation units a change affects.

A translation unit of the compilation database is affected when it changed,
or when it includes a changed file directly or through other project headers.
Project includes are quoted, either relative to the including file or as
"umbra/<module>/<header>.h" from include/. A change to a .clang-tidy file
affects every translation unit below its directory.

Only tracked sources of the repository count: generated sources in the build
tree are not analysed.

Usage: python3 -m checks.affected [-p BUILD_DIR] [PATH...]
"""

import argparse
from collections.abc import Iterable, Sequence
import json
import pathlib
import posixpath
import re
import sys

from checks import git
from checks import sources

DEFAULT_BUILD_DIR = git.REPO_ROOT / "build" / "debug"
QUOTED_INCLUDE = re.compile(r'^\s*#\s*include\s*"([^"]+)"', re.MULTILINE)
PUBLIC_INCLUDE_DIR = "include"
TIDY_CONFIG = ".clang-tidy"


def database_sources(build_dir: pathlib.Path) -> list[str]:
    """Returns the tracked sources of a compilation database.

    Args:
        build_dir: The build tree holding compile_commands.json.

    Returns:
        The sources compiled in the build tree that are files of the
        repository, relative to its root, without duplicates.
    """
    database = json.loads(
        (build_dir / "compile_commands.json").read_text(encoding="utf-8")
    )
    tracked = set(sources.tracked())
    found = []
    for entry in database:
        path = pathlib.Path(entry["directory"], entry["file"]).resolve()
        if not path.is_relative_to(git.REPO_ROOT):
            continue
        relative = path.relative_to(git.REPO_ROOT).as_posix()
        if relative in tracked and relative not in found:
            found.append(relative)
    return found


def includes(path: str, tracked: set[str]) -> set[str]:
    """Returns the tracked files a C++ file includes with quotes.

    Args:
        path: The including file, relative to the repository root.
        tracked: The tracked files.

    Returns:
        The included project files, relative to the repository root.
    """
    text = sources.read_working_tree(path)
    found = set()
    for name in QUOTED_INCLUDE.findall(text):
        for candidate in (
            posixpath.normpath(posixpath.join(posixpath.dirname(path), name)),
            posixpath.join(PUBLIC_INCLUDE_DIR, name),
        ):
            if candidate in tracked:
                found.add(candidate)
                break
    return found


def affected_files(changed: Iterable[str], tracked: set[str]) -> set[str]:
    """Returns the changed files and every C++ file that includes one.

    Args:
        changed: The changed files, relative to the repository root.
        tracked: The tracked files.

    Returns:
        The changed files and the C++ files that include them, directly or
        through other headers.
    """
    included_by: dict[str, set[str]] = {}
    for path in filter(sources.is_cpp, tracked):
        for header in includes(path, tracked):
            included_by.setdefault(header, set()).add(path)
    affected = set(changed)
    pending = list(affected)
    while pending:
        for includer in included_by.get(pending.pop(), ()):
            if includer not in affected:
                affected.add(includer)
                pending.append(includer)
    return affected


def translation_units(
    changed: Iterable[str], build_dir: pathlib.Path = DEFAULT_BUILD_DIR
) -> list[str]:
    """Returns the translation units a change affects.

    Args:
        changed: The changed files, relative to the repository root.
        build_dir: The build tree holding compile_commands.json.

    Returns:
        The affected sources of the compilation database, relative to the
        repository root.
    """
    changed = list(changed)
    config_dirs = [
        posixpath.dirname(path)
        for path in changed
        if posixpath.basename(path) == TIDY_CONFIG
    ]
    affected = affected_files(changed, set(sources.tracked()))
    return [
        path
        for path in database_sources(build_dir)
        if path in affected
        or any(
            not directory or path.startswith(directory + "/")
            for directory in config_dirs
        )
    ]


def main(argv: Sequence[str] | None = None) -> int:
    """Prints the translation units the given files affect, one per line.

    Args:
        argv: The arguments without the program name; ``sys.argv[1:]`` when
            None.

    Returns:
        The process exit status, 0.
    """
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "-p",
        dest="build_dir",
        type=pathlib.Path,
        default=DEFAULT_BUILD_DIR,
        help="build tree with compile_commands.json (default: build/debug)",
    )
    parser.add_argument(
        "paths",
        nargs="*",
        metavar="PATH",
        help="changed file relative to the repository root",
    )
    args = parser.parse_args(argv)
    for path in translation_units(args.paths, args.build_dir):
        print(path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
