# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
"""The namespace matches the directory.

The C++ files of a module declare their code in the module's namespace:

- include/umbra/<module>/, src/<module>/, tests/<module>/ and
  benchmarks/<module>/: umbra::<module>;
- the same trees under ext/<dependency>/: umbra::ext::<dependency>.

Every named namespace opened at file scope is that namespace or one nested in
it. Anonymous namespaces are allowed. Files outside a module directory (apps/,
tests/main.cc, benchmarks/main.cc) are not checked.

Usage: python3 -m checks.namespace [--staged] [PATH...]
"""

from collections.abc import Sequence
import pathlib
import re
import sys

from checks import sources

MODULE_TREES = (("include", "umbra"), ("src",), ("tests",), ("benchmarks",))
# Comments, string and character literals, and preprocessor lines: blanked out
# before the namespaces and braces are read.
NOISE = re.compile(
    r"//[^\n]*"
    r"|/\*.*?\*/"
    r'|R"(?P<delimiter>[^(\s]*)\(.*?\)(?P=delimiter)"'
    r'|"(?:\\.|[^"\\\n])*"'
    r"|(?<!\w)'(?:\\.|[^'\\\n])+'"
    r"|^[ \t]*#(?:\\\n|[^\n])*",
    re.DOTALL | re.MULTILINE,
)
# A namespace definition or alias, or a brace.
TOKEN = re.compile(
    r"\b(?:inline\s+)?namespace\b(?P<name>[\s\w:]*?)\s*(?P<open>[{=])|[{}]"
)


def expected_namespace(path: str) -> str | None:
    """Returns the namespace of the module a file belongs to, if any.

    Args:
        path: The file, relative to the repository root.

    Returns:
        For example ``"umbra::core"`` for src/core/version.cc, or None when
        the file is not in a module directory.
    """
    parts = pathlib.PurePosixPath(path).parts
    for tree in MODULE_TREES:
        if parts[: len(tree)] != tree:
            continue
        module = parts[len(tree) : -1]
        if module[:1] == ("ext",):
            return "umbra::ext::" + module[1] if len(module) > 1 else None
        return "umbra::" + module[0] if module else None
    return None


def blank(match: re.Match[str]) -> str:
    """Replaces a match with spaces, keeping its newlines."""
    return re.sub(r"[^\n]", " ", match.group())


def file_scope_namespaces(text: str) -> list[tuple[int, str]]:
    """Returns the named namespaces a C++ file opens at file scope.

    Args:
        text: The content of the file.

    Returns:
        The line number and qualified name (``inline`` dropped) of each
        named namespace definition that no other named namespace encloses.
    """
    code = NOISE.sub(blank, text)
    found = []
    # One entry per open brace: whether it opened a named namespace.
    braces: list[bool] = []
    for token in TOKEN.finditer(code):
        if token.group() == "}":
            if braces:
                braces.pop()
        elif token.group("open") == "{":
            name = re.sub(r"\binline\b|\s", "", token.group("name"))
            if name and not any(braces):
                found.append((code.count("\n", 0, token.start()) + 1, name))
            braces.append(bool(name))
        elif token.group() == "{":
            braces.append(False)
    return found


def applies_to(path: str) -> bool:
    """Returns whether the file's namespaces are checked."""
    return sources.is_cpp(path) and expected_namespace(path) is not None


def validate(path: str, text: str) -> list[str]:
    """Validates the namespaces of a C++ file against its directory.

    Args:
        path: The file, relative to the repository root.
        text: The content of the file.

    Returns:
        The human-readable errors; empty when the namespaces match.
    """
    expected = expected_namespace(path)
    return [
        f"{path}:{number}: namespace {name} does not match the directory; "
        f"use {expected} (or a namespace nested in it)"
        for number, name in file_scope_namespaces(text)
        if name != expected and not name.startswith(f"{expected}::")
    ]


def main(argv: Sequence[str] | None = None) -> int:
    """Validates the namespaces of the given files.

    Args:
        argv: The arguments without the program name; ``sys.argv[1:]`` when
            None.

    Returns:
        The process exit status: 0 when every file is valid, 1 otherwise.
    """
    return sources.validator_main(
        "namespace does not match the directory", applies_to, validate, argv
    )


if __name__ == "__main__":
    sys.exit(main())
