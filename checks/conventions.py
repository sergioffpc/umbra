# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
"""Runs every file validator.

The validators check SPDX headers, the NOLINT format, SAFETY comments,
assume-nonnull headers and namespaces matching their directory. This module
validates the given files, all tracked files when none is given, or with
--staged the staged content of the staged files (the pre-commit hook).

Usage: python3 -m checks.conventions [--staged] [PATH...]
"""

from collections.abc import Sequence
import sys

from checks import assume_nonnull
from checks import namespace
from checks import nolint
from checks import report
from checks import safety_comment
from checks import sources
from checks import spdx

# (subject of the errors, validator module)
VALIDATORS = (
    ("missing SPDX header", spdx),
    ("invalid NOLINT", nolint),
    ("unjustified unsafe region", safety_comment),
    ("header without assume-nonnull", assume_nonnull),
    ("namespace does not match the directory", namespace),
)


def run(paths: Sequence[str], read: sources.Reader) -> int:
    """Runs every validator over the files.

    Args:
        paths: The files, relative to the repository root.
        read: Reads the content of a file.

    Returns:
        The process exit status: 0 when every file is valid, 1 otherwise.
    """
    status = 0
    for subject, validator in VALIDATORS:
        errors = sources.validate_files(
            paths, validator.applies_to, validator.validate, read
        )
        status = report.fail(subject, errors) or status
    return status


def main(argv: Sequence[str] | None = None) -> int:
    """Validates the files given on the command line.

    Args:
        argv: The arguments without the program name; ``sys.argv[1:]`` when
            None.

    Returns:
        The process exit status: 0 when every file is valid, 1 otherwise.
    """
    paths, read = sources.parse_file_arguments(argv)
    return run(paths, read)


if __name__ == "__main__":
    sys.exit(main())
