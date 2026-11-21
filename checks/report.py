# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
"""Uniform error reporting for the hooks and CI."""

from collections.abc import Iterable
import sys


def fail(subject: str, errors: Iterable[str]) -> int:
    """Prints the errors about a subject to stderr.

    Callers can write ``return report.fail(...)`` from a ``main`` function.

    Args:
        subject: What was validated, for example ``"invalid branch name"``.
        errors: The human-readable errors; may be empty.

    Returns:
        The process exit status: 0 when there are no errors, 1 otherwise.
    """
    messages = list(errors)
    if not messages:
        return 0
    print(f"error: {subject}", file=sys.stderr)
    for error in messages:
        print(f"  - {error}", file=sys.stderr)
    print("See CONTRIBUTING.md for the project conventions.", file=sys.stderr)
    return 1


def exit_status(subject: str, command: str, status: int) -> int:
    """Reports a tool that exited with a non-zero status.

    Args:
        subject: What failed, for example ``"clang-tidy found problems"``.
        command: The command, as shown to the user, for example ``"pylint"``.
        status: The tool's exit status.

    Returns:
        The process exit status: 0 when the tool succeeded, 1 otherwise.
    """
    errors = [f"{command} exited with status {status}"] if status else []
    return fail(subject, errors)
