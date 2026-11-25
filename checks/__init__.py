# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
"""Checks shared by the git hooks in .githooks/, forge and CI.

Validator modules (branch_name, commit_message, and the file validators spdx,
nolint, safety_comment, assume_nonnull and namespace) expose a ``validate``
function that returns a list of human-readable error strings (empty when the
input is valid) and a ``main`` entry point, so each can be run as
``python3 -m checks.<validator>``. ``conventions`` runs every file validator.

Driver modules run the tools: ``formatting`` (clang-format, pyink, isort),
``lint`` (clang-tidy, the clang-query rules of ``clang_query``, pylint,
actionlint and the Python ``embedded`` in workflows) and ``affected`` (the
translation units a change affects).

Helper modules have no entry point: ``git`` runs git and the tools from the
repository root, ``report`` prints errors uniformly, and ``sources`` lists
the repository's files and tells their kind.

Only the Python 3 standard library is used.
"""
