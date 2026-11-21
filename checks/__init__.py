# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
"""Validators shared by the git hooks in .githooks/ and the CI workflows.

Each validator module exposes a ``validate`` function that returns a list of
human-readable error strings (empty when the input is valid) and a ``main``
entry point, so it can be run as ``python3 -m checks.<validator>``.

Only the Python 3 standard library is used.
"""
