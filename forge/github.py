# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
"""GitHub Actions: annotations, step outputs, the event and the GitHub CLI.

Under GitHub Actions (``GITHUB_ACTIONS`` set), an annotation is a workflow
command printed to stdout, such as ``::error file=PATH::MESSAGE``; elsewhere
it is a plain ``error: MESSAGE`` line on stderr, so that the commands also run
outside CI. A step output is appended to the file named by ``GITHUB_OUTPUT``,
or printed as ``NAME=VALUE`` when there is none.
"""

from collections.abc import Mapping
import json
import os
import subprocess
import sys
from typing import Any

from checks import git


class StepError(Exception):
    """A step cannot run: a GitHub Actions variable or a tool is missing.

    A ``Call`` step reports it as an error and fails with exit status 1.
    """


def in_actions() -> bool:
    """Returns whether the process runs in a GitHub Actions job."""
    return bool(os.environ.get("GITHUB_ACTIONS"))


def escape_data(text: str) -> str:
    """Escapes the message of a workflow command."""
    return text.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")


def escape_property(text: str) -> str:
    """Escapes a property value of a workflow command."""
    return escape_data(text).replace(":", "%3A").replace(",", "%2C")


def annotate(
    level: str, message: str, properties: Mapping[str, str | None]
) -> None:
    """Prints an annotation.

    Args:
        level: ``error``, ``warning`` or ``notice``.
        message: The message.
        properties: The properties of the annotation (``file``, ``title``);
            those that are None are left out.
    """
    present = {name: value for name, value in properties.items() if value}
    if in_actions():
        fields = ",".join(
            f"{name}={escape_property(value)}"
            for name, value in present.items()
        )
        separator = " " if fields else ""
        print(f"::{level}{separator}{fields}::{escape_data(message)}")
        sys.stdout.flush()
        return
    prefix = "".join(f"{value}: " for value in present.values())
    print(f"{level}: {prefix}{message}", file=sys.stderr, flush=True)


def error(
    message: str, *, file: str | None = None, title: str | None = None
) -> None:
    """Prints an error annotation, on a file or with a title if given."""
    annotate("error", message, {"file": file, "title": title})


def notice(message: str, *, title: str | None = None) -> None:
    """Prints a notice annotation, with a title if given."""
    annotate("notice", message, {"title": title})


def set_output(name: str, value: str) -> None:
    """Sets an output of the current step.

    Args:
        name: The output's name.
        value: Its value, on one line.
    """
    path = os.environ.get("GITHUB_OUTPUT")
    if not path:
        print(f"{name}={value}", flush=True)
        return
    with open(path, "a", encoding="utf-8") as outputs:
        outputs.write(f"{name}={value}\n")


def variable(name: str) -> str:
    """Returns a variable that GitHub Actions sets.

    Args:
        name: The variable's name, for example ``GITHUB_REPOSITORY``.

    Raises:
        StepError: The variable is unset or empty.
    """
    value = os.environ.get(name)
    if not value:
        raise StepError(
            f"{name} is not set: this command runs in a GitHub Actions job"
        )
    return value


def event() -> dict[str, Any]:
    """Returns the payload of the event that started the workflow run.

    Raises:
        StepError: GITHUB_EVENT_PATH is unset.
    """
    path = variable("GITHUB_EVENT_PATH")
    with open(path, encoding="utf-8") as payload:
        return json.load(payload)


def gh(*args: str) -> str:
    """Runs the GitHub CLI and returns its standard output.

    Its standard error passes through. gh authenticates with ``GH_TOKEN``.

    Args:
        *args: The gh arguments, for example ``"api", "repos/..."``.

    Raises:
        StepError: gh cannot start or exits with a non-zero status.
    """
    command = ["gh", *args]
    try:
        result = subprocess.run(
            command,
            cwd=git.REPO_ROOT,
            stdout=subprocess.PIPE,
            text=True,
            check=False,
        )
    except OSError as error_:
        raise StepError(f"gh: {error_.strerror}") from error_
    if result.returncode:
        program = " ".join(command[:3])
        raise StepError(f"{program} exited with status {result.returncode}")
    return result.stdout
