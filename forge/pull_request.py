# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
"""The branch name, title and commits of a pull request.

The pull request is that of the event, or on another event (a dispatched
run) the open pull request whose head is the branch of the run; without one,
the checks are skipped with a notice. The checks stop at the first that
fails:

- the branch name follows git flow; Dependabot's branches
  (dependabot/<ecosystem>/<dependency>) are exempt;
- the title is a Conventional Commit: features and bugfixes are
  squash-merged, and the title becomes the commit header on develop;
- every commit has a signature that GitHub verified, and is a Conventional
  Commit carrying its author's Developer Certificate of Origin sign-off; bots
  (Dependabot, the submodules workflow) are exempt from the sign-off. Merge
  commits are allowed only in release and hotfix pull requests into main
  (the git flow back-merges), and are not checked. A release or hotfix pull
  request into main checks only its commits that are not on develop: those
  on develop passed these checks in their own pull requests. GitHub lists at
  most MAX_COMMITS commits of a pull request, so one with more fails.

Needs the GitHub Actions variables GITHUB_EVENT_NAME, GITHUB_EVENT_PATH,
GITHUB_REF_NAME and GITHUB_REPOSITORY, and gh (authenticated by GH_TOKEN).

Usage: python3 -m forge pull-request
"""

import argparse
from collections.abc import Sequence
import dataclasses
import json
from typing import Any

from checks import branch_name
from checks import commit_message
from checks import report
from forge import github

# The pull request commits endpoint lists at most 250 commits.
MAX_COMMITS = 250
DEPENDABOT = "dependabot[bot]"


@dataclasses.dataclass(frozen=True)
class PullRequest:
    """A pull request.

    Attributes:
        number: Its number.
        title: Its title.
        base: The branch it merges into.
        head: The branch it merges.
        author: The login of its author.
    """

    number: int
    title: str
    base: str
    head: str
    author: str


def lookup() -> PullRequest | None:
    """Returns the pull request of the run, or None when there is none."""
    if github.variable("GITHUB_EVENT_NAME") == "pull_request":
        pull = github.event()["pull_request"]
        return PullRequest(
            pull["number"],
            pull["title"],
            pull["base"]["ref"],
            pull["head"]["ref"],
            pull["user"]["login"],
        )
    branch = github.variable("GITHUB_REF_NAME")
    found = json.loads(
        github.gh(
            "pr",
            "list",
            "--repo",
            github.variable("GITHUB_REPOSITORY"),
            "--head",
            branch,
            "--state",
            "open",
            "--limit",
            "1",
            "--json",
            "number,title,baseRefName,author",
        )
    )
    if not found:
        return None
    pull = found[0]
    return PullRequest(
        pull["number"],
        pull["title"],
        pull["baseRefName"],
        branch,
        pull["author"]["login"],
    )


def check_branch_name(pull: PullRequest) -> int:
    """Validates the head branch name, except Dependabot's."""
    if pull.author == DEPENDABOT:
        return 0
    errors = branch_name.validate(pull.head, pull_request=True)
    return report.fail("invalid branch name", errors)


def check_title(pull: PullRequest) -> int:
    """Validates the title as a commit message without a sign-off."""
    message = commit_message.clean(f"{pull.title}\n")
    errors = commit_message.validate(message, commit_message.allowed_scopes())
    return report.fail("invalid commit message", errors)


def commit_errors(
    commit: dict[str, Any], scopes: frozenset[str], merges_allowed: bool
) -> list[str]:
    """Returns the errors of one commit.

    Args:
        commit: The commit object of the GitHub API.
        scopes: The scopes a commit header may use.
        merges_allowed: Whether merge commits are allowed.

    Returns:
        The human-readable errors; empty when the commit is valid.
    """
    sha = commit["sha"][:12]
    message = commit["commit"]["message"]
    header = message.split("\n", 1)[0]
    errors = []
    verification = commit["commit"]["verification"]
    if not verification["verified"]:
        reason = verification["reason"]
        errors.append(f"signature not verified by GitHub ({reason})")
    if len(commit["parents"]) > 1:
        if not merges_allowed:
            errors.append("merge commits are not allowed; rebase instead")
    else:
        login = (commit.get("author") or {}).get("login") or ""
        author = None
        if not login.endswith("[bot]"):
            author = commit["commit"]["author"]["email"]
        errors.extend(
            commit_message.validate(message.strip(), scopes, author=author)
        )
    return [f"{sha} {header!r}: {error}" for error in errors]


def check_commits(pull: PullRequest) -> int:
    """Validates the commits of the pull request."""
    repository = github.variable("GITHUB_REPOSITORY")
    merges_allowed = pull.base == "main" and pull.head.startswith(
        ("release/", "hotfix/")
    )
    if merges_allowed:
        endpoint = f"repos/{repository}/compare/develop...{pull.head}"
        jq = ".commits[] | @json"
    else:
        endpoint = f"repos/{repository}/pulls/{pull.number}/commits"
        jq = ".[] | @json"
    lines = github.gh("api", "--paginate", endpoint, "--jq", jq)
    commits = [json.loads(line) for line in lines.splitlines() if line.strip()]
    scopes = commit_message.allowed_scopes()
    errors = []
    if len(commits) >= MAX_COMMITS:
        errors.append(
            f"the pull request has {len(commits)} commits or more; "
            f"only the first {MAX_COMMITS} can be verified"
        )
    for commit in commits:
        errors.extend(commit_errors(commit, scopes, merges_allowed))
    return report.fail("invalid pull request commits", errors)


def main(argv: Sequence[str] | None = None) -> int:
    """Checks the pull request of the run, if any.

    Args:
        argv: The arguments without the program name; there are none.

    Returns:
        The process exit status: 0 when every check passes or there is no
        pull request, 1 otherwise.
    """
    argparse.ArgumentParser(description=__doc__.splitlines()[0]).parse_args(
        argv
    )
    pull = lookup()
    if pull is None:
        branch = github.variable("GITHUB_REF_NAME")
        github.notice(
            f"{branch} has no open pull request:"
            " the branch name, title and commit checks are skipped",
            title="No pull request",
        )
        return 0
    print(f"Pull request #{pull.number}: {pull.title}", flush=True)
    for check in (check_branch_name, check_title, check_commits):
        status = check(pull)
        if status:
            return status
    return 0
