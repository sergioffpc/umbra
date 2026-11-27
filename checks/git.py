# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
"""Thin helpers to run git and the other tools from the repository root.

The root is the working tree git runs in, so that a hook run from a git
worktree checks that worktree, even when ``core.hooksPath`` names the hooks
of another checkout.
"""

from collections.abc import Sequence
import pathlib
import subprocess

# The checkout this package was loaded from: where its own files live.
PACKAGE_ROOT = pathlib.Path(__file__).resolve().parent.parent


def checked_root() -> pathlib.Path:
    """The working tree the checks run on.

    It is the top level of the working tree git runs in, which for a hook is
    the worktree being committed, whichever checkout the hook file itself
    lives in. Outside a checkout of this repository, it is the checkout this
    package was loaded from.

    Returns:
        The absolute path of the working tree's top level.
    """
    try:
        top = subprocess.run(
            ["git", "rev-parse", "--show-toplevel"],
            capture_output=True,
            text=True,
            check=True,
        ).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return PACKAGE_ROOT
    root = pathlib.Path(top).resolve() if top else PACKAGE_ROOT
    if not (root / "checks" / "git.py").is_file():
        return PACKAGE_ROOT
    return root


# The working tree the checks and commands run on, whose files they read and
# whose build tree they use.
REPO_ROOT = checked_root()


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
