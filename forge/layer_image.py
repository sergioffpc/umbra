# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
"""The content-addressed image reference of a container image layer.

Every workflow that builds or pins a layer image computes its reference here,
so that they all agree on tags. The recipe::

    tag = first 16 hex digits of sha256 over
            "parent <parent image ref>\\n"     (layers with a parent only)
            + `git ls-tree -r HEAD -- <layer input paths>`
    ref = ghcr.io/<owner>/umbra-<layer>:<tag>

``git ls-tree`` lists the mode, object id and path of every tracked file
under the input paths, and the commit of every submodule among them. The tag
therefore changes exactly when a submodule commit, a patch, a dependency build
file or a Dockerfile of the layer (or its parent's tag) changes, and it is
computed from the commit alone, without checking out submodules.

A layer's inputs are: its Dockerfile, .dockerignore and any other file its
Dockerfile reads (e.g. docker/python-tools.txt), the dependency build files
every layer uses (third_party/CMakeLists.txt, third_party/cmake/), its layer
file and its layer directory of helpers only it uses (layers/<layer>/, if
any), and for each of its dependencies the submodule, its config/ file and
its patches/ directory. Nothing only another layer uses may be an input, or
that layer's changes would rebuild this one. To add a layer, add an entry to
LAYERS with its paths and its parent layer.

The step outputs are ``tag``, ``image`` (the reference without the tag),
``ref`` (``image:tag``) and ``submodules``, the space-separated paths of the
submodules among the layer's inputs, to check out before building it. Needs
git and GITHUB_REPOSITORY_OWNER.

Usage: python3 -m forge layer-image {toolchain,deps}
"""

import argparse
from collections.abc import Sequence
import dataclasses
import hashlib

from checks import git
from forge import github

# The dependency build files that every layer uses.
DEPENDENCY_BUILD = ("third_party/CMakeLists.txt", "third_party/cmake")
# Inputs that may be absent: a dependency without patches.
OPTIONAL_INPUTS = "third_party/patches/"
# The git mode of a submodule.
GITLINK_MODE = "160000"
TAG_LENGTH = 16


def dependency(name: str) -> tuple[str, ...]:
    """Returns the inputs of a dependency: submodule, config and patches."""
    return (
        f"third_party/{name}",
        f"third_party/config/{name}.cmake",
        f"third_party/patches/{name}",
    )


@dataclasses.dataclass(frozen=True)
class Layer:
    """A container image layer.

    Attributes:
        parent: The layer it is built on, or None.
        inputs: The paths whose content the image is built from.
    """

    parent: str | None
    inputs: tuple[str, ...]


LAYERS = {
    "toolchain": Layer(
        None,
        (
            "docker/toolchain.Dockerfile",
            "docker/toolchain.Dockerfile.dockerignore",
            "docker/python-tools.txt",
            *DEPENDENCY_BUILD,
            "third_party/layers/toolchain.cmake",
            *dependency("llvm-project"),
        ),
    ),
    "deps": Layer(
        "toolchain",
        (
            "docker/deps.Dockerfile",
            "docker/deps.Dockerfile.dockerignore",
            *DEPENDENCY_BUILD,
            "third_party/layers/deps.cmake",
            "third_party/layers/deps",
            *(
                path
                for name in (
                    "googletest",
                    "benchmark",
                    "oneTBB",
                    "embree",
                    "openqmc",
                    "Imath",
                    "openexr",
                    "OpenSubdiv",
                    "MaterialX",
                    "yaml-cpp",
                    "pystring",
                    "OpenColorIO",
                    "fmt",
                    "robin-map",
                    "libtiff",
                    "pugixml",
                    "ptex",
                    "OpenImageIO",
                    "openvdb",
                    "OpenShadingLanguage",
                    "OpenUSD",
                    "oidn",
                )
                for path in dependency(name)
            ),
        ),
    ),
}


def image_of(layer: str) -> str:
    """Returns the image of a layer, without its tag."""
    return github.package_image(f"umbra-{layer}")


def tag_of(layer: str) -> str:
    """Returns the content hash tag of a layer.

    Raises:
        StepError: An input of the layer is not tracked.
    """
    spec = LAYERS[layer]
    for path in spec.inputs:
        if path.startswith(OPTIONAL_INPUTS):
            continue
        if not git.run("ls-tree", "HEAD", "--", path):
            raise github.StepError(
                f"input '{path}' of layer '{layer}' is not tracked"
            )
    digest = hashlib.sha256()
    if spec.parent is not None:
        parent = f"{image_of(spec.parent)}:{tag_of(spec.parent)}"
        digest.update(f"parent {parent}\n".encode())
    digest.update(git.run("ls-tree", "-r", "HEAD", "--", *spec.inputs).encode())
    return digest.hexdigest()[:TAG_LENGTH]


def submodules_of(layer: str) -> list[str]:
    """Returns the paths of the submodules among the inputs of a layer."""
    listing = git.run("ls-tree", "HEAD", "--", *LAYERS[layer].inputs)
    paths = []
    for line in listing.splitlines():
        info, path = line.split("\t", 1)
        if info.split()[0] == GITLINK_MODE:
            paths.append(path)
    return paths


def main(argv: Sequence[str] | None = None) -> int:
    """Computes the reference of a layer image and writes the step outputs.

    Args:
        argv: The arguments without the program name: the layer.

    Returns:
        The process exit status: 0.

    Raises:
        StepError: An input of the layer is not tracked, or
            GITHUB_REPOSITORY_OWNER is unset.
    """
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("layer", choices=LAYERS, help="the image layer")
    layer = parser.parse_args(argv).layer
    tag = tag_of(layer)
    image = image_of(layer)
    github.set_output("tag", tag)
    github.set_output("image", image)
    github.set_output("ref", f"{image}:{tag}")
    github.set_output("submodules", " ".join(submodules_of(layer)))
    print(f"{layer} image: {image}:{tag}")
    return 0
