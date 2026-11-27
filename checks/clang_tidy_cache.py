#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
"""Runs clang-tidy through ctcache, which skips results it has cached.

run-clang-tidy runs this script as its clang-tidy (``-clang-tidy-binary``),
with clang-tidy's own arguments. ctcache hashes the translation unit
preprocessed with ``-E -P``, which drops the preprocessor's directives, so a
change that only adds a macro or an include would match an earlier clean
run, though clang-tidy checks both (macro names, include-cleaner). This
script makes ctcache keep them, with ``-dD -dI``, then runs it.

The environment chooses the cache: ``CTCACHE_CLIENT`` is ctcache's
clang_tidy_cache.py, ``CTCACHE_CLANG_TIDY`` the clang-tidy it runs, and
ctcache reads its own variables (``CTCACHE_DIR`` and the like).
``checks.lint`` sets them.

Usage: clang_tidy_cache.py CLANG_TIDY_ARGUMENT...
"""

import importlib.util
import os
import sys

# The flags that make the preprocessed output keep the #define and #include
# directives.
KEEP_DIRECTIVES = ("-dD", "-dI")


def load_ctcache(path: str):
    """Loads ctcache's client module from its file.

    Args:
        path: The path of clang_tidy_cache.py.

    Returns:
        The module.
    """
    spec = importlib.util.spec_from_file_location("clang_tidy_cache", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def keep_directives(ctcache) -> None:
    """Makes ctcache keep the directives in the output it hashes.

    Args:
        ctcache: ctcache's client module.
    """
    options = ctcache.ClangTidyCacheOpts
    # ctcache has no option for its preprocessor flags: its method that
    # sets them is wrapped.
    # pylint: disable=protected-access
    adjust = options._adjust_compiler_args

    def adjust_keeping_directives(self) -> None:
        adjust(self)
        arguments = self._compiler_args
        if "-E" in arguments:
            position = arguments.index("-E") + 1
            arguments[position:position] = KEEP_DIRECTIVES

    options._adjust_compiler_args = adjust_keeping_directives
    # pylint: enable=protected-access


def main() -> int:
    """Runs clang-tidy, with this script's arguments, through ctcache.

    Returns:
        clang-tidy's exit status, or ctcache's cached one.
    """
    ctcache = load_ctcache(os.environ["CTCACHE_CLIENT"])
    keep_directives(ctcache)
    sys.argv = [
        sys.argv[0],
        os.environ.get("CTCACHE_CLANG_TIDY", "clang-tidy"),
        *sys.argv[1:],
    ]
    return ctcache.main()


if __name__ == "__main__":
    sys.exit(main())
