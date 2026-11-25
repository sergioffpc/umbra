# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
"""Whether an image is published in its registry.

Writes the step output ``exists``, ``true`` or ``false``, so that the steps
that build the image run only when it is missing. Needs docker, logged in to
the registry.

Usage: python3 -m forge published REF
"""

import argparse
from collections.abc import Sequence

from checks import git
from forge import github


def is_published(ref: str) -> bool:
    """Returns whether the registry has the image ``ref``."""
    try:
        status = git.call(
            ["docker", "manifest", "inspect", ref], capture=True
        ).returncode
    except OSError:
        return False
    return status == 0


def main(argv: Sequence[str] | None = None) -> int:
    """Tells whether the image is published in the step output ``exists``.

    Args:
        argv: The arguments without the program name: the image.

    Returns:
        The process exit status: 0.
    """
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("ref", metavar="REF", help="the image reference")
    ref = parser.parse_args(argv).ref
    exists = is_published(ref)
    if exists:
        print(f"{ref} is already published; nothing to build.")
    github.set_output("exists", "true" if exists else "false")
    return 0
