# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
"""The SPDX 2.3 SBOM of the release image.

The SBOM describes the image, which contains umbra and the Ubuntu packages
that Chisel cut into it; umbra contains the submodules whose license texts it
installs in /opt/umbra/share/licenses/<component>/, the components of the
image. Each Ubuntu package comes from Chisel's manifest in the image,
/var/lib/chisel/manifest.wall, with its version, architecture and copyright
file, and the license that the Ubuntu table of third_party/versions.md
declares, if any. Each submodule comes from the table of submodules of that
file, with its tag, commit, upstream repository (.gitmodules) and license;
the text of a LicenseRef- license is read from the image. umbra's version is
the CMake project version, its commit HEAD, and the repository the image's
org.opencontainers.image.source label.

The output is deterministic: the creation time is HEAD's commit time, the
namespace is made of the repository, the version and the image ID, and the
packages and relationships are sorted. Needs docker, with the image loaded,
and git.

Usage: python3 -m forge sbom IMAGE --output FILE
"""

import argparse
from collections.abc import Iterable, Sequence
import dataclasses
import datetime
import importlib
import json
import pathlib
import re
import subprocess
from typing import Any
import urllib.parse

from checks import git
from forge import github
from forge import release
from forge import release_image
from forge import third_party

MANIFEST = "var/lib/chisel/manifest.wall"
OS_RELEASE = "usr/lib/os-release"
PREFIX = "/opt/umbra"
LICENSES = "opt/umbra/share/licenses"
SOURCE_LABEL = "org.opencontainers.image.source"
NOASSERTION = "NOASSERTION"
LICENSE_REF = re.compile(r"LicenseRef-[A-Za-z0-9.-]+")
# The characters that an SPDX identifier may not contain.
NOT_IN_ID = re.compile(r"[^A-Za-z0-9.-]")
# A row of the Ubuntu packages table: | Name | Needed by | Packages | License |
UBUNTU_SECTION = "## Ubuntu packages"


@dataclasses.dataclass(frozen=True)
class DebPackage:
    """An Ubuntu package in Chisel's manifest.

    Attributes:
        name: The package's name.
        version: Its Debian version.
        arch: Its architecture.
    """

    name: str
    version: str
    arch: str


def spdx_id(kind: str, name: str) -> str:
    """Returns the SPDX identifier of a package of a kind."""
    return f"SPDXRef-{kind}-" + NOT_IN_ID.sub("-", name)


def decompress(data: bytes) -> bytes:
    """Decompresses Zstandard data, with Python's zstd or the zstd program.

    Raises:
        github.StepError: Neither can decompress it.
    """
    try:
        zstd = importlib.import_module("compression.zstd")
    except ImportError:
        zstd = None
    if zstd is not None:
        return zstd.decompress(data)
    try:
        result = subprocess.run(
            ["zstd", "-dcq"], input=data, stdout=subprocess.PIPE, check=False
        )
    except OSError as error:
        raise github.StepError(f"zstd: {error.strerror}") from error
    if result.returncode:
        raise github.StepError(
            f"zstd exited with status {result.returncode} on {MANIFEST}"
        )
    return result.stdout


def read_chisel_manifest(
    data: bytes,
) -> tuple[list[DebPackage], frozenset[str]]:
    """Reads Chisel's manifest, a Zstandard-compressed jsonwall database.

    Args:
        data: The compressed manifest.

    Returns:
        The packages, and the paths of the files that the slices cut.

    Raises:
        github.StepError: The manifest is not a jsonwall database of schema
            1.0.
    """
    lines = decompress(data).decode("utf-8").splitlines()
    header = json.loads(lines[0]) if lines else {}
    if "jsonwall" not in header or header.get("schema") != "1.0":
        raise github.StepError(
            f"/{MANIFEST} is not a Chisel manifest of schema 1.0"
        )
    packages = []
    paths = set()
    for line in lines[1:]:
        entry = json.loads(line)
        if entry.get("kind") == "package":
            packages.append(
                DebPackage(entry["name"], entry["version"], entry["arch"])
            )
        elif entry.get("kind") == "path":
            paths.add(entry["path"])
    return packages, frozenset(paths)


