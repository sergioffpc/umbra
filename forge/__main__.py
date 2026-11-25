# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
"""The forge command line.

Every step is echoed to stderr before it runs, and the command stops at the
first step that fails, with its exit status.

Usage: python3 -m forge COMMAND [OPTION...]
"""

# Python fixes the name of a package's entry point, which is not a valid
# module name.
# pylint: disable=invalid-name

import argparse
from collections.abc import Sequence
import sys

from forge import commands
from forge import layer_image


def option_parsers() -> dict[commands.Option, argparse.ArgumentParser]:
    """Returns a parent parser for each option a command may take."""
    preset = argparse.ArgumentParser(add_help=False)
    preset.add_argument(
        "--preset",
        choices=[preset.value for preset in commands.BUILD_PRESETS],
        default=commands.Preset.DEBUG.value,
        help="the CMake preset: debug (the default) or release (LTO)",
    )
    audit_preset = argparse.ArgumentParser(add_help=False)
    audit_preset.add_argument(
        "--preset",
        choices=[preset.value for preset in commands.AUDIT_PRESETS],
        default=commands.Preset.DEBUG.value,
        help="the build tree, build/PRESET: debug (the default), release, asan"
        " or tsan",
    )
    label = argparse.ArgumentParser(add_help=False)
    label.add_argument(
        "--label",
        metavar="REGEX",
        help="run only the tests with a matching CTest label, e.g. gpu",
    )
    verbose = argparse.ArgumentParser(add_help=False)
    verbose.add_argument(
        "-v",
        "--verbose",
        action="store_true",
        help=(
            "print as much as each tool can: the configure's messages down"
            " to TRACE, every command the build runs, and the full output of"
            " every test"
        ),
    )
    check = argparse.ArgumentParser(add_help=False)
    check.add_argument(
        "--check",
        action="store_true",
        help="change no file: fail when one is not formatted",
    )
    ref = argparse.ArgumentParser(add_help=False)
    ref.add_argument(
        "--ref",
        required=True,
        metavar="IMAGE",
        help="the deps image that the current inputs hash to",
    )
    branch = argparse.ArgumentParser(add_help=False)
    branch.add_argument(
        "--branch",
        metavar="NAME",
        help="the release or hotfix branch; by default, that of the GitHub"
        " Actions run",
    )
    image = argparse.ArgumentParser(add_help=False)
    image.add_argument(
        "image",
        metavar="IMAGE",
        help="the image, in docker or in its registry",
    )
    tag = argparse.ArgumentParser(add_help=False)
    tag.add_argument(
        "--tag",
        metavar="NAME",
        help="the release tag, vX.Y.Z; by default, the tag of the GitHub"
        " Actions run",
    )
    output = argparse.ArgumentParser(add_help=False)
    output.add_argument(
        "--output", required=True, metavar="FILE", help="the file to write"
    )
    pushed = argparse.ArgumentParser(add_help=False)
    pushed.add_argument(
        "pushed",
        metavar="IMAGE@DIGEST",
        help="the image in its registry, by digest",
    )
    tags = argparse.ArgumentParser(add_help=False)
    tags.add_argument(
        "tags",
        metavar="TAGS",
        help="the references to create, comma-separated, e.g."
        " ghcr.io/o/umbra:1.2.3,ghcr.io/o/umbra:latest",
    )
    layer = argparse.ArgumentParser(add_help=False)
    layer.add_argument(
        "layer", choices=layer_image.LAYERS, help="the image layer"
    )
    layer_option = argparse.ArgumentParser(add_help=False)
    layer_option.add_argument(
        "--layer",
        required=True,
        choices=layer_image.LAYERS,
        help="the image layer",
    )
    touch = argparse.ArgumentParser(add_help=False)
    touch.add_argument(
        "--touch",
        action="store_true",
        help="give the files of each submodule its commit time",
    )
    paths = argparse.ArgumentParser(add_help=False)
    paths.add_argument(
        "paths", nargs="+", metavar="PATH", help="a file or directory"
    )
    published_ref = argparse.ArgumentParser(add_help=False)
    published_ref.add_argument("ref", metavar="REF", help="the image reference")
    pin_tag = argparse.ArgumentParser(add_help=False)
    pin_tag.add_argument(
        "--tag", dest="pin_tag", required=True, help="the image's tag"
    )
    pin_branch = argparse.ArgumentParser(add_help=False)
    pin_branch.add_argument(
        "--branch", required=True, metavar="NAME", help="the branch to pin"
    )
    base = argparse.ArgumentParser(add_help=False)
    base.add_argument(
        "--base",
        required=True,
        metavar="BRANCH",
        help="the base branch of the pull requests",
    )
    pr_label = argparse.ArgumentParser(add_help=False)
    pr_label.add_argument(
        "--label",
        dest="pr_label",
        required=True,
        metavar="NAME",
        help="the label of the pull requests, created when missing",
    )
    return {
        commands.Option.PRESET: preset,
        commands.Option.AUDIT_PRESET: audit_preset,
        commands.Option.LABEL: label,
        commands.Option.VERBOSE: verbose,
        commands.Option.CHECK: check,
        commands.Option.REF: ref,
        commands.Option.BRANCH: branch,
        commands.Option.IMAGE: image,
        commands.Option.TAG: tag,
        commands.Option.OUTPUT: output,
        commands.Option.PUSHED: pushed,
        commands.Option.TAGS: tags,
        commands.Option.LAYER: layer,
        commands.Option.LAYER_OPTION: layer_option,
        commands.Option.TOUCH: touch,
        commands.Option.PATHS: paths,
        commands.Option.PUBLISHED_REF: published_ref,
        commands.Option.BUILDKIT_CACHE: buildkit_cache_parser(),
        commands.Option.PIN_TAG: pin_tag,
        commands.Option.PIN_BRANCH: pin_branch,
        commands.Option.BASE: base,
        commands.Option.PR_LABEL: pr_label,
    }


