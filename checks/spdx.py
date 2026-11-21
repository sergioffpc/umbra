# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
"""SPDX license headers.

Every source file (code, build and tool configuration; see
``sources.is_source``) identifies its license as Apache-2.0 in a comment
within its first lines: ``SPDX-License-Identifier: Apache-2.0``.

Usage: python3 -m checks.spdx [--staged] [PATH...]
"""

from collections.abc import Sequence
import re
import sys

from checks import sources

# The header may follow a shebang and a file-type comment.
HEAD_LINES = 5
HEADER = re.compile(r"SPDX-License-Identifier: Apache-2\.0(?![\w.+-])")


def applies_to(path: str) -> bool:
    """Returns whether the file must carry an SPDX header."""
    return sources.is_source(path)


def validate(path: str, text: str) -> list[str]:
    """Validates the SPDX header of a file.

    Args:
        path: The file, relative to the repository root.
        text: The content of the file.

    Returns:
        The human-readable errors; empty when the header is present.
    """
    head = text.splitlines()[:HEAD_LINES]
    if any(HEADER.search(line) for line in head):
        return []
    return [
        f"{path}:1: no 'SPDX-License-Identifier: Apache-2.0' comment in the "
        f"first {HEAD_LINES} lines"
    ]


def main(argv: Sequence[str] | None = None) -> int:
    """Validates the SPDX headers of the given files.

    Args:
        argv: The arguments without the program name; ``sys.argv[1:]`` when
            None.

    Returns:
        The process exit status: 0 when every file is valid, 1 otherwise.
    """
    return sources.validator_main(
        "missing SPDX header", applies_to, validate, argv
    )


if __name__ == "__main__":
    sys.exit(main())
