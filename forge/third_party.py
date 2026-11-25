# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
"""The versions manifest of the third-party submodules and their licenses.

Every row of third_party/versions.md whose Path cell is code names a
submodule, its upstream release tag, the commit the tag points to and its
license, an SPDX expression compared whole with LICENSES. Every gitlink of
the tree must be listed, every listed path must be a gitlink at that commit,
and the tag must exist upstream (its URL in .gitmodules) and point to that
commit, peeled when annotated. Needs git and access to the upstream
repositories. The bump-submodules command reads the manifest through this
module too.

Usage: python3 -m forge third-party
"""

import argparse
from collections.abc import Sequence
import dataclasses
import re

from checks import git
from forge import github

MANIFEST = "third_party/versions.md"
# The license allow-list.
LICENSES = frozenset(
    {
        "Apache-2.0",
        "Apache-2.0 WITH LLVM-exception",
        "BSD-2-Clause",
        "BSD-3-Clause",
        "MIT",
        "Zlib",
        "BSL-1.0",
        "LicenseRef-TOST-1.0",
        "libtiff",
    }
)
# A table row whose Path cell is code: | Name | `path` | ...
ROW = re.compile(r"^\|[^|]*\|\s*`")


@dataclasses.dataclass(frozen=True)
class Entry:
    """A row of the manifest.

    Attributes:
        name: The dependency's name.
        path: The submodule path.
        tag: The upstream release tag.
        commit: The full commit the tag points to.
        license: The SPDX license expression.
    """

    name: str
    path: str
    tag: str
    commit: str
    license: str


def cell(text: str) -> str:
    """Returns the content of a table cell, without code marks or padding."""
    return text.replace("`", "").strip()


def fail(message: str) -> None:
    """Reports an error on the manifest."""
    github.error(message, file=MANIFEST)


def read_manifest() -> tuple[list[Entry], list[str]]:
    """Reads the rows of the manifest.

    Returns:
        The entries, in order, a path listed twice keeping its last row, and
        the errors.
    """
    entries: dict[str, Entry] = {}
    errors = []
    text = (git.REPO_ROOT / MANIFEST).read_text(encoding="utf-8")
    for line in text.splitlines():
        if not ROW.match(line):
            continue
        cells = [*line.split("|")[1:6], "", "", "", "", ""][:5]
        name, path, tag, commit, license_ = map(cell, cells)
        if path in entries:
            errors.append(f"{path} is listed more than once")
        entries[path] = Entry(name, path, tag, commit, license_)
    return list(entries.values()), errors


def gitlinks() -> dict[str, str]:
    """Returns the commit of every gitlink of HEAD's tree, by path."""
    links = {}
    for line in git.run("ls-tree", "-r", "HEAD").splitlines():
        info, path = line.split("\t", 1)
        _, kind, sha = info.split()
        if kind == "commit":
            links[path] = sha
    return links


def submodule_urls() -> dict[str, str]:
    """Returns the URL of every submodule in .gitmodules, by path."""
    result = git.call(
        ["git", "config", "-f", ".gitmodules", "--get-regexp", "^submodule\\."],
        capture=True,
    )
    keys = {}
    for line in result.stdout.splitlines():
        key, _, value = line.partition(" ")
        keys[key] = value
    urls = {}
    for key, value in keys.items():
        if key.startswith("submodule.") and key.endswith(".path"):
            url = keys.get(key.removesuffix(".path") + ".url")
            if url:
                urls[value] = url
    return urls


def upstream_commit(url: str, tag: str) -> str | None:
    """Returns the commit a tag points to upstream, peeled when annotated.

    Args:
        url: The upstream repository.
        tag: The tag's name.

    Returns:
        The commit; an empty string when the tag does not exist, and None
        when the repository cannot be listed.
    """
    result = git.call(
        [
            "git",
            "ls-remote",
            "--tags",
            url,
            f"refs/tags/{tag}",
            f"refs/tags/{tag}^{{}}",
        ],
        capture=True,
    )
    if result.returncode:
        print(result.stderr, end="")
        return None
    refs = {}
    for line in result.stdout.splitlines():
        sha, _, ref = line.partition("\t")
        refs[ref] = sha
    return refs.get(f"refs/tags/{tag}^{{}}") or refs.get(f"refs/tags/{tag}", "")


def entry_errors(
    entry: Entry, links: dict[str, str], urls: dict[str, str]
) -> list[str]:
    """Returns the errors of one manifest entry.

    Args:
        entry: The entry.
        links: The commit of every gitlink, by path.
        urls: The URL of every submodule, by path.
    """
    errors = []
    path, tag, commit = entry.path, entry.tag, entry.commit
    if entry.license not in LICENSES:
        errors.append(
            f"{path}: license '{entry.license}' is not on the allow-list"
        )
    if path not in links:
        return [*errors, f"{path} is listed but is not a submodule"]
    if commit != links[path]:
        errors.append(
            f"{path}: the manifest says {commit}, the submodule is at"
            f" {links[path]}"
        )
    url = urls.get(path)
    if not url:
        return [*errors, f"{path} has no URL in .gitmodules"]
    upstream = upstream_commit(url, tag)
    if upstream is None:
        errors.append(f"{path}: the tags of {url} cannot be listed")
    elif not upstream:
        errors.append(f"{path}: tag {tag} does not exist in {url}")
    elif upstream != commit:
        errors.append(f"{path}: tag {tag} points to {upstream}, not {commit}")
    return errors


def main(argv: Sequence[str] | None = None) -> int:
    """Checks the manifest against the tree, the licenses and the tags.

    Args:
        argv: The arguments without the program name; there are none.

    Returns:
        The process exit status: 0 when every check passes, 1 otherwise.
    """
    argparse.ArgumentParser(description=__doc__.splitlines()[0]).parse_args(
        argv
    )
    entries, errors = read_manifest()
    for entry in entries:
        print(
            f"Manifest: {entry.name} {entry.path} {entry.tag} {entry.commit}"
            f" {entry.license}"
        )
    links = gitlinks()
    listed = {entry.path for entry in entries}
    errors.extend(
        f"submodule {path} is missing from the manifest"
        for path in sorted(links)
        if path not in listed
    )
    urls = submodule_urls()
    for entry in entries:
        errors.extend(entry_errors(entry, links, urls))
    for message in errors:
        fail(message)
    return 1 if errors else 0
