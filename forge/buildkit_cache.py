# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
"""Moves a BuildKit cache mount in and out of a builder.

A Dockerfile builds in a cache mount (``RUN --mount=type=cache``), which
BuildKit keeps in its builder only. ``load`` copies a directory (a build tree
restored from the Actions cache, removed once loaded) or the files of an image
(the compiler cache of the previous run) into the cache mount;
``save`` copies the cache mount into a directory, or into an image that it
pushes. Each runs ``docker buildx build`` on a Dockerfile read from its
standard input.

A package names the image ``ghcr.io/<owner>/<package>:latest``, read and
written by every branch. Loading from a package that is not published yet
loads nothing: the build starts cold. A pushed image has one manifest, without
provenance or SBOM manifests, and the source label of the repository. Needs
docker with buildx, logged in to the registry for a package.

Usage: python3 -m forge buildkit-cache {load,save} --builder NAME --id ID
       --target DIR (--from-dir DIR | --from-package NAME | --to-dir DIR |
       --to-package NAME)
"""

import argparse
from collections.abc import Sequence
import shutil
import subprocess
import sys
import tempfile

from forge import github
from forge import published

# The image that copies the files in and out of the cache mount.
BASE = "ubuntu:26.04"
# The tag of a package image.
PACKAGE_TAG = "latest"


def package_ref(package: str) -> str:
    """Returns the image of a cache package."""
    return f"{github.package_image(package)}:{PACKAGE_TAG}"


def cache_mount(cache_id: str, target: str) -> str:
    """Returns the mount option of the cache mount."""
    return f"--mount=type=cache,id={cache_id},target={target},sharing=locked"


# Every stage runs its RUN instructions with bash, errexit, nounset and
# pipefail.
SHELL = (
    'SHELL ["/bin/bash", "-o", "errexit", "-o", "nounset", "-o", "pipefail",'
    ' "-c"]\n'
)


def load_dockerfile(cache_id: str, target: str, source: str) -> str:
    """Returns the Dockerfile that copies a bind mount into the cache mount.

    Args:
        cache_id: The id of the cache mount.
        target: Where the Dockerfile mounts it.
        source: The options of the bind mount, after ``type=bind``.
    """
    return (
        f"FROM {BASE}\n"
        f"{SHELL}"
        f"RUN {cache_mount(cache_id, target)} \\\n"
        f"  --mount=type=bind{source},target=/restore \\\n"
        f"  cp -a /restore/. {target}/\n"
    )


def save_dockerfile(cache_id: str, target: str, label: str) -> str:
    """Returns the Dockerfile whose image holds the files of the cache mount.

    Args:
        cache_id: The id of the cache mount.
        target: Where the Dockerfile mounts it.
        label: Lines added to the final stage, such as a LABEL.
    """
    return (
        f"FROM {BASE} AS export\n"
        f"{SHELL}"
        f"RUN {cache_mount(cache_id, target)} \\\n"
        f"  mkdir /export \\\n"
        f"  && cp -a {target}/. /export/\n"
        "FROM scratch\n"
        f"{label}"
        "COPY --from=export /export/ /\n"
    )


def buildx(builder: str, options: Sequence[str], dockerfile: str) -> int:
    """Builds a Dockerfile given on standard input.

    Args:
        builder: The buildx builder.
        options: The options and the context directory.
        dockerfile: The Dockerfile.

    Returns:
        The exit status of docker.

    Raises:
        StepError: docker cannot start.
    """
    command = ["docker", "buildx", "build", "--builder", builder, "--file", "-"]
    sys.stdout.flush()
    try:
        return subprocess.run(
            [*command, *options], input=dockerfile, text=True, check=False
        ).returncode
    except OSError as error:
        raise github.StepError(f"docker: {error.strerror}") from error


def load(args: argparse.Namespace) -> int:
    """Copies a directory or a package image into the cache mount."""
    if args.from_dir is not None:
        dockerfile = load_dockerfile(args.id, args.target, "")
        status = buildx(args.builder, [args.from_dir], dockerfile)
        if not status:
            shutil.rmtree(args.from_dir)
        return status
    ref = package_ref(args.from_package)
    if not published.is_published(ref):
        print("No compiler cache yet: the build starts cold.")
        return 0
    dockerfile = load_dockerfile(args.id, args.target, f",from={ref}")
    with tempfile.TemporaryDirectory() as empty:
        return buildx(args.builder, [empty], dockerfile)


def save(args: argparse.Namespace) -> int:
    """Copies the cache mount into a directory or a package image."""
    with tempfile.TemporaryDirectory() as empty:
        if args.to_dir is not None:
            shutil.rmtree(args.to_dir, ignore_errors=True)
            dockerfile = save_dockerfile(args.id, args.target, "")
            output = f"type=local,dest={args.to_dir}"
            return buildx(args.builder, ["--output", output, empty], dockerfile)
        source = github.variable("GITHUB_SERVER_URL")
        repository = github.variable("GITHUB_REPOSITORY")
        label = f"LABEL org.opencontainers.image.source={source}/{repository}\n"
        dockerfile = save_dockerfile(args.id, args.target, label)
        output = f"type=image,name={package_ref(args.to_package)},push=true"
        options = ["--provenance=false", "--sbom=false", "--output", output]
        return buildx(args.builder, [*options, empty], dockerfile)


def parser() -> argparse.ArgumentParser:
    """Returns the parser of the command line."""
    root = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    root.add_argument("action", choices=("load", "save"))
    root.add_argument("--builder", required=True, metavar="NAME")
    root.add_argument("--id", required=True, help="the cache mount's id")
    root.add_argument(
        "--target", required=True, metavar="DIR", help="where it is mounted"
    )
    where = root.add_mutually_exclusive_group(required=True)
    where.add_argument("--from-dir", metavar="DIR")
    where.add_argument("--from-package", metavar="NAME")
    where.add_argument("--to-dir", metavar="DIR")
    where.add_argument("--to-package", metavar="NAME")
    return root


def main(argv: Sequence[str] | None = None) -> int:
    """Loads or saves a cache mount.

    Args:
        argv: The arguments without the program name.

    Returns:
        The process exit status: that of docker, or 2 when the source or
        destination does not match the action.

    Raises:
        StepError: docker cannot start, or a GitHub Actions variable that
            a package needs is unset.
    """
    root = parser()
    args = root.parse_args(argv)
    loading = args.action == "load"
    if loading != (args.from_dir is not None or args.from_package is not None):
        root.error(
            "load takes --from-dir or --from-package, save --to-dir or"
            " --to-package"
        )
    return load(args) if loading else save(args)
