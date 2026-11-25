# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
"""Opens a pull request per submodule with a newer upstream release tag.

Each pull request moves a submodule and its third_party/versions.md row
together (see ``third_party``). Release tags are the tags shaped like the
pinned one: the same prefix, then X.Y.Z, so pre-releases are ignored; a
pinned tag of another shape is skipped with a warning. The branch is
``feature/bump-<dependency>-<tag>``, and a dependency whose branch exists is
skipped.

The commit is created on the checked-out base through the REST Git database
API, which can record a submodule commit, with no author or committer, so
GitHub signs it. The pull requests carry a label, created when missing.
Branches and pull requests created with the workflow's token start no
workflow runs: the deps workflow is dispatched on the branch by hand, which
builds and pins the image and then dispatches CI on it. Needs git, access to
the upstream repositories and gh, authenticated with ``GH_TOKEN``.

Usage: python3 -m forge bump-submodules --base BRANCH --label NAME
"""

import argparse
from collections.abc import Sequence
import dataclasses
import json
import pathlib
import re

from checks import git
from forge import github
from forge import third_party

# The version part of a release tag, after its prefix.
VERSION = re.compile(r"^([0-9]+)\.([0-9]+)\.([0-9]+)$")
LABEL_COLOR = "0366d6"
LABEL_DESCRIPTION = "Dependency updates"


def split_tag(tag: str) -> tuple[str, tuple[int, ...] | None]:
    """Splits a tag into its prefix, up to the first digit, and its version.

    Returns:
        The prefix, and the version as numbers, or None when the rest of
        the tag is not X.Y.Z.
    """
    digit = re.search(r"[0-9]", tag)
    prefix = tag[: digit.start()] if digit else tag
    match = VERSION.match(tag[len(prefix) :])
    return prefix, tuple(map(int, match.groups())) if match else None


def kebab(text: str) -> str:
    """Returns text lowercased, each run of other characters a hyphen."""
    return re.sub(r"[^a-z0-9]+", "-", text.lower()).strip("-")


def latest_release(url: str, prefix: str) -> tuple[str, tuple[int, ...]]:
    """Returns the newest release tag with a prefix upstream.

    Returns:
        The tag and its version; an empty tag when there is none.

    Raises:
        StepError: The tags of the repository cannot be listed.
    """
    result = git.call(
        ["git", "ls-remote", "--tags", "--refs", url, f"refs/tags/{prefix}*"],
        capture=True,
    )
    if result.returncode:
        print(result.stderr, end="")
        raise github.StepError(f"the tags of {url} cannot be listed")
    best: tuple[str, tuple[int, ...]] = ("", ())
    for line in result.stdout.splitlines():
        tag = line.partition("\t")[2].removeprefix("refs/tags/")
        if not tag.startswith(prefix):
            continue
        match = VERSION.match(tag[len(prefix) :])
        if match:
            version = tuple(map(int, match.groups()))
            best = max(best, (tag, version), key=lambda item: item[1])
    return best


def branch_exists(repository: str, branch: str) -> bool:
    """Returns whether the repository has a branch."""
    refs = json.loads(
        github.gh("api", f"repos/{repository}/git/matching-refs/heads/{branch}")
    )
    return any(ref["ref"] == f"refs/heads/{branch}" for ref in refs)


def ensure_label(repository: str, label: str) -> None:
    """Creates the label of the pull requests when the repository lacks it."""
    if git.call(
        ["gh", "api", f"repos/{repository}/labels/{label}"], capture=True
    ).returncode:
        github.gh(
            "label",
            "create",
            label,
            "--repo",
            repository,
            "--color",
            LABEL_COLOR,
            "--description",
            LABEL_DESCRIPTION,
        )


def bumped_manifest(entry: third_party.Entry, tag: str, commit: str) -> str:
    """Returns the manifest with the row of an entry moved to a tag."""
    start = f"| {entry.name} | `{entry.path}` |"
    lines = (git.REPO_ROOT / third_party.MANIFEST).read_text(encoding="utf-8")
    return "".join(
        (
            f"{start} `{tag}` | `{commit}` | `{entry.license}` |"
            if line.startswith(start)
            else line
        )
        + "\n"
        for line in lines.splitlines()
    )


@dataclasses.dataclass(frozen=True)
class Base:
    """Where the bump pull requests go.

    Attributes:
        repository: The repository, ``owner/name``.
        branch: The base branch, checked out.
        label: The label of the pull requests.
        commit: The commit of the checkout, the parent of each bump.
        tree: Its tree.
    """

    repository: str
    branch: str
    label: str
    commit: str
    tree: str


