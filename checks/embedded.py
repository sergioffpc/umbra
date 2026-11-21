# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
"""Python embedded in the workflows' run steps.

CI-only logic stays inline in the workflows. Python is embedded there as a
quoted heredoc passed to ``python3 -``::

    run: |
      python3 - "$ARGUMENT" <<'EOF'
      ...
      EOF

The linter extracts every such script and checks it like a Python file:
formatting (pyink and isort) and pylint. YAML removes the run block's
indentation, so the script is what the shell sees, dedented.

Usage: python3 -m checks.embedded [PATH...]  (prints the scripts found)
"""

from collections.abc import Sequence
import dataclasses
import re
import sys
import textwrap

from checks import sources

# python3 - [ARG...] <<'TAG' (or <<"TAG"): a heredoc the shell does not
# expand, read by Python from standard input.
HEREDOC = re.compile(
    r"""\bpython3?\s+-(?:\s[^<]*)?<<-?\s*(['"])(?P<tag>\w+)\1\s*$"""
)


@dataclasses.dataclass(frozen=True)
class Script:
    """A Python script embedded in a workflow.

    Attributes:
        path: The workflow, relative to the repository root.
        line: The line of the workflow where the script starts (1-based).
        code: The script, dedented.
    """

    path: str
    line: int
    code: str

    @property
    def name(self) -> str:
        """Returns where the script is, for messages: ``<path>:<line>``."""
        return f"{self.path}:{self.line}"

    def aligned(self) -> str:
        """Returns the code shifted down to its lines in the workflow.

        Blank lines precede the code, so that the line numbers a tool reports
        are the line numbers of the workflow.
        """
        return "\n" * (self.line - 1) + self.code


def extract(path: str, text: str) -> list[Script]:
    """Returns the Python scripts embedded in a workflow.

    Args:
        path: The workflow, relative to the repository root.
        text: The content of the workflow.

    Returns:
        The scripts, in order. A heredoc without its closing tag ends at the
        end of the file.
    """
    lines = text.splitlines()
    scripts = []
    index = 0
    while index < len(lines):
        match = HEREDOC.search(lines[index])
        index += 1
        if match is None:
            continue
        start = index
        while index < len(lines) and lines[index].strip() != match["tag"]:
            index += 1
        code = textwrap.dedent("\n".join(lines[start:index]) + "\n")
        scripts.append(Script(path, start + 1, code))
    return scripts


def scripts_of(
    paths: Sequence[str], read: sources.Reader = sources.read_working_tree
) -> list[Script]:
    """Returns the Python scripts embedded in the workflows among the files.

    Args:
        paths: The candidate files, relative to the repository root.
        read: Reads the content of a file.

    Returns:
        The scripts of every workflow among the files.
    """
    return [
        script
        for path in paths
        if sources.is_workflow(path)
        for script in extract(path, read(path))
    ]


def main(argv: Sequence[str] | None = None) -> int:
    """Prints the Python scripts embedded in the given or all workflows.

    Args:
        argv: The file paths without the program name; ``sys.argv[1:]`` when
            None.

    Returns:
        The process exit status: always 0.
    """
    paths = list(sys.argv[1:] if argv is None else argv)
    for script in scripts_of(paths or sources.tracked()):
        print(f"# {script.name}")
        print(script.code)
    return 0


if __name__ == "__main__":
    sys.exit(main())
