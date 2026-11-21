# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
"""Thin helpers around the git command line."""

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
