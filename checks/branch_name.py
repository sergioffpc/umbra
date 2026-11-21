# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
"""git flow branch names.

- main and develop;
- feature/<name> and bugfix/<name>, where <name> is kebab-case;
- release/X.Y.Z and hotfix/X.Y.Z, where X.Y.Z is a SemVer core version.

Usage: python3 -m checks.branch_name NAME...
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


def validate(name: str) -> list[str]:
    """Validates a branch name.

    Args:
        name: The branch name, without the ``refs/heads/`` prefix.

    Returns:
        The human-readable errors; empty when the name is valid.
    """
    if BRANCH.match(name):
        return []
    return [
        f"branch {name!r} does not follow git flow: use main, develop, "
        "feature/<kebab-case>, bugfix/<kebab-case>, release/X.Y.Z or "
        "hotfix/X.Y.Z"
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
    parser.add_argument("names", nargs="+", metavar="NAME", help="branch name")
    args = parser.parse_args(argv)
    errors = [error for name in args.names for error in validate(name)]
    return report.fail("invalid branch name", errors)


if __name__ == "__main__":
    sys.exit(main())
