# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
"""Whether a build tree has the GPU module.

The image carries nvcc and the OptiX headers, so UMBRA_GPU (AUTO) builds the
GPU module, lib/libumbra-gpu.so; its tests need a GPU and run on GPU hosts
only.

Usage: python3 -m forge audit --preset (debug | release)
"""

import argparse
from collections.abc import Sequence

from checks import git
from forge import github

MODULE = "lib/libumbra-gpu.so"


def main(argv: Sequence[str] | None = None) -> int:
    """Checks that the build tree given on the command line has the module.

    Args:
        argv: The arguments without the program name: the build tree,
            relative to the repository root.

    Returns:
        The process exit status: 0 when the module was built, 1 otherwise.
    """
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("tree", metavar="TREE", help="the build tree")
    tree = parser.parse_args(argv).tree
    if not (git.REPO_ROOT / tree / MODULE).is_file():
        github.error("the GPU module was not built (UMBRA_GPU)")
        return 1
    print(f"{tree}/{MODULE} was built.")
    return 0
