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

With --since BASE, the commits of BASE..HEAD are validated instead, as CI
validates those of a pull request: merge commits are skipped, and nothing is
validated when BASE does not exist (a shallow clone, for example).

Usage: python3 -m checks.commit_message [--allow-autosquash] FILE
       (FILE is ``-`` to read the message from stdin)
       python3 -m checks.commit_message [--allow-autosquash] --since BASE
"""

import argparse
from collections.abc import Sequence
import pathlib
import re
import subprocess
import sys

from checks import git
from checks import report

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


def module_scopes(root: pathlib.Path = git.REPO_ROOT) -> frozenset[str]:
    """Returns the module scopes.

    Args:
        root: The repository root.

    Returns:
        The names of the directories directly under ``src/``.
    """
    src = root / "src"
    if not src.is_dir():
        return frozenset()
    return frozenset(entry.name for entry in src.iterdir() if entry.is_dir())


def allowed_scopes(root: pathlib.Path = git.REPO_ROOT) -> frozenset[str]:
    """Returns every scope a commit may use.

    Args:
        root: The repository root.

    Returns:
        FIXED_SCOPES plus the module scopes.
    """
    return frozenset(FIXED_SCOPES) | module_scopes(root)


def clean(raw: str, comment_char: str = "#") -> str:
    """Strips what git strips from a message file before committing.

    Drops everything from the scissors line on, comment lines, trailing
    whitespace, and leading and trailing blank lines.

    Args:
        raw: The contents of the message file.
        comment_char: The comment prefix (git's ``core.commentChar``).

    Returns:
        The message as git records it.
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
    """Returns whether the message is a fixup!, squash! or amend! commit."""
    return message.startswith(AUTOSQUASH_PREFIXES)


def _is_imperative(word: str) -> bool:
    """Returns whether a word looks like an imperative verb.

    Rejects past (``-ed``), progressive (``-ing``) and third-person (``-s``)
    forms, apart from the known exceptions.
    """
    word = word.lower()
    if word in IMPERATIVE_EXCEPTIONS or word.endswith("eed"):
        return True
    if word.endswith(("ed", "ing")):
        return False
    return not (
        word.endswith("s") and not word.endswith(NON_THIRD_PERSON_S_ENDINGS)
    )


def _validate_description(description: str) -> list[str]:
    """Returns the errors in the description part of a header."""
    errors = []
    if description[0].isupper():
        errors.append(
            f"description must start in lowercase: {description[0]!r}"
        )
    if description.endswith("."):
        errors.append("description must not end with a period")
    first_word = re.match(r"[A-Za-z]*", description).group()
    if first_word and not _is_imperative(first_word):
        errors.append(
            "description must use the imperative mood "
            f"(for example 'add', not 'added' or 'adds'): {first_word!r}"
        )
    return errors


def validate_header(header: str, scopes: frozenset[str]) -> list[str]:
    """Validates a commit header; also used for pull request titles.

    Args:
        header: The first line of the commit message.
        scopes: The scopes the header may use.

    Returns:
        The human-readable errors; empty when the header is valid.
    """
    errors = []
    if len(header) > MAX_HEADER_LENGTH:
        errors.append(
            f"header is {len(header)} characters long; "
            f"the maximum is {MAX_HEADER_LENGTH}"
        )
    if not header.isascii():
        errors.append(
            "header must be written in English, using ASCII characters only"
        )
    match = HEADER.match(header)
    if match is None:
        errors.append(
            "header must look like '<type>[(<scope>)][!]: <description>': "
            f"{header!r}"
        )
        return errors
    commit_type = match["type"]
    if commit_type not in TYPES:
        types = ", ".join(TYPES)
        errors.append(f"unknown type {commit_type!r}; use one of: {types}")
    scope = match["scope"]
    if scope is not None and scope not in scopes:
        names = ", ".join(sorted(scopes))
        errors.append(
            f"unknown scope {scope!r}; use a module name or one of: {names}"
        )
    errors.extend(_validate_description(match["description"]))
    return errors


def validate(
    message: str,
    scopes: frozenset[str],
    *,
    allow_autosquash: bool = False,
) -> list[str]:
    """Validates a cleaned commit message.

    Args:
        message: The message, as returned by ``clean``.
        scopes: The scopes the header may use.
        allow_autosquash: Whether to accept fixup!, squash! and amend!
            commits (local work before ``git rebase --autosquash``).

    Returns:
        The human-readable errors; empty when the message is valid.
    """
    if not message.strip():
        return ["commit message is empty"]
    if is_autosquash(message):
        if allow_autosquash:
            return []
        return [
            "fixup!, squash! and amend! commits must be squashed before "
            "merging"
        ]
    header, *rest = message.split("\n")
    errors = validate_header(header, scopes)
    if rest and rest[0].strip():
        errors.append("the header must be followed by a blank line")
    return errors


def validate_history(
    base: str, scopes: frozenset[str], *, allow_autosquash: bool = False
) -> list[str]:
    """Validates the messages of the commits of BASE..HEAD.

    Args:
        base: The revision the commits are based on, for example
            ``develop``.
        scopes: The scopes a header may use.
        allow_autosquash: Whether to accept fixup!, squash! and amend!
            commits.

    Returns:
        The human-readable errors, each prefixed with the commit; empty when
        every message is valid or when ``base`` does not exist.
    """
    try:
        git.run("rev-parse", "--verify", "--quiet", f"{base}^{{commit}}")
    except subprocess.CalledProcessError:
        print(f"{base} does not exist: no commits to validate", file=sys.stderr)
        return []
    shas = git.run("rev-list", "--no-merges", f"{base}..HEAD").split()
    errors = []
    for sha in shas:
        message = git.run("log", "-1", "--format=%B", sha).strip()
        header = message.split("\n", 1)[0]
        errors.extend(
            f"{sha[:12]} {header!r}: {error}"
            for error in validate(
                message, scopes, allow_autosquash=allow_autosquash
            )
        )
    return errors


def main(argv: Sequence[str] | None = None) -> int:
    """Validates the commit message in the file given on the command line.

    Args:
        argv: The arguments without the program name; ``sys.argv[1:]`` when
            None.

    Returns:
        The process exit status: 0 when the message is valid, 1 otherwise.
    """
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("file", nargs="?", help="message file, or - for stdin")
    source.add_argument(
        "--since",
        metavar="BASE",
        help="validate the commits of BASE..HEAD instead",
    )
    parser.add_argument(
        "--allow-autosquash",
        action="store_true",
        help="accept fixup!, squash! and amend! commits",
    )
    args = parser.parse_args(argv)
    if args.since is not None:
        errors = validate_history(
            args.since,
            allowed_scopes(),
            allow_autosquash=args.allow_autosquash,
        )
        return report.fail("invalid commit messages", errors)
    if args.file == "-":
        raw = sys.stdin.read()
    else:
        raw = pathlib.Path(args.file).read_text(encoding="utf-8")
    message = clean(raw)
    errors = validate(
        message, allowed_scopes(), allow_autosquash=args.allow_autosquash
    )
    return report.fail("invalid commit message", errors)


if __name__ == "__main__":
    sys.exit(main())
