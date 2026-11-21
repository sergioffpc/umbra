# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
"""git flow branch names.

- main and develop;
- feature/<name> and bugfix/<name>, where <name> is kebab-case;
- release/X.Y.Z and hotfix/X.Y.Z, where X.Y.Z is a SemVer core version.

Disposable research/<name> and prototype/<name> branches may be pushed but
never opened as pull requests; --pull-request rejects them.

Usage: python3 -m checks.branch_name [--pull-request] NAME...
"""

import argparse
from collections.abc import Sequence
import re
import sys

from checks import report

KEBAB_CASE = r"[a-z0-9]+(?:-[a-z0-9]+)*"
VERSION = r"(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)"
BRANCH = re.compile(
    r"^(?:main|develop"
    rf"|(?:feature|bugfix)/{KEBAB_CASE}"
    rf"|(?:release|hotfix)/{VERSION})$"
)
DISPOSABLE_BRANCH = re.compile(rf"^(?:research|prototype)/{KEBAB_CASE}$")


def validate(name: str, pull_request: bool = False) -> list[str]:
    """Validates a branch name.

    Args:
        name: The branch name, without the ``refs/heads/`` prefix.
        pull_request: Whether the branch heads a pull request, which rules
            out the disposable research and prototype branches.

    Returns:
        The human-readable errors; empty when the name is valid.
    """
    if BRANCH.match(name):
        return []
    if DISPOSABLE_BRANCH.match(name):
        if not pull_request:
            return []
        return [
            f"branch {name!r} is disposable: research and prototype "
            "branches are never opened as pull requests"
        ]
    return [
        f"branch {name!r} does not follow git flow: use main, develop, "
        "feature/<kebab-case>, bugfix/<kebab-case>, release/X.Y.Z or "
        "hotfix/X.Y.Z (research/<kebab-case> and prototype/<kebab-case> "
        "may be pushed but not opened as pull requests)"
    ]


def main(argv: Sequence[str] | None = None) -> int:
    """Validates the branch names given on the command line.

    Args:
        argv: The arguments without the program name; ``sys.argv[1:]`` when
            None.

    Returns:
        The process exit status: 0 when every name is valid, 1 otherwise.
    """
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--pull-request",
        action="store_true",
        help="reject disposable research and prototype branches",
    )
    parser.add_argument("names", nargs="+", metavar="NAME", help="branch name")
    args = parser.parse_args(argv)
    errors = [
        error
        for name in args.names
        for error in validate(name, args.pull_request)
    ]
    return report.fail("invalid branch name", errors)


if __name__ == "__main__":
    sys.exit(main())
