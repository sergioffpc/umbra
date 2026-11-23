# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
"""NOLINT suppressions that name their checks and give a reason.

Every NOLINT, NOLINTNEXTLINE and NOLINTBEGIN in C++ names each suppressed
check in full (no wildcard) and gives a reason after a colon; NOLINTEND names
the checks of its NOLINTBEGIN::

    // NOLINT(bugprone-narrowing-conversions): <reason>
    // NOLINTNEXTLINE(umbra-bool-parameter): <reason>

Usage: python3 -m checks.nolint [--staged] [PATH...]
"""

from collections.abc import Sequence
import dataclasses
import re
import sys

from checks import sources

DIRECTIVE = re.compile(
    r"\bNOLINT(?P<kind>NEXTLINE|BEGIN|END)?"
    r"(?:\((?P<checks>[^()]*)\))?"
    r"(?P<reason>:\s*\S.*)?"
)
# A full check name, for example readability-function-size or
# clang-analyzer-core.NullDereference.
CHECK = re.compile(r"[a-z][a-z0-9]*(?:-[A-Za-z0-9.]+)+")


@dataclasses.dataclass(frozen=True)
class Directive:
    """A NOLINT directive of a line.

    Attributes:
        kind: "", "NEXTLINE", "BEGIN" or "END".
        checks: The names inside the parentheses; empty when there are none.
        reason: The text after the colon; empty when there is none.
    """

    kind: str
    checks: tuple[str, ...]
    reason: str


def directives(line: str) -> list[Directive]:
    """Returns the NOLINT directives of a line."""
    found = []
    for match in DIRECTIVE.finditer(line):
        names = match.group("checks") or ""
        found.append(
            Directive(
                kind=match.group("kind") or "",
                checks=tuple(
                    name.strip() for name in names.split(",") if name.strip()
                ),
                reason=(match.group("reason") or "").lstrip(":").strip(),
            )
        )
    return found


def suppresses(line: str, previous_line: str, check: str) -> bool:
    """Returns whether a well-formed NOLINT suppresses a check on a line.

    Args:
        line: The line the diagnostic is on.
        previous_line: The line before it, for NOLINTNEXTLINE.
        check: The name of the check.

    Returns:
        Whether the line carries a NOLINT, or the previous line a
        NOLINTNEXTLINE, that names the check and gives a reason.
    """
    candidates = [(d, "") for d in directives(line)]
    candidates += [(d, "NEXTLINE") for d in directives(previous_line)]
    return any(
        directive.kind == kind
        and check in directive.checks
        and directive.reason
        for directive, kind in candidates
    )


def applies_to(path: str) -> bool:
    """Returns whether NOLINT directives are checked in the file."""
    return sources.is_cpp(path)


def directive_errors(directive: Directive) -> list[str]:
    """Returns what is wrong with a NOLINT directive."""
    name = f"NOLINT{directive.kind}"
    if not directive.checks:
        return [f"{name} must name the suppressed checks: {name}(<check>)"]
    errors = [
        f"{name}: '{check}' is not a full check name"
        for check in directive.checks
        if not CHECK.fullmatch(check)
    ]
    if directive.kind != "END" and not directive.reason:
        errors.append(f"{name} must give a reason: {name}(<check>): <reason>")
    return errors


def validate(path: str, text: str) -> list[str]:
    """Validates the NOLINT directives of a C++ file.

    Args:
        path: The file, relative to the repository root.
        text: The content of the file.

    Returns:
        The human-readable errors; empty when every directive is valid.
    """
    return [
        f"{path}:{number}: {error}"
        for number, line in enumerate(text.splitlines(), start=1)
        for directive in directives(line)
        for error in directive_errors(directive)
    ]


def main(argv: Sequence[str] | None = None) -> int:
    """Validates the NOLINT directives of the given files.

    Args:
        argv: The arguments without the program name; ``sys.argv[1:]`` when
            None.

    Returns:
        The process exit status: 0 when every file is valid, 1 otherwise.
    """
    return sources.validator_main("invalid NOLINT", applies_to, validate, argv)


if __name__ == "__main__":
    sys.exit(main())
