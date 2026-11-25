# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
"""The developer commands: builds, tests, sanitizers, coverage and checks.

A convenience over the CMake presets (CMakePresets.json) and the shared
checks, run inside the devcontainer as ``python3 -m forge <command>`` from the
repository root. CI does not use it: it runs the presets and the checks
directly.

``commands`` lists the steps of each command and runs them; ``__main__``
parses the command line. The pre-push hook builds and tests through
``commands.check``.

Only the Python 3 standard library is used.
"""
