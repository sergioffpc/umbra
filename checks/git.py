# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
"""Thin helpers around the git command line."""

import subprocess
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent


def run(*args: str, stdin: str | None = None) -> str:
    """Run ``git`` with ``args`` in the repository and return its stdout.

    Raises ``subprocess.CalledProcessError`` when git fails.
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
    """Return the value of git config ``key``, or ``default`` when unset."""
    try:
        value = run("config", "--get", key).strip()
    except subprocess.CalledProcessError:
        return default
    return value or default
