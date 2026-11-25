# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
"""The contract of the release image, and that umbra starts in it.

The release image (docker/release.Dockerfile) runs umbra as user and group
65532, from /work, with ``--help`` as its default command. Its file system
holds umbra's tree under /opt/umbra and the Ubuntu libraries it needs, with
no shell, no package manager and no other program.

The contract is read from the image configuration and from the file system of
a container created from it. umbra then runs with no network: with no
argument, ``version --build-options``, ``devices`` (where the GPU module must
load, though without a GPU it finds no device) and ``render``. It needs
docker, with the image loaded.

Usage: python3 -m forge release-image IMAGE
"""

import argparse
from collections.abc import Sequence
import json
import re
import subprocess
import sys
import tarfile

from forge import github

UID = 65532
USER = f"{UID}:{UID}"
CONFIG = {
    "User": USER,
    "Entrypoint": ["/opt/umbra/bin/umbra"],
    "Cmd": ["--help"],
    "WorkingDir": "/work",
}
# The directories the user owns: the working directory and its home.
OWNED = ("work", "home/umbra")
REQUIRED = (
    "opt/umbra/bin/umbra",
    "opt/umbra/lib/libumbra-gpu.so",
    "opt/umbra/share/licenses/umbra/LICENSE",
    "etc/passwd",
    "etc/group",
)
PROGRAM = re.compile(r"^(usr/)?s?bin/.")
SHELL_OR_PACKAGE_MANAGER = re.compile(
    r"(^|/)(sh|bash|dash|busybox|apt|apt-get|dpkg)$"
    r"|^(etc/apt|var/lib/dpkg)(/|$)"
)
# What umbra devices prints when the GPU module does not load.
GPU_MODULE_ERROR = re.compile(
    r"cannot load |is not installed|has no entry point|has ABI version"
)


def docker(*args: str) -> str:
    """Runs docker and returns its standard output.

    Raises:
        StepError: docker cannot start or exits with a non-zero status.
    """
    try:
        result = subprocess.run(
            ["docker", *args], stdout=subprocess.PIPE, text=True, check=False
        )
    except OSError as error:
        raise github.StepError(f"docker: {error.strerror}") from error
    if result.returncode:
        raise github.StepError(
            f"docker {args[0]} exited with status {result.returncode}"
        )
    return result.stdout


def config_errors(image: str) -> list[str]:
    """Returns how the image configuration differs from the contract."""
    config = json.loads(
        docker("image", "inspect", "--format", "{{json .Config}}", image)
    )
    return [
        f"{name} is {json.dumps(config.get(name))}, not {json.dumps(want)}"
        for name, want in CONFIG.items()
        if config.get(name) != want
    ]


def file_system(image: str) -> dict[str, tarfile.TarInfo]:
    """Returns the entries of the file system of the image, by path.

    Paths are relative to the root, without a trailing slash.
    """
    container = docker("create", image).strip()
    try:
        with subprocess.Popen(
            ["docker", "export", container], stdout=subprocess.PIPE
        ) as export:
            with tarfile.open(fileobj=export.stdout, mode="r|") as archive:
                entries = {
                    member.name.rstrip("/"): member for member in archive
                }
        if export.returncode:
            raise github.StepError(
                f"docker export exited with status {export.returncode}"
            )
    finally:
        docker("rm", container)
    return entries


def file_system_errors(entries: dict[str, tarfile.TarInfo]) -> list[str]:
    """Returns how the image's file system breaks the contract."""
    errors = []
    programs = sorted(path for path in entries if PROGRAM.match(path))
    if programs:
        errors.append("the image has programs: " + " ".join(programs))
    others = sorted(
        path for path in entries if SHELL_OR_PACKAGE_MANAGER.search(path)
    )
    if others:
        errors.append(
            "the image has a shell or a package manager: " + " ".join(others)
        )
    errors.extend(
        f"the image has no /{path}" for path in REQUIRED if path not in entries
    )
    for path in OWNED:
        entry = entries.get(path)
        if entry is None:
            errors.append(f"the image has no /{path}")
        elif not entry.isdir() or (entry.uid, entry.gid) != (UID, UID):
            errors.append(
                f"/{path} is not a directory owned by {USER}"
                f" ({entry.uid}:{entry.gid})"
            )
    return errors


def shell_runs(image: str) -> bool:
    """Returns whether /bin/sh runs in the image."""
    return (
        subprocess.run(
            ["docker", "run", "--rm", "--entrypoint", "/bin/sh", image],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        ).returncode
        == 0
    )


def umbra(image: str, *args: str) -> str | None:
    """Runs umbra in the image with no network, echoing its output.

    Returns:
        Its standard output, or None when it fails.
    """
    command = ["docker", "run", "--rm", "--network", "none", image, *args]
    shown = " ".join(["umbra", *args])
    print(f"+ {shown}", file=sys.stderr, flush=True)
    result = subprocess.run(
        command, stdout=subprocess.PIPE, text=True, check=False
    )
    sys.stdout.write(result.stdout)
    sys.stdout.flush()
    if result.returncode:
        github.error(f"{shown} exited with status {result.returncode}")
        return None
    return result.stdout


def starts(image: str) -> bool:
    """Returns whether umbra starts in the image and loads the GPU module."""
    for args in ((), ("version", "--build-options")):
        if umbra(image, *args) is None:
            return False
    devices = umbra(image, "devices")
    if devices is None:
        return False
    if GPU_MODULE_ERROR.search(devices):
        github.error("the GPU module does not load in the image")
        return False
    return umbra(image, "render") is not None


def main(argv: Sequence[str] | None = None) -> int:
    """Checks the contract of the release image, then starts umbra in it.

    Args:
        argv: The arguments without the program name: the image.

    Returns:
        The process exit status: 0 when the image keeps the contract and
        umbra starts, 1 otherwise.
    """
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("image", help="the image, loaded in docker")
    image = parser.parse_args(argv).image
    errors = config_errors(image) + file_system_errors(file_system(image))
    if shell_runs(image):
        errors.append("/bin/sh runs in the image")
    for message in errors:
        github.error(message)
    if errors:
        return 1
    return 0 if starts(image) else 1
