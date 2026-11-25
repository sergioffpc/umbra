# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
"""The deps image pinned in the devcontainer configuration.

The ``"image"`` line of .devcontainer/devcontainer.json pins the deps image by
content hash. This command resolves it for the jobs that run in it, and fails
when it is not published yet. It writes the image to the step output ``ref``
and needs docker, logged in to the registry.

Usage: python3 -m forge image
"""

import argparse
from collections.abc import Sequence
import re

from checks import git
from forge import github

PIN_FILE = ".devcontainer/devcontainer.json"
IMAGE_LINE = re.compile(
    r'^[ \t]*"image":[ \t]*"(?P<ref>[^"\n]*)"', re.MULTILINE
)


def pinned_image() -> str:
    """Returns the pinned image, or an empty string when there is none."""
    text = (git.REPO_ROOT / PIN_FILE).read_text(encoding="utf-8")
    match = IMAGE_LINE.search(text)
    return match["ref"] if match else ""


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
    """Resolves the pinned image and writes it to the step output ``ref``.

    Args:
        argv: The arguments without the program name; there are none.

    Returns:
        The process exit status: 0 when the image is published, 1 otherwise.
    """
    argparse.ArgumentParser(description=__doc__.splitlines()[0]).parse_args(
        argv
    )
    ref = pinned_image()
    if not ref:
        github.error('no "image" line with the pinned image', file=PIN_FILE)
        return 1
    if not is_published(ref):
        github.error(
            f"{ref} is not published: run the deps workflow on this branch to"
            " build it, then re-run CI",
            title="Image missing",
        )
        return 1
    github.set_output("ref", ref)
    return 0
