# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
"""Pins the deps image in the devcontainer configuration of a branch.

The pin commit is created through the GraphQL API instead of pushed, so
GitHub signs it. It goes on top of the branch's current head and edits the
file as it is there. Commits made with the workflow's token start no workflow
runs, except workflow_dispatch, so the ci workflow is then dispatched on the
branch: its jobs report on the pin commit. When the branch already pins the
image, CI is dispatched all the same, since the image may have been missing
when it ran. Needs gh, authenticated with ``GH_TOKEN``.

Usage: python3 -m forge pin-devcontainer --ref IMAGE --tag TAG --branch NAME
"""

import argparse
import base64
from collections.abc import Sequence
import re

from forge import github
from forge import image

# The "image" line, one line at a time: its start, the image and its end.
IMAGE_LINE = re.compile(
    r'^(?P<start>[^\S\n]*"image":[^\S\n]*")(?P<ref>[^"\n]*)(?P<end>".*)$',
    re.MULTILINE,
)
CREATE_COMMIT = """
mutation($repository: String!, $branch: String!, $head: GitObjectID!,
         $path: String!, $contents: Base64String!, $headline: String!,
         $body: String!) {
  createCommitOnBranch(input: {
    branch: {repositoryNameWithOwner: $repository, branchName: $branch},
    expectedHeadOid: $head,
    message: {headline: $headline, body: $body},
    fileChanges: {additions: [{path: $path, contents: $contents}]}
  }) { commit { oid url } }
}
"""


def dispatch_ci(repository: str, branch: str) -> None:
    """Dispatches the ci workflow on a branch."""
    github.gh(
        "workflow", "run", "ci.yml", "--repo", repository, "--ref", branch
    )
    print(f"Dispatched the ci workflow on {branch}.")


def main(argv: Sequence[str] | None = None) -> int:
    """Pins the image on the branch, then dispatches CI on it.

    Args:
        argv: The arguments without the program name: ``--ref IMAGE --tag
            TAG --branch NAME``.

    Returns:
        The process exit status: 0 when the branch pins the image, 1 when
        the file has no image line.

    Raises:
        StepError: gh fails, or a GitHub Actions variable is unset.
    """
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--ref", required=True, metavar="IMAGE")
    parser.add_argument("--tag", required=True, help="the image's tag")
    parser.add_argument("--branch", required=True, metavar="NAME")
    args = parser.parse_args(argv)
    repository = github.variable("GITHUB_REPOSITORY")
    head = github.gh(
        "api",
        f"repos/{repository}/git/ref/heads/{args.branch}",
        "--jq",
        ".object.sha",
    ).strip()
    text = github.gh(
        "api",
        "-H",
        "Accept: application/vnd.github.raw+json",
        f"repos/{repository}/contents/{image.PIN_FILE}?ref={head}",
    )
    pinned = "\n".join(match["ref"] for match in IMAGE_LINE.finditer(text))
    if not pinned:
        github.error('no "image" line to pin', file=image.PIN_FILE)
        return 1
    if pinned == args.ref:
        print(f"{image.PIN_FILE} already pins {args.ref}.")
        dispatch_ci(repository, args.branch)
        return 0
    pin = IMAGE_LINE.sub(
        lambda line: line.group("start") + args.ref + line.group("end"),
        text,
    )
    sha = github.variable("GITHUB_SHA")
    print(
        github.gh(
            "api",
            "graphql",
            "-f",
            f"repository={repository}",
            "-f",
            f"branch={args.branch}",
            "-f",
            f"head={head}",
            "-f",
            f"path={image.PIN_FILE}",
            "-f",
            f"contents={base64.b64encode(pin.encode()).decode()}",
            "-f",
            f"headline=build(devcontainer): pin umbra-deps image {args.tag}",
            "-f",
            f"body=Image {args.ref}, built from {sha}.",
            "-f",
            f"query={CREATE_COMMIT}",
            "--jq",
            r'"Pinned \(.data.createCommitOnBranch.commit.url)"',
        ),
        end="",
    )
    dispatch_ci(repository, args.branch)
    return 0
