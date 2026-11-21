# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
"""Every header assumes pointers non-null.

A header wraps everything after its includes in UMBRA_ASSUME_NONNULL_BEGIN and
UMBRA_ASSUME_NONNULL_END, each once and on its own line, so that pointers are
non-null unless marked UMBRA_NULLABLE. The safety header, which defines the
macros, is the only exception.

Usage: python3 -m checks.assume_nonnull [--staged] [PATH...]
"""

from collections.abc import Sequence
import re
import sys

from checks import sources

# Defines the macros, and declares nothing a pointer could appear in.
SAFETY_HEADER = "include/umbra/core/safety.h"
BEGIN = "UMBRA_ASSUME_NONNULL_BEGIN"
END = "UMBRA_ASSUME_NONNULL_END"
INCLUDE = re.compile(r"^\s*#\s*include\b")


def applies_to(path: str) -> bool:
    """Returns whether the file is a header that must assume non-null."""
    return sources.is_header(path) and path != SAFETY_HEADER


def validate(path: str, text: str) -> list[str]:
    """Validates the assume-nonnull region of a header.

    Args:
        path: The header, relative to the repository root.
        text: The content of the header.

    Returns:
        The human-readable errors; empty when the region is valid.
    """
    lines = [line.strip() for line in text.splitlines()]
    begins = [number for number, line in enumerate(lines, 1) if line == BEGIN]
    ends = [number for number, line in enumerate(lines, 1) if line == END]
    if len(begins) != 1 or len(ends) != 1:
        return [
            f"{path}:1: a header has exactly one {BEGIN} line and one {END} "
            f"line (found {len(begins)} and {len(ends)})"
        ]
    begin, end = begins[0], ends[0]
    if end < begin:
        return [f"{path}:{end}: {END} comes before {BEGIN}"]
    return [
        f"{path}:{number}: #include inside the assume-nonnull region; "
        f"include before {BEGIN}"
        for number, line in enumerate(lines, 1)
        if begin < number < end and INCLUDE.match(line)
    ]


def main(argv: Sequence[str] | None = None) -> int:
    """Validates the assume-nonnull regions of the given headers.

    Args:
        argv: The arguments without the program name; ``sys.argv[1:]`` when
            None.

    Returns:
        The process exit status: 0 when every header is valid, 1 otherwise.
    """
    return sources.validator_main(
        "header without assume-nonnull", applies_to, validate, argv
    )


if __name__ == "__main__":
    sys.exit(main())