def os_release(text: str) -> dict[str, str]:
    """Returns the variables of an os-release file."""
    variables = {}
    for line in text.splitlines():
        name, separator, value = line.partition("=")
        if separator and not name.startswith("#"):
            variables[name.strip()] = value.strip().strip("\"'")
    return variables


def ubuntu_licenses() -> dict[str, str]:
    """Returns the license of each Ubuntu package that versions.md lists."""
    text = (git.REPO_ROOT / third_party.MANIFEST).read_text(encoding="utf-8")
    section = text.partition(UBUNTU_SECTION)[2].partition("\n## ")[0]
    licenses = {}
    for line in section.splitlines():
        cells = line.split("|")[1:-1]
        if len(cells) != 4 or "`" not in cells[2]:
            continue
        license_ = third_party.cell(cells[3])
        for name in re.findall(r"`([^`]+)`", cells[2]):
            licenses[name] = license_
    return licenses


def purl_of_repository(url: str, version: str) -> str | None:
    """Returns the package URL of a GitHub or GitLab repository's version.

    Args:
        url: The repository's https URL.
        version: The tag.

    Returns:
        The package URL, or None for another host.
    """
    parsed = urllib.parse.urlsplit(url)
    host = {"github.com": "github", "gitlab.com": "gitlab"}.get(parsed.netloc)
    if host is None:
        return None
    path = parsed.path.strip("/").removesuffix(".git").lower()
    return f"pkg:{host}/{path}@" + urllib.parse.quote(version, safe="")


def package(
    spdx: str,
    name: str,
    version: str,
    *,
    download: str = NOASSERTION,
    license_: str = NOASSERTION,
    purl: str | None = None,
    comment: str | None = None,
) -> dict[str, Any]:
    """Returns an SPDX package whose files are not analyzed.

    Args:
        spdx: Its SPDX identifier.
        name: Its name.
        version: Its version.
        download: Where it is downloaded from, an SPDX download location.
        license_: Its declared license, an SPDX expression.
        purl: Its package URL, if any.
        comment: A comment on it, if any.
    """
    result: dict[str, Any] = {
        "SPDXID": spdx,
        "name": name,
        "versionInfo": version,
        "downloadLocation": download,
        "filesAnalyzed": False,
        "licenseConcluded": NOASSERTION,
        "licenseDeclared": license_,
        "copyrightText": NOASSERTION,
    }
    if purl:
        result["externalRefs"] = [
            {
                "referenceCategory": "PACKAGE-MANAGER",
                "referenceType": "purl",
                "referenceLocator": purl,
            }
        ]
    if comment:
        result["comment"] = comment
    return result


def deb_packages(
    debs: Iterable[DebPackage], paths: frozenset[str], distro: str
) -> list[dict[str, Any]]:
    """Returns the SPDX packages of the Ubuntu packages.

    Args:
        debs: The packages in Chisel's manifest.
        paths: The paths that Chisel cut.
        distro: The Ubuntu release, ubuntu-<VERSION_ID>.
    """
    licenses = ubuntu_licenses()
    packages = []
    for deb in debs:
        copyright_ = f"/usr/share/doc/{deb.name}/copyright"
        comment = (
            f"Its copyright and license are in {copyright_}."
            if copyright_ in paths
            else "Its copyright file is not in the image."
        )
        version = urllib.parse.quote(deb.version, safe="")
        purl = f"pkg:deb/ubuntu/{deb.name}@{version}?arch={deb.arch}"
        packages.append(
            package(
                spdx_id("Deb", deb.name),
                deb.name,
                deb.version,
                license_=licenses.get(deb.name, NOASSERTION),
                purl=f"{purl}&distro={distro}",
                comment=comment,
            )
        )
    return packages


