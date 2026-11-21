# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
"""Conventional Commits 1.0.0 plus the project's commit message rules.

    <type>[(<scope>)][!]: <description>

    [body]

    [footers]

- type: one of TYPES;
- scope: optional; a module (a directory directly under src/) or one of
  FIXED_SCOPES;
- description: English (ASCII), imperative, starting with a lowercase letter,
  no trailing period;
- header: at most MAX_HEADER_LENGTH characters;
- body: separated from the header by a blank line.

Usage: python3 -m checks.commit_message [--allow-autosquash] FILE
       (FILE is ``-`` to read the message from stdin)
"""

import argparse
import re
import sys
from pathlib import Path

from checks.git import REPO_ROOT
from checks.report import fail

TYPES = (
    "feat",
    "fix",
    "perf",
    "refactor",
    "docs",
    "test",
    "build",
    "ci",
    "style",
    "chore",
    "revert",
)
FIXED_SCOPES = ("deps", "ci", "devcontainer", "lint")
MAX_HEADER_LENGTH = 72
AUTOSQUASH_PREFIXES = ("fixup! ", "squash! ", "amend! ")
SCISSORS = "------------------------ >8 ------------------------"

HEADER = re.compile(
    r"^(?P<type>[a-z]+)"
    r"(?:\((?P<scope>[^()]*)\))?"
    r"(?P<breaking>!)?"
    r": (?P<description>\S.*)$"
)

# Words that end like non-imperative verb forms but are imperative themselves.
IMPERATIVE_EXCEPTIONS = frozenset(
    {
        "bring",
        "embed",
        "ping",
        "ring",
        "shed",
        "sing",
        "spring",
        "string",
        "swing",
        "wing",
    }
)
# Endings of words that end in "s" without being a third-person form.
NON_THIRD_PERSON_S_ENDINGS = ("ss", "us", "is", "as", "os")


def module_scopes(root: Path = REPO_ROOT) -> frozenset[str]:
    """Return the module scopes: the directories directly under ``src/``."""
    src = root / "src"
    if not src.is_dir():
        return frozenset()
    return frozenset(entry.name for entry in src.iterdir() if entry.is_dir())


def allowed_scopes(root: Path = REPO_ROOT) -> frozenset[str]:
    """Return every scope a commit may use."""
    return frozenset(FIXED_SCOPES) | module_scopes(root)


def clean(raw: str, comment_char: str = "#") -> str:
    """Strip what git strips from a message file before committing.

    Drops everything from the scissors line on, comment lines, trailing
    whitespace, and leading and trailing blank lines.
    """
    lines = []
    for line in raw.splitlines():
        if line.startswith(comment_char) and SCISSORS in line:
            break
        if line.startswith(comment_char):
            continue
        lines.append(line.rstrip())
    return "\n".join(lines).strip("\n")


def is_autosquash(message: str) -> bool:
    """Return whether the message is a fixup!, squash! or amend! commit."""
    return message.startswith(AUTOSQUASH_PREFIXES)


def _is_imperative(word: str) -> bool:
    word = word.lower()
    if word in IMPERATIVE_EXCEPTIONS or word.endswith("eed"):
        return True
    if word.endswith(("ed", "ing")):
        return False
    return not (word.endswith("s") and not word.endswith(NON_THIRD_PERSON_S_ENDINGS))


def _validate_description(description: str) -> list[str]:
    errors = []
    if description[0].isupper():
        errors.append(f"description must start in lowercase: {description[0]!r}")
    if description.endswith("."):
        errors.append("description must not end with a period")
    first_word = re.match(r"[A-Za-z]*", description).group()
    if first_word and not _is_imperative(first_word):
        errors.append(
            f"description must use the imperative mood "
            f"(for example 'add', not 'added' or 'adds'): {first_word!r}"
        )
    return errors


def validate_header(header: str, scopes: frozenset[str]) -> list[str]:
    """Validate a commit header (also used for pull request titles)."""
    errors = []
    if len(header) > MAX_HEADER_LENGTH:
        errors.append(
            f"header is {len(header)} characters long; "
            f"the maximum is {MAX_HEADER_LENGTH}"
        )
    if not header.isascii():
        errors.append("header must be written in English, using ASCII characters only")
    match = HEADER.match(header)
    if match is None:
        errors.append(
            f"header must look like '<type>[(<scope>)][!]: <description>': {header!r}"
        )
        return errors
    if match["type"] not in TYPES:
        errors.append(f"unknown type {match['type']!r}; use one of: {', '.join(TYPES)}")
    scope = match["scope"]
    if scope is not None and scope not in scopes:
        errors.append(
            f"unknown scope {scope!r}; use a module name or one of: "
            f"{', '.join(sorted(scopes))}"
        )
    errors.extend(_validate_description(match["description"]))
    return errors


def validate(
    message: str,
    scopes: frozenset[str],
    *,
    allow_autosquash: bool = False,
) -> list[str]:
    """Validate a cleaned commit message; return the list of errors.

    fixup!, squash! and amend! commits are accepted only with
    ``allow_autosquash`` (local work before ``git rebase --autosquash``).
    """
    if not message.strip():
        return ["commit message is empty"]
    if is_autosquash(message):
        if allow_autosquash:
            return []
        return ["fixup!, squash! and amend! commits must be squashed before merging"]
    header, *rest = message.split("\n")
    errors = validate_header(header, scopes)
    if rest and rest[0].strip():
        errors.append("the header must be followed by a blank line")
    return errors


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("file", help="message file, or - for stdin")
    parser.add_argument(
        "--allow-autosquash",
        action="store_true",
        help="accept fixup!, squash! and amend! commits",
    )
    args = parser.parse_args(argv)
    if args.file == "-":
        raw = sys.stdin.read()
    else:
        raw = Path(args.file).read_text(encoding="utf-8")
    message = clean(raw)
    errors = validate(message, allowed_scopes(), allow_autosquash=args.allow_autosquash)
    return fail("invalid commit message", errors)


if __name__ == "__main__":
    sys.exit(main())
