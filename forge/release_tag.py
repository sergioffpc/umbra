# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
"""The release tag that publishes the release image, and the image's tags.

The tag must be named vX.Y.Z, be annotated and signed (only the presence of
a signature is checked, since no allowed-signers file is committed), point
to a commit on main, and the CMakeLists.txt of that commit must declare the
version X.Y.Z. The tag is --tag, or in GitHub Actions the tag of the run.
Needs the whole history, the tags and origin/main.

On success it writes the step outputs ``version`` (X.Y.Z), ``image`` (the
image's repository, ghcr.io/<owner>/<repository> in lowercase, from
GITHUB_REPOSITORY) and ``tags``, the image with its tags X.Y.Z, X.Y, X and
latest, separated by commas.

Usage: python3 -m forge release-tag [--tag NAME]
"""

import argparse
from collections.abc import Sequence
import os
import re

from checks import git
from forge import github
from forge import release

# A release tag: vX.Y.Z, without leading zeros (Semantic Versioning).
RELEASE_TAG = re.compile(
    r"v(?P<major>0|[1-9][0-9]*)\.(?P<minor>0|[1-9][0-9]*)"
    r"\.(?P<patch>0|[1-9][0-9]*)"
)
MAIN = "refs/remotes/origin/main"
REGISTRY = "ghcr.io"


def tag_of_run() -> str:
    """Returns the tag of the GitHub Actions run.

    Raises:
        github.StepError: The run is not for a tag.
    """
    if os.environ.get("GITHUB_REF_TYPE") != "tag":
        raise github.StepError(
            "give --tag, or run in a GitHub Actions job for a tag push"
            " (GITHUB_REF_TYPE and GITHUB_REF_NAME)"
        )
    return github.variable("GITHUB_REF_NAME")


def image_tags(image: str, match: re.Match[str]) -> list[str]:
    """Returns the image with each of its tags: X.Y.Z, X.Y, X and latest.

    Args:
        image: The image's repository.
        match: The release tag, matched by RELEASE_TAG.
    """
    major, minor, patch = match["major"], match["minor"], match["patch"]
    tags = (f"{major}.{minor}.{patch}", f"{major}.{minor}", major, "latest")
    return [f"{image}:{tag}" for tag in tags]


def commit_errors(tag: str, version: str) -> list[str]:
    """Returns the errors of the commit of a release tag.

    Args:
        tag: The tag's name, which exists in the repository.
        version: The tag's version, X.Y.Z.
    """
    errors = []
    commit = git.run("rev-parse", f"refs/tags/{tag}^{{commit}}").strip()
    if git.call(
        ["git", "rev-parse", "-q", "--verify", MAIN], quiet=True
    ).returncode:
        errors.append("origin/main is not fetched: check out the whole history")
    elif git.call(
        ["git", "merge-base", "--is-ancestor", commit, MAIN]
    ).returncode:
        errors.append(f"{tag} points to {commit}, which is not on main")
    cmake_lists = git.call(
        ["git", "show", f"{commit}:CMakeLists.txt"], capture=True
    )
    cmake_version = release.project_version(cmake_lists.stdout)
    if cmake_version != version:
        errors.append(
            f"the CMake project version of {tag} is"
            f" '{cmake_version}', not {version}"
        )
    return errors


def main(argv: Sequence[str] | None = None) -> int:
    """Checks the release tag and writes the image and its tags.

    Args:
        argv: The arguments without the program name: ``[--tag NAME]``.

    Returns:
        The process exit status: 0 when the tag can publish the release
        image, 1 otherwise.
    """
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--tag",
        metavar="NAME",
        help="the release tag, vX.Y.Z; by default, the tag of the run",
    )
    tag = parser.parse_args(argv).tag or tag_of_run()
    print(f"Release tag: {tag}", flush=True)
    match = RELEASE_TAG.fullmatch(tag)
    if not match:
        github.error(f"{tag} is not a release tag, vX.Y.Z")
        return 1
    if git.call(
        ["git", "rev-parse", "-q", "--verify", f"refs/tags/{tag}"], quiet=True
    ).returncode:
        github.error(f"{tag} is not fetched: check out the tags")
        return 1
    version = tag.removeprefix("v")
    signature = release.signature_error(tag)
    errors = [*([signature] if signature else []), *commit_errors(tag, version)]
    for message in errors:
        github.error(message)
    if errors:
        return 1
    repository = github.variable("GITHUB_REPOSITORY").lower()
    image = f"{REGISTRY}/{repository}"
    github.set_output("version", version)
    github.set_output("image", image)
    github.set_output("tags", ",".join(image_tags(image, match)))
    return 0
