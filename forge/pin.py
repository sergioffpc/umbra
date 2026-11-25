# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
"""The devcontainer must pin the deps image built from the current inputs.

The deps workflow builds that image and commits the pin; the layer-image
action computes its reference from the inputs, which is given as --ref.

Usage: python3 -m forge pin --ref IMAGE
"""

import argparse
from collections.abc import Sequence

from forge import github
from forge import image


def main(argv: Sequence[str] | None = None) -> int:
    """Checks that the devcontainer pins the given image.

    Args:
        argv: The arguments without the program name: ``--ref IMAGE``.

    Returns:
        The process exit status: 0 when the pin is the image, 1 otherwise.
    """
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--ref",
        required=True,
        metavar="IMAGE",
        help="the deps image that the current inputs hash to",
    )
    ref = parser.parse_args(argv).ref
    pinned = image.pinned_image()
    if pinned != ref:
        shown = pinned or "none"
        github.error(
            f"the pinned image '{shown}' is stale; the deps inputs"
            f" hash to {ref}. Let the deps workflow publish and pin it (or run"
            " it: gh workflow run deps.yml --ref <branch>), then pull.",
            file=image.PIN_FILE,
        )
        return 1
    print(f"{image.PIN_FILE} pins {ref}.")
    return 0
