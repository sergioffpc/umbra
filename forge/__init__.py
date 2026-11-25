# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
"""The commands of development and CI: builds, tests, sanitizers and checks.

A layer over the CMake presets (CMakePresets.json) and the shared checks, run
as ``python3 -m forge <command>`` from the repository root: inside the
devcontainer, and in every job of the ci workflow, whose steps run forge
commands.

``commands`` lists the steps of each command and runs them; ``__main__``
parses the command line. The pre-push hook builds and tests through
``commands.check``. The modules that the CI checks call, each with a
``main(argv)``: ``pin`` (the devcontainer's image pin), ``pull_request`` (the
branch name, title and commits of a pull request), ``third_party`` (the
versions manifest and licenses), ``release`` (the version, changelog and tags
of a release branch), ``changes`` (whether a pull request changes code),
``image`` (the pinned image), ``gpu_module`` (the GPU module of a build tree)
and ``modules`` (an acyclic target graph). ``github`` writes the annotations
and step outputs of GitHub Actions, reads the event and runs gh.

Only the Python 3 standard library is used, so that the commands that need
no toolchain also run directly on a CI runner.
"""
