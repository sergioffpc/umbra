# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
"""Checks out the submodules that a layer image is built from.

Only the submodules among the layer's inputs (see ``layer_image``) are
checked out, at depth 1. With --touch, every file of each checked-out
submodule then gets the time of the submodule's commit as its modification
time, so that a build tree restored from a cache is newer than the sources
and ninja resumes instead of rebuilding everything. Needs git.

Usage: python3 -m forge checkout-submodules --layer LAYER [--touch]
"""

import argparse
from collections.abc import Sequence
import os
import pathlib

from checks import git
from forge import github
from forge import layer_image


def checkout(paths: Sequence[str]) -> None:
    """Checks out the given submodules at depth 1.

    Raises:
        StepError: git fails.
    """
    command = ["git", "submodule", "update", "--init", "--depth", "1"]
    if git.call([*command, "--", *paths]).returncode:
        shown = " ".join(paths)
        raise github.StepError(f"cannot check out {shown}")


def checked_out() -> list[pathlib.Path]:
    """Returns the checked-out submodules, nested ones after their parent."""
    listing = git.run("submodule", "foreach", "--quiet", "--recursive", "pwd")
    return [pathlib.Path(line) for line in listing.splitlines()]


def touch(submodule: pathlib.Path) -> None:
    """Sets the times of a submodule's files to its commit time.

    The submodule's .git is left alone; symbolic links get the time
    themselves, not their target.
    """
    commit_time = int(
        git.run("-C", str(submodule), "log", "-1", "--format=%ct")
    )
    times = (commit_time, commit_time)
    os.utime(submodule, times, follow_symlinks=False)
    for directory, subdirectories, names in os.walk(submodule):
        if pathlib.Path(directory) == submodule:
            subdirectories[:] = [
                name for name in subdirectories if name != ".git"
            ]
            names = [name for name in names if name != ".git"]
        for name in [*subdirectories, *names]:
            os.utime(
                os.path.join(directory, name), times, follow_symlinks=False
            )


def main(argv: Sequence[str] | None = None) -> int:
    """Checks out the submodules of a layer, and touches them if asked.

    Args:
        argv: The arguments without the program name: ``--layer LAYER`` and
            optionally ``--touch``.

    Returns:
        The process exit status: 0.

    Raises:
        StepError: git fails.
    """
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--layer",
        required=True,
        choices=layer_image.LAYERS,
        help="the image layer",
    )
    parser.add_argument(
        "--touch",
        action="store_true",
        help="give the files of each submodule its commit time",
    )
    args = parser.parse_args(argv)
    checkout(layer_image.submodules_of(args.layer))
    if args.touch:
        for submodule in checked_out():
            touch(submodule)
    return 0
