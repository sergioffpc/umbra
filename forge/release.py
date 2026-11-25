# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
"""The version, changelog and tags of a release or hotfix branch.

The version in the branch name (release/X.Y.Z or hotfix/X.Y.Z) must equal the
CMake project version and git-cliff's bumped version, must not be tagged
yet, and CHANGELOG.md must have a dated section for it. Every release tag
must be signed: only the presence of a signature is checked, since no
allowed-signers file is committed. The branch is --branch, or in GitHub
Actions the head of the pull request or the branch of the run. Needs the
whole history and the tags, and git-cliff.

Usage: python3 -m forge release [--branch NAME]
"""

import argparse
from collections.abc import Sequence
import datetime
import os
import re
import subprocess

from checks import git
from forge import github

# project(umbra ... VERSION X.Y.Z ...), the CMakeLists.txt joined on one line.
PROJECT_VERSION = re.compile(
    r".*project\(\s*umbra\s[^)]*VERSION\s+(?P<version>[0-9]+\.[0-9]+\.[0-9]+)"
)
DATE = re.compile(r"[0-9]{4}-[0-9]{2}-[0-9]{2}")
SIGNATURE = re.compile(r"^-----BEGIN (SSH|PGP) SIGNATURE-----$", re.MULTILINE)
RELEASE_TAGS = "v[0-9]*.[0-9]*.[0-9]*"


def branch_of_run() -> str:
    """Returns the head of the pull request, or the branch of the run.

    Raises:
        github.StepError: Neither GITHUB_HEAD_REF nor GITHUB_REF_NAME is set.
    """
    branch = os.environ.get("GITHUB_HEAD_REF") or os.environ.get(
        "GITHUB_REF_NAME"
    )
    if not branch:
        raise github.StepError(
            "give --branch, or run in a GitHub Actions job (GITHUB_HEAD_REF"
            " or GITHUB_REF_NAME)"
        )
    return branch


def project_version(cmake_lists: str) -> str:
    """Returns the project version that a CMakeLists.txt declares.

    Args:
        cmake_lists: The text of the CMakeLists.txt.

    Returns:
        The version, X.Y.Z, or an empty string when it declares none.
    """
    match = PROJECT_VERSION.match(cmake_lists.replace("\n", " "))
    return match["version"] if match else ""


def version_errors(version: str) -> list[str]:
    """Returns the errors of the CMake and git-cliff versions.

    Args:
        version: The version of the branch.
    """
    errors = []
    cmake_lists = git.REPO_ROOT / "CMakeLists.txt"
    cmake_version = ""
    if cmake_lists.is_file():
        cmake_version = project_version(cmake_lists.read_text(encoding="utf-8"))
    if not cmake_version:
        errors.append(
            "CMakeLists.txt declares no 'project(umbra VERSION X.Y.Z ...)'"
        )
    elif cmake_version != version:
        errors.append(
            f"the CMake project version is {cmake_version}, the branch says"
            f" {version}"
        )
    cliff = subprocess.run(
        ["git", "cliff", "--bumped-version"],
        cwd=git.REPO_ROOT,
        stdout=subprocess.PIPE,
        text=True,
        check=False,
    )
    bumped = cliff.stdout.strip().removeprefix("v")
    if cliff.returncode:
        errors.append("git cliff --bumped-version failed")
    elif bumped != version:
        errors.append(f"git-cliff bumps to {bumped}, the branch says {version}")
    tag = f"refs/tags/v{version}"
    if not git.call(
        ["git", "rev-parse", "-q", "--verify", tag], quiet=True
    ).returncode:
        errors.append(f"v{version} is already tagged")
    return errors


def is_valid_date(text: str) -> bool:
    """Returns whether the text is a calendar date, YYYY-MM-DD."""
    if not DATE.fullmatch(text):
        return False
    try:
        datetime.date.fromisoformat(text)
    except ValueError:
        return False
    return True


def changelog_errors(version: str) -> list[str]:
    """Returns the errors of the CHANGELOG.md section of a version."""
    heading = f"## [{version}] - "
    changelog = git.REPO_ROOT / "CHANGELOG.md"
    lines = []
    if changelog.is_file():
        lines = changelog.read_text(encoding="utf-8").splitlines()
    date = next((line[len(heading) :] for line in lines if heading in line), "")
    if not date:
        return [f"CHANGELOG.md has no '## [{version}] - YYYY-MM-DD' section"]
    if not is_valid_date(date):
        return [
            f"the CHANGELOG.md section of {version} has an invalid date:"
            f" '{date}'"
        ]
    return []


def signature_error(tag: str) -> str:
    """Returns why a release tag is not signed, or an empty string.

    Args:
        tag: The tag's name, which exists in the repository.
    """
    if git.run("cat-file", "-t", f"refs/tags/{tag}").strip() != "tag":
        return f"release tag {tag} is lightweight, so it is not signed"
    if not SIGNATURE.search(git.run("cat-file", "tag", f"refs/tags/{tag}")):
        return f"release tag {tag} is not signed"
    return ""


def tag_errors() -> list[str]:
    """Returns the errors of the release tags: each must be signed."""
    errors = []
    for tag in git.run("tag", "--list", RELEASE_TAGS).split():
        message = signature_error(tag)
        if message:
            errors.append(message)
    return errors


def main(argv: Sequence[str] | None = None) -> int:
    """Checks the version, the changelog and the tags of a release branch.

    Args:
        argv: The arguments without the program name: ``[--branch NAME]``.

    Returns:
        The process exit status: 0 when every check passes, 1 otherwise.
    """
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--branch",
        metavar="NAME",
        help="the release or hotfix branch; by default, that of the run",
    )
    branch = parser.parse_args(argv).branch or branch_of_run()
    version = branch.split("/", 1)[-1]
    print(f"Branch version: {version}", flush=True)
    errors = [
        *version_errors(version),
        *changelog_errors(version),
        *tag_errors(),
    ]
    for message in errors:
        github.error(message)
    return 1 if errors else 0