def buildkit_cache_parser() -> argparse.ArgumentParser:
    """Returns the parent parser of the buildkit-cache options."""
    cache = argparse.ArgumentParser(add_help=False)
    cache.add_argument(
        "cache_action",
        choices=("load", "save"),
        metavar="{load,save}",
        help="load into the cache mount, or save it",
    )
    cache.add_argument(
        "--builder", required=True, metavar="NAME", help="the buildx builder"
    )
    cache.add_argument(
        "--id",
        dest="cache_id",
        required=True,
        metavar="ID",
        help="the id of the cache mount",
    )
    cache.add_argument(
        "--target",
        dest="cache_target",
        required=True,
        metavar="DIR",
        help="where the Dockerfile mounts it",
    )
    place = cache.add_mutually_exclusive_group(required=True)
    place.add_argument(
        "--from-dir", metavar="DIR", help="load a directory, then remove it"
    )
    place.add_argument(
        "--from-package",
        metavar="NAME",
        help="load the image ghcr.io/<owner>/NAME:latest, if published",
    )
    place.add_argument("--to-dir", metavar="DIR", help="save into a directory")
    place.add_argument(
        "--to-package",
        metavar="NAME",
        help="save into the image ghcr.io/<owner>/NAME:latest and push it",
    )
    return cache


def parser() -> argparse.ArgumentParser:
    """Returns the parser of the forge command line."""
    root = argparse.ArgumentParser(
        prog="python3 -m forge",
        description=(
            "Builds, tests and checks Umbra through the CMake presets and"
            " the shared checks, locally and in CI."
        ),
    )
    subcommands = root.add_subparsers(
        dest="name", metavar="COMMAND", required=True
    )
    parents = option_parsers()
    for command in commands.COMMANDS:
        subcommands.add_parser(
            command.name,
            help=command.summary,
            description=command.summary,
            parents=[parents[option] for option in command.options],
        ).set_defaults(command=command)
    return root


def main(argv: Sequence[str] | None = None) -> int:
    """Runs the forge command given on the command line.

    Args:
        argv: The command and its options; ``sys.argv[1:]`` when None.

    Returns:
        The process exit status: 0 when every step passed, otherwise the
        status of the step that failed.
    """
    args = parser().parse_args(argv)
    options = commands.Options(
        preset=commands.Preset(getattr(args, "preset", commands.Preset.DEBUG)),
        label=getattr(args, "label", None),
        verbose=getattr(args, "verbose", False),
        check=getattr(args, "check", False),
        ref=getattr(args, "ref", ""),
        branch=getattr(args, "branch", None),
        image=getattr(args, "image", ""),
        tag=getattr(args, "tag", None),
        output=getattr(args, "output", ""),
        pushed=getattr(args, "pushed", ""),
        tags=getattr(args, "tags", ""),
        layer=getattr(args, "layer", ""),
        touch=getattr(args, "touch", False),
        paths=tuple(getattr(args, "paths", ())),
        cache_action=getattr(args, "cache_action", ""),
        builder=getattr(args, "builder", ""),
        cache_id=getattr(args, "cache_id", ""),
        cache_target=getattr(args, "cache_target", ""),
        from_dir=getattr(args, "from_dir", None),
        from_package=getattr(args, "from_package", None),
        to_dir=getattr(args, "to_dir", None),
        to_package=getattr(args, "to_package", None),
        pin_tag=getattr(args, "pin_tag", ""),
        base=getattr(args, "base", ""),
        pr_label=getattr(args, "pr_label", ""),
    )
    return commands.execute(args.command.steps(options))


if __name__ == "__main__":
    sys.exit(main())
