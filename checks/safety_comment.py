# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
"""SAFETY comments before unsafe regions.

An unsafe region opens with UMBRA_UNSAFE_BUFFER_BEGIN (or the pragma behind
it) or with a clang floating-point pragma that relaxes precision
(``#pragma clang fp`` or ``#pragma float_control``). The comment block right
above it must start with ``// SAFETY: <why the region is sound>``. Every
UMBRA_UNSAFE_BUFFER_BEGIN is closed by an UMBRA_UNSAFE_BUFFER_END in the same
file.

Usage: python3 -m checks.safety_comment [--staged] [PATH...]
"""

from collections.abc import Sequence
import re
import sys

from checks import sources

BUFFER_BEGIN = re.compile(
    r"\bUMBRA_UNSAFE_BUFFER_BEGIN\b|unsafe_buffer_usage\s+begin"
)
BUFFER_END = re.compile(
    r"\bUMBRA_UNSAFE_BUFFER_END\b|unsafe_buffer_usage\s+end"
)
FP_PRAGMA = re.compile(r"^\s*#\s*pragma\s+(?:clang\s+fp\b|float_control\b)")
DEFINE = re.compile(r"^\s*#\s*define\b")
SAFETY = re.compile(r"^\s*//\s*SAFETY:\s*\S")


def applies_to(path: str) -> bool:
    """Returns whether unsafe regions are checked in the file."""
    return sources.is_cpp(path)


def code(line: str) -> str:
    """Returns a line without its trailing // comment."""
    return line.split("//", 1)[0]


def has_safety_comment(lines: Sequence[str], index: int) -> bool:
    """Returns whether a SAFETY comment block ends right above a line.

    Args:
        lines: The lines of the file.
        index: The index of the line that opens the unsafe region.

    Returns:
        Whether the contiguous // comment block above the line starts with
        ``// SAFETY:`` and some text.
    """
    first = index
    while first > 0 and lines[first - 1].lstrip().startswith("//"):
        first -= 1
    return first < index and bool(SAFETY.match(lines[first]))


def validate(path: str, text: str) -> list[str]:
    """Validates the unsafe regions of a C++ file.

    Args:
        path: The file, relative to the repository root.
        text: The content of the file.

    Returns:
        The human-readable errors; empty when every region is justified and
        closed.
    """
    lines = text.splitlines()
    errors = []
    open_line = 0
    for index, line in enumerate(lines):
        if DEFINE.match(line):
            continue
        number = index + 1
        opens_buffer = bool(BUFFER_BEGIN.search(code(line)))
        if (opens_buffer or FP_PRAGMA.match(line)) and not has_safety_comment(
            lines, index
        ):
            errors.append(
                f"{path}:{number}: an unsafe region must be immediately "
                "preceded by a '// SAFETY: <reason>' comment"
            )
        if opens_buffer:
            if open_line:
                errors.append(
                    f"{path}:{number}: unsafe buffer region nested in the "
                    f"one opened on line {open_line}"
                )
            open_line = number
        elif BUFFER_END.search(code(line)):
            if not open_line:
                errors.append(
                    f"{path}:{number}: UMBRA_UNSAFE_BUFFER_END without a "
                    "matching UMBRA_UNSAFE_BUFFER_BEGIN"
                )
            open_line = 0
    if open_line:
        errors.append(
            f"{path}:{open_line}: unsafe buffer region never closed with "
            "UMBRA_UNSAFE_BUFFER_END"
        )
    return errors


def main(argv: Sequence[str] | None = None) -> int:
    """Validates the unsafe regions of the given files.

    Args:
        argv: The arguments without the program name; ``sys.argv[1:]`` when
            None.

    Returns:
        The process exit status: 0 when every file is valid, 1 otherwise.
    """
    return sources.validator_main(
        "unjustified unsafe region", applies_to, validate, argv
    )


if __name__ == "__main__":
    sys.exit(main())
