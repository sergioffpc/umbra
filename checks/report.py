# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
"""Uniform error reporting for the hooks and CI."""

import sys
from collections.abc import Iterable


def fail(subject: str, errors: Iterable[str]) -> int:
    """Print the errors about ``subject`` to stderr and return an exit status.

    Returns 0 when there are no errors and 1 otherwise, so callers can write
    ``return fail(...)``.
    """
    errors = list(errors)
    if not errors:
        return 0
    print(f"error: {subject}", file=sys.stderr)
    for error in errors:
        print(f"  - {error}", file=sys.stderr)
    print("See CONTRIBUTING.md for the project conventions.", file=sys.stderr)
    return 1