def create_commit(
    base: Base, entry: third_party.Entry, tag: str, commit: str, message: str
) -> str:
    """Creates the bump commit of an entry, unsigned by any author.

    Returns:
        The new commit.
    """
    manifest = bumped_manifest(entry, tag, commit)
    tree = {
        "base_tree": base.tree,
        "tree": [
            {
                "path": entry.path,
                "mode": "160000",
                "type": "commit",
                "sha": commit,
            },
            {
                "path": third_party.MANIFEST,
                "mode": "100644",
                "type": "blob",
                "content": manifest,
            },
        ],
    }
    tree_sha = github.gh(
        "api",
        f"repos/{base.repository}/git/trees",
        "--input",
        "-",
        "--jq",
        ".sha",
        stdin=json.dumps(tree),
    ).strip()
    # No author or committer: GitHub then signs the commit.
    payload = {"message": message, "tree": tree_sha, "parents": [base.commit]}
    return github.gh(
        "api",
        f"repos/{base.repository}/git/commits",
        "--input",
        "-",
        "--jq",
        ".sha",
        stdin=json.dumps(payload),
    ).strip()


def bump(base: Base, entry: third_party.Entry, url: str) -> None:
    """Opens the bump pull request of an entry, if there is a newer tag.

    Raises:
        StepError: The upstream tags cannot be listed, or gh fails.
    """
    prefix, version = split_tag(entry.tag)
    if version is None:
        github.warning(
            f"{entry.path}: {entry.tag} is not shaped PREFIX X.Y.Z; skipped",
            file=third_party.MANIFEST,
        )
        return
    latest, latest_version = latest_release(url, prefix)
    if not latest or latest_version <= version:
        print(f"{entry.name}: {entry.tag} is the newest release tag.")
        return
    dependency = pathlib.PurePosixPath(entry.path).name
    branch = f"feature/bump-{kebab(dependency)}-{kebab(latest)}"
    if branch_exists(base.repository, branch):
        print(f"{entry.name}: {branch} already exists.")
        return
    upstream = third_party.upstream_commit(url, latest)
    if not upstream:
        raise github.StepError(f"{entry.path}: tag {latest} has no commit")
    print(f"{entry.name}: {entry.tag} -> {latest} ({upstream}) on {branch}")
    header = f"build(deps): bump {dependency} to {latest}"
    body = (
        f"Moves {entry.name} from {entry.tag} ({entry.commit}) to the release"
        f" tag {latest} ({upstream})."
    )
    commit = create_commit(base, entry, latest, upstream, f"{header}\n\n{body}")
    github.gh(
        "api",
        f"repos/{base.repository}/git/refs",
        "-f",
        f"ref=refs/heads/{branch}",
        "-f",
        f"sha={commit}",
    )
    print(
        github.gh(
            "pr",
            "create",
            "--repo",
            base.repository,
            "--base",
            base.branch,
            "--head",
            branch,
            "--title",
            header,
            "--label",
            base.label,
            "--body",
            f"{body}\n\nUpstream: {url}\n\nPull requests opened by this"
            " workflow start no workflow runs: run the\ndeps workflow on"
            f" `{branch}` to build and pin the image; it then\ndispatches CI"
            " on the branch.",
        ),
        end="",
    )


def main(argv: Sequence[str] | None = None) -> int:
    """Opens the bump pull requests.

    Args:
        argv: The arguments without the program name: ``--base BRANCH
            --label NAME``.

    Returns:
        The process exit status: 0.

    Raises:
        StepError: A submodule has no URL, upstream tags cannot be listed,
            gh fails, or GITHUB_REPOSITORY is unset.
    """
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--base", required=True, metavar="BRANCH", help="the base branch"
    )
    parser.add_argument(
        "--label", required=True, metavar="NAME", help="the label of the PRs"
    )
    args = parser.parse_args(argv)
    base = Base(
        repository=github.variable("GITHUB_REPOSITORY"),
        branch=args.base,
        label=args.label,
        commit=git.run("rev-parse", "HEAD").strip(),
        tree=git.run("rev-parse", "HEAD^{tree}").strip(),
    )
    ensure_label(base.repository, base.label)
    entries, _ = third_party.read_manifest()
    urls = third_party.submodule_urls()
    for entry in entries:
        url = urls.get(entry.path)
        if not url:
            raise github.StepError(f"{entry.path} has no URL in .gitmodules")
        bump(base, entry, url)
    return 0
