# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
"""The target graph (modules, executables, tests) must be acyclic.

CMake accepts cycles between static libraries, so this checks the graph that
the debug configure writes with --graphviz, in build/graph/. Every edge of the
graph is printed, and a cycle is reported with the targets along it.

Usage: python3 -m forge modules
"""

import argparse
from collections.abc import Iterable, Sequence
import re

from checks import git
from forge import github

GRAPH = "build/graph/umbra.dot"
# Each dependency edge ends in a comment: // <target> -> <target>.
EDGE = re.compile(
    r'^\s*"node[0-9]+" -> "node[0-9]+".*// (?P<source>.+) -> (?P<target>.+)$'
)


def edges(dot: str) -> list[tuple[str, str]]:
    """Returns the dependency edges of a Graphviz file that CMake wrote."""
    return [
        (match["source"], match["target"])
        for match in map(EDGE.match, dot.splitlines())
        if match is not None
    ]


def find_cycle(pairs: Iterable[tuple[str, str]]) -> list[str]:
    """Returns the targets along a cycle of the graph.

    An edge from a target to itself orders nothing and is not a cycle.

    Args:
        pairs: The edges, from a target to a target it depends on.

    Returns:
        The targets of a cycle, in order; empty when the graph is acyclic.
    """
    successors: dict[str, list[str]] = {}
    for source, target in pairs:
        successors.setdefault(target, [])
        if source != target:
            successors.setdefault(source, []).append(target)
    done: set[str] = set()
    for root, targets in successors.items():
        if root in done:
            continue
        # A depth-first search: the path from the root, and for each target
        # on it the successors not visited yet.
        path = [root]
        pending = [iter(targets)]
        while path:
            target = next(pending[-1], None)
            if target is None:
                done.add(path.pop())
                pending.pop()
            elif target in path:
                return path[path.index(target) :]
            elif target not in done:
                path.append(target)
                pending.append(iter(successors[target]))
    return []


def main(argv: Sequence[str] | None = None) -> int:
    """Configures the debug preset and checks its target graph.

    Args:
        argv: The arguments without the program name; there are none.

    Returns:
        The process exit status: 0 when the graph is acyclic, the configure's
        status when it fails, and 1 on a cycle.
    """
    argparse.ArgumentParser(description=__doc__.splitlines()[0]).parse_args(
        argv
    )
    status = git.call(
        ["cmake", "--preset", "debug", f"--graphviz={GRAPH}"], quiet=True
    ).returncode
    if status:
        return status
    pairs = edges((git.REPO_ROOT / GRAPH).read_text(encoding="utf-8"))
    print("Dependencies:")
    for source, target in pairs:
        print(f"  {source} {target}")
    cycle = find_cycle(pairs)
    if cycle:
        targets = " ".join(cycle)
        github.error(
            f"the target graph has a cycle: {targets}",
            title="Module cycle",
        )
        return 1
    print("The target graph is acyclic.")
    return 0
