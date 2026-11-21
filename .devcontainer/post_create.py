#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
"""Set up a newly created devcontainer.

Runs once, as the devcontainer's postCreateCommand, from the repository root:
installs the git hooks, enables commit and tag signing in the repository's
local configuration, configures the debug CMake preset, and installs Claude
Code with the project's plugins. Every step runs even when an earlier one
fails; the script fails at the end if any step did.

Usage: python3 .devcontainer/post_create.py
"""

from collections.abc import Callable, Sequence
import pathlib
import subprocess
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent

# Anthropic's native installer, and the release channel it installs.
CLAUDE_INSTALLER_URL = "https://claude.ai/install.sh"
CLAUDE_CHANNEL = "stable"
CLAUDE_BIN = pathlib.Path.home() / ".local" / "bin" / "claude"

# The plugin .claude/settings.json enables, and its marketplace. Enabling a
# plugin in project settings does not download it; installing it does.
PLUGIN_MARKETPLACE = "claude-plugins-official"
PLUGIN_MARKETPLACE_REPO = "anthropics/claude-plugins-official"
PLUGIN = f"mattpocock-skills@{PLUGIN_MARKETPLACE}"


def run(args: Sequence[str], stdin: bytes | None = None) -> str:
    """Runs a command from the repository root.

    Args:
        args: The command and its arguments.
        stdin: Bytes to feed to the command's standard input, if any.

    Returns:
        The command's standard output, decoded as UTF-8.

    Raises:
        subprocess.CalledProcessError: The command exited with a non-zero
            status.
    """
    print("+", " ".join(args), flush=True)
    result = subprocess.run(
        args,
        cwd=REPO_ROOT,
        input=stdin,
        stdout=subprocess.PIPE,
        check=True,
    )
    output = result.stdout.decode("utf-8")
    print(output, end="", flush=True)
    return output


def git_config(*args: str) -> None:
    """Sets a value in the repository's local git configuration.

    Args:
        *args: The arguments to ``git config --local``.
    """
    run(["git", "config", "--local", *args])


def install_hooks() -> None:
    """Points git at the hooks in .githooks."""
    git_config("core.hooksPath", ".githooks")


def enable_signing() -> None:
    """Signs every commit and tag with the forwarded SSH signing key.

    The key itself stays in the host's ssh-agent, which VS Code forwards
    together with the host's git configuration (and its user.signingkey).
    """
    git_config("gpg.format", "ssh")
    git_config("commit.gpgsign", "true")
    git_config("tag.gpgsign", "true")
    key = subprocess.run(
        ["git", "config", "--get", "user.signingkey"],
        cwd=REPO_ROOT,
        stdout=subprocess.PIPE,
        check=False,
    ).stdout.strip()
    if not key:
        print(
            "warning: user.signingkey is not set; set it in the host's git"
            " configuration to your SSH public key (key::ssh-ed25519 ...).",
            file=sys.stderr,
        )


def configure_debug_preset() -> None:
    """Configures the debug CMake preset, so that the IDE works at once."""
    if not (REPO_ROOT / "CMakePresets.json").is_file():
        print("No CMakePresets.json yet; skipping the debug preset.")
        return
    run(["cmake", "--preset", "debug"])


def install_claude() -> None:
    """Installs Claude Code and the plugins its project settings enable.

    The installer needs curl, which the GitHub CLI feature installs.
    """
    installer = subprocess.run(
        ["curl", "-fsSL", CLAUDE_INSTALLER_URL],
        stdout=subprocess.PIPE,
        check=True,
    ).stdout
    run(["bash", "-s", "--", CLAUDE_CHANNEL], stdin=installer)
    claude = str(CLAUDE_BIN)
    # The marketplace list lives in the Claude config volume, so it survives
    # a rebuild; adding it twice is an error.
    if PLUGIN_MARKETPLACE not in run([claude, "plugin", "marketplace", "list"]):
        run([claude, "plugin", "marketplace", "add", PLUGIN_MARKETPLACE_REPO])
    run([claude, "plugin", "install", PLUGIN, "--scope", "project"])


STEPS: Sequence[tuple[str, Callable[[], None]]] = (
    ("install the git hooks", install_hooks),
    ("enable commit and tag signing", enable_signing),
    ("configure the debug preset", configure_debug_preset),
    ("install Claude Code", install_claude),
)


def main() -> int:
    """Runs every set-up step.

    Returns:
        The exit status: 0 when every step succeeded, 1 otherwise.
    """
    failed = []
    for name, step in STEPS:
        print(f"==> {name}", flush=True)
        try:
            step()
        except (OSError, subprocess.CalledProcessError) as error:
            print(f"error: could not {name}: {error}", file=sys.stderr)
            failed.append(name)
    if failed:
        steps = "; ".join(failed)
        print(f"error: failed to {steps}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
