# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
"""git flow branch names.

- main and develop;
- feature/<name> and bugfix/<name>, where <name> is kebab-case;
- release/X.Y.Z and hotfix/X.Y.Z, where X.Y.Z is a SemVer core version.

Usage: python3 -m checks.branch_name NAME...
"""

import argparse
import re
import sys

from checks.report import fail

KEBAB_CASE = r"[a-z0-9]+(?:-[a-z0-9]+)*"
VERSION = r"(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)"
BRANCH = re.compile(
    rf"^(?:main|develop|(?:feature|bugfix)/{KEBAB_CASE}|(?:release|hotfix)/{VERSION})$"
)


def validate(name: str) -> list[str]:
    """Validate a branch name (without ``refs/heads/``); return the errors."""
    if BRANCH.match(name):
        return []
    return [
        f"branch {name!r} does not follow git flow: use main, develop, "
        "feature/<kebab-case>, bugfix/<kebab-case>, release/X.Y.Z or hotfix/X.Y.Z"
    ]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("names", nargs="+", metavar="NAME", help="branch name")
    args = parser.parse_args(argv)
    errors = [error for name in args.names for error in validate(name)]
    return fail("invalid branch name", errors)


if __name__ == "__main__":
    sys.exit(main())