def submodule_packages(
    components: Iterable[str],
) -> tuple[list[dict[str, Any]], list[str]]:
    """Returns the SPDX packages of the submodules in the image.

    Args:
        components: The components of the image, each the last part of a
            submodule path.

    Returns:
        The packages, and the errors: a component that is no submodule.
    """
    entries, errors = third_party.read_manifest()
    by_component = {pathlib.PurePosixPath(e.path).name: e for e in entries}
    urls = third_party.submodule_urls()
    packages = []
    for component in components:
        entry = by_component.get(component)
        if entry is None:
            errors.append(
                f"{PREFIX}/share/licenses/{component} is the license of no"
                f" submodule in {third_party.MANIFEST}"
            )
            continue
        url = urls.get(entry.path, "")
        packages.append(
            package(
                spdx_id("Submodule", component),
                entry.name,
                entry.tag,
                download=f"git+{url}@{entry.commit}" if url else NOASSERTION,
                license_=entry.license,
                purl=purl_of_repository(url, entry.tag) if url else None,
                comment=(
                    f"Commit {entry.commit}. Its license texts are in"
                    f" {PREFIX}/share/licenses/{component}/."
                ),
            )
        )
    return packages, errors


def extracted_licenses(
    packages: Iterable[dict[str, Any]], texts: dict[str, bytes]
) -> tuple[list[dict[str, str]], list[str]]:
    """Returns the text of each LicenseRef- license that a package declares.

    The text is the first license file of the first component, in order of
    SPDX identifier, that declares the license.

    Args:
        packages: The SPDX packages; those of submodules have the identifier
            SPDXRef-Submodule-<component>.
        texts: The license files of the image, by path.

    Returns:
        The extracted licensing information, and the errors: a license
        without a text in the image.
    """
    found: dict[str, dict[str, str]] = {}
    for spdx in sorted(packages, key=lambda spdx: spdx["SPDXID"]):
        component = spdx["SPDXID"].removeprefix("SPDXRef-Submodule-")
        files = sorted(
            path
            for path in texts
            if path.startswith(f"{LICENSES}/{component}/")
        )
        for license_id in LICENSE_REF.findall(spdx["licenseDeclared"]):
            if license_id in found or not files:
                continue
            found[license_id] = {
                "licenseId": license_id,
                "extractedText": texts[files[0]].decode("utf-8", "replace"),
                "comment": f"The text of /{files[0]}.",
            }
    declared = {
        license_id
        for spdx in packages
        for license_id in LICENSE_REF.findall(spdx["licenseDeclared"])
    }
    errors = [
        f"no license text of {license_id} in the image"
        for license_id in sorted(declared - found.keys())
    ]
    return [found[license_id] for license_id in sorted(found)], errors


def image_repository(image: str) -> str:
    """Returns the repository of an image reference, without tag or digest."""
    name = image.partition("@")[0]
    head, slash, last = name.rpartition("/")
    return head + slash + last.partition(":")[0]


