# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
"""Whether a pull request changes more than documentation.

Documentation is the Markdown files and docs/. The files are those of the
whole pull request, renames counted by both names. The step output ``code``
is ``false`` when the pull request changes documentation only, and ``true``
otherwise and on every event without a pull request. Needs the GitHub
Actions variables GITHUB_EVENT_PATH and GITHUB_REPOSITORY, and gh
(authenticated by GH_TOKEN).

Usage: python3 -m forge changes
"""

import argparse
from collections.abc import Sequence

from forge import github


def is_documentation(path: str) -> bool:
    """Returns whether a changed file is documentation."""
    return path.startswith("docs/") or path.endswith(".md")


def changed_files(number: int) -> list[str]:
    """Returns the files a pull request changes, renames by both names."""
    repository = github.variable("GITHUB_REPOSITORY")
    files = github.gh(
        "api",
        "--paginate",
        f"repos/{repository}/pulls/{number}/files",
        "--jq",
        ".[] | .filename, (.previous_filename // empty)",
    )
    return files.rstrip("\n").split("\n")


def main(argv: Sequence[str] | None = None) -> int:
    """Writes whether the pull request changes code to the output ``code``.

    Args:
        argv: The arguments without the program name; there are none.

    Returns:
        The process exit status: 0.
    """
    argparse.ArgumentParser(description=__doc__.splitlines()[0]).parse_args(
        argv
    )
    number = github.event().get("pull_request", {}).get("number")
    if not number:
        github.set_output("code", "true")
        return 0
    code = False
    for path in changed_files(number):
        if not is_documentation(path):
            print(f"Not documentation: {path}")
            code = True
    if not code:
        print("Documentation only: the image and the build jobs are skipped.")
    github.set_output("code", "true" if code else "false")
    return 0
