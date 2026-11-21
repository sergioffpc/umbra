# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
"""Thin helpers to run git and the other tools from the repository root."""

from collections.abc import Sequence
import pathlib
import subprocess

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent


def run(*args: str, stdin: str | None = None) -> str:
    """Runs git in the repository and returns its standard output.

    Args:
        *args: The git arguments, for example ``"config", "--get", key``.
        stdin: Text written to git's standard input, if any.

    Returns:
        The standard output of git, decoded as text.

    Raises:
        subprocess.CalledProcessError: git exited with a non-zero status.
    """
    result = subprocess.run(
        ["git", *args],
        cwd=REPO_ROOT,
        input=stdin,
        capture_output=True,
        text=True,
        check=True,
    )
    return result.stdout


def config(key: str, default: str) -> str:
    """Reads a git configuration value.

    Args:
        key: The configuration key, for example ``core.commentChar``.
        default: The value returned when the key is unset or empty.

    Returns:
        The configured value, or ``default``.
    """
    try:
        value = run("config", "--get", key).strip()
    except subprocess.CalledProcessError:
        return default
    return value or default


def call(
    args: Sequence[str],
    *,
    stdin: str | None = None,
    capture: bool = False,
    quiet: bool = False,
) -> subprocess.CompletedProcess[str]:
    """Runs a command from the repository root; a failure does not raise.

    Args:
        args: The program and its arguments.
        stdin: Text written to the command's standard input, if any.
        capture: Whether to capture the standard output and error instead of
            passing them through.
        quiet: Whether to discard the standard output (ignored when
            capturing).

    Returns:
        The finished process; its ``returncode`` is the exit status.
    """
    if capture:
        output = {"stdout": subprocess.PIPE, "stderr": subprocess.PIPE}
    else:
        output = {"stdout": subprocess.DEVNULL if quiet else None}
    return subprocess.run(
        args, cwd=REPO_ROOT, input=stdin, text=True, check=False, **output
    )