def document(
    image: str, config: dict[str, Any], files: dict[str, bytes]
) -> tuple[dict[str, Any], list[str]]:
    """Returns the SBOM of the release image.

    Args:
        image: The image reference.
        config: The image's ``docker image inspect`` object.
        files: The manifest, the os-release file and the license files of
            the image, by path.

    Returns:
        The SPDX document, and the errors.
    """
    errors = []
    source = (config.get("Config", {}).get("Labels") or {}).get(SOURCE_LABEL)
    if not source:
        errors.append(f"the image has no {SOURCE_LABEL} label")
        source = NOASSERTION
    if MANIFEST not in files:
        raise github.StepError(f"the image has no /{MANIFEST}")
    debs, paths = read_chisel_manifest(files[MANIFEST])
    release_info = os_release(files.get(OS_RELEASE, b"").decode("utf-8"))
    version_id = release_info.get("VERSION_ID", "")
    if not version_id:
        errors.append(f"the image has no VERSION_ID in /{OS_RELEASE}")
    distro = f"ubuntu-{version_id}"
    cmake_lists = (git.REPO_ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    version = release.project_version(cmake_lists)
    commit = git.run("rev-parse", "HEAD").strip()
    timestamp = int(git.run("log", "-1", "--format=%ct", "HEAD"))
    created = datetime.datetime.fromtimestamp(timestamp, datetime.UTC)
    components = sorted(
        {
            path.split("/")[4]
            for path in files
            if path.startswith(f"{LICENSES}/") and path.split("/")[4] != "umbra"
        }
    )
    submodules, submodule_errors = submodule_packages(components)
    errors.extend(submodule_errors)
    licenses, license_errors = extracted_licenses(submodules, files)
    errors.extend(license_errors)
    repository = image_repository(image)
    image_package = package("SPDXRef-Image", repository, version)
    image_package["primaryPackagePurpose"] = "CONTAINER"
    umbra = package(
        "SPDXRef-Package-umbra",
        "umbra",
        version,
        download=f"git+{source}.git@{commit}",
        license_="Apache-2.0",
        purl=purl_of_repository(source, f"v{version}"),
        comment=(
            f"Commit {commit}. Its license texts are in"
            f" {PREFIX}/share/licenses/umbra/."
        ),
    )
    umbra["supplier"] = "Organization: The Umbra Authors"
    umbra["primaryPackagePurpose"] = "APPLICATION"
    ubuntu = deb_packages(debs, paths, distro)
    relationships = [
        ("SPDXRef-DOCUMENT", "DESCRIBES", "SPDXRef-Image"),
        ("SPDXRef-Image", "CONTAINS", "SPDXRef-Package-umbra"),
        *(("SPDXRef-Image", "CONTAINS", deb["SPDXID"]) for deb in ubuntu),
        *(
            ("SPDXRef-Package-umbra", "CONTAINS", submodule["SPDXID"])
            for submodule in submodules
        ),
    ]
    image_id = str(config.get("Id", "")).removeprefix("sha256:")
    result = {
        "spdxVersion": "SPDX-2.3",
        "dataLicense": "CC0-1.0",
        "SPDXID": "SPDXRef-DOCUMENT",
        "name": f"{repository}:{version}",
        "documentNamespace": f"{source}/sbom/{version}/{image_id}",
        "comment": (
            f"The license texts of umbra and of each component under {PREFIX}"
            f" are in {PREFIX}/share/licenses/<component>/ in the image; the"
            " copyright and license of each Ubuntu package are in"
            " /usr/share/doc/<package>/copyright."
        ),
        "creationInfo": {
            "created": created.strftime("%Y-%m-%dT%H:%M:%SZ"),
            "creators": [
                "Organization: The Umbra Authors",
                "Tool: forge-sbom",
            ],
        },
        "packages": sorted(
            [image_package, umbra, *submodules, *ubuntu],
            key=lambda spdx: spdx["SPDXID"],
        ),
        "relationships": [
            {
                "spdxElementId": element,
                "relationshipType": kind,
                "relatedSpdxElement": related,
            }
            for element, kind, related in sorted(relationships)
        ],
    }
    if licenses:
        result["hasExtractedLicensingInfos"] = licenses
    return result, errors


def wanted(path: str) -> bool:
    """Returns whether the SBOM reads the image's file at a path."""
    return path in (MANIFEST, OS_RELEASE) or path.startswith(f"{LICENSES}/")


def main(argv: Sequence[str] | None = None) -> int:
    """Writes the SBOM of the release image.

    Args:
        argv: The arguments without the program name: the image and
            ``--output FILE``.

    Returns:
        The process exit status: 0 when the SBOM is written, 1 otherwise.
    """
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("image", help="the image, loaded in docker")
    parser.add_argument(
        "--output", required=True, metavar="FILE", help="the SBOM to write"
    )
    args = parser.parse_args(argv)
    config = json.loads(
        release_image.docker(
            "image", "inspect", "--format", "{{json .}}", args.image
        )
    )
    _, files = release_image.export(args.image, wanted)
    sbom, errors = document(args.image, config, files)
    for message in errors:
        github.error(message)
    if errors:
        return 1
    pathlib.Path(args.output).write_text(
        json.dumps(sbom, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    count = len(sbom["packages"])
    print(f"{args.output}: {count} packages", flush=True)
    return 0
