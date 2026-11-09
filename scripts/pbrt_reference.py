#!/usr/bin/env python3
"""Builds the pinned pbrt-v4, renders the reference statistics of every
Reference Scene and publishes them as the GitHub release
`pbrt-v4-reference/<commit>` (ADR 0013, arc42 §8.11).

Run by hand on a target machine, never in CI. Needs Python >= 3.11, git,
cmake, a C++17 compiler and an authenticated `gh`.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
import shutil
import subprocess
import sys
import tomllib
from array import array
from collections.abc import Sequence
from dataclasses import asdict, dataclass
from enum import Enum
from pathlib import Path, PurePosixPath
from typing import Any, NamedTuple


class ReferenceListError(Exception):
    """The reference list is malformed."""


@dataclass(frozen=True)
class Pin:
    """A repository at a pinned commit."""

    repository: str
    commit: str


@dataclass(frozen=True)
class Settings:
    """Resolution and Pixel Samples per pixel of one use of a Reference Scene."""

    resolution: tuple[int, int]
    pixel_samples: int


@dataclass(frozen=True)
class Edit:
    """The declared edit of a Reference Scene Variant: `find` is replaced by
    `replace` in its Scene File, and must occur there exactly once."""

    find: str
    replace: str


@dataclass(frozen=True)
class ReferenceScene:
    """One entry of the reference list."""

    name: str
    scene_file: str
    benchmark: Settings
    validation: Settings
    edit: Edit | None


@dataclass(frozen=True)
class ReferenceList:
    """The pinned commits and the Reference Scenes."""

    pbrt_v4: Pin
    pbrt_v4_scenes: Pin
    scenes: list[ReferenceScene]


def load_reference_list(text: str) -> ReferenceList:
    """Parses the reference list (`reference/scenes.toml`).

    Raises `ReferenceListError` on any malformed or missing entry. Keys the
    script does not use (Tile size, seeds, golden settings) are left to the
    other readers of the list.
    """
    try:
        doc = tomllib.loads(text)
    except tomllib.TOMLDecodeError as error:
        raise ReferenceListError(str(error)) from error
    scenes = [
        _scene(entry, f"reference-scene[{i}]")
        for i, entry in enumerate(_get(doc, "reference-scene", list, "the list"))
    ]
    if not scenes:
        raise ReferenceListError("the list has no reference-scene")
    names = [scene.name for scene in scenes]
    if duplicates := sorted({name for name in names if names.count(name) > 1}):
        raise ReferenceListError(f"duplicate reference-scene names: {duplicates}")
    return ReferenceList(
        pbrt_v4=_pin(_get(doc, "pbrt-v4", dict, "the list"), "pbrt-v4"),
        pbrt_v4_scenes=_pin(_get(doc, "pbrt-v4-scenes", dict, "the list"), "pbrt-v4-scenes"),
        scenes=scenes,
    )


_COMMIT = re.compile(r"[0-9a-f]{40}")
# A name becomes the prefix of release asset and file names.
_NAME = re.compile(r"[a-z0-9][a-z0-9._-]*")


def _is_positive_int(value: Any) -> bool:
    # `bool` is an `int` to Python, never a valid count or size here.
    return isinstance(value, int) and not isinstance(value, bool) and value > 0


def _get(table: dict[str, Any], key: str, kind: type, where: str) -> Any:
    if key not in table:
        raise ReferenceListError(f"{where}: missing `{key}`")
    value = table[key]
    if not isinstance(value, kind):
        raise ReferenceListError(f"{where}: `{key}` must be a {kind.__name__}")
    return value


def _positive(table: dict[str, Any], key: str, where: str) -> int:
    value = table.get(key)
    if not _is_positive_int(value):
        raise ReferenceListError(f"{where}: `{key}` must be a positive integer")
    return value


def _pin(table: dict[str, Any], where: str) -> Pin:
    commit = _get(table, "commit", str, where)
    if not _COMMIT.fullmatch(commit):
        raise ReferenceListError(f"{where}: `commit` must be a full 40-digit SHA-1")
    return Pin(repository=_get(table, "repository", str, where), commit=commit)


def _settings(table: dict[str, Any], where: str) -> Settings:
    resolution = _get(table, "resolution", list, where)
    if len(resolution) != 2 or not all(_is_positive_int(n) for n in resolution):
        raise ReferenceListError(f"{where}: `resolution` must be two positive integers")
    x, y = resolution
    return Settings(resolution=(x, y), pixel_samples=_positive(table, "pixel-samples", where))


def _scene(table: dict[str, Any], where: str) -> ReferenceScene:
    name = _get(table, "name", str, where)
    if not _NAME.fullmatch(name):
        raise ReferenceListError(f"{where}: `name` {name!r} must match {_NAME.pattern}")
    where = f"reference-scene {name!r}"
    benchmark = _settings(_get(table, "benchmark", dict, where), f"{where}.benchmark")
    validation = _settings(_get(table, "validation", dict, where), f"{where}.validation")
    # pbrt-v4 renders both the validation seeds and the Reference Image from
    # the one Scene File, at the resolution it declares.
    if benchmark.resolution != validation.resolution:
        raise ReferenceListError(
            f"{where}: the benchmark and validation resolutions must be equal"
        )
    return ReferenceScene(
        name=name,
        scene_file=_scene_file(table, where),
        benchmark=benchmark,
        validation=validation,
        edit=_edit(table["edit"], f"{where}.edit") if "edit" in table else None,
    )


def _scene_file(table: dict[str, Any], where: str) -> str:
    scene_file = _get(table, "scene-file", str, where)
    path = PurePosixPath(scene_file)
    if path.is_absolute() or ".." in path.parts or not path.parts:
        raise ReferenceListError(
            f"{where}: `scene-file` must be a path inside the scene collection"
        )
    return scene_file


def _edit(table: dict[str, Any], where: str) -> Edit:
    find = _get(table, "find", str, where)
    if not find:
        raise ReferenceListError(f"{where}: `find` must not be empty")
    return Edit(find=find, replace=_get(table, "replace", str, where))


class SceneEditError(Exception):
    """A Scene File cannot be edited as the reference requires."""


# Added to the Film of every Scene File pbrt-v4 renders for the reference, and
# recorded in the manifest, since Umbra does not get this edit.
FLOAT_FILM_PARAMETERS = '"bool savefp16" [ false ]'
# A `Film` directive at the start of a line, outside comments.
_FILM = re.compile(r'^([ \t]*Film[ \t]+"[^"]*")', re.MULTILINE)


def edit_scene(text: str, edit: Edit | None) -> str:
    """Returns the Scene File that pbrt-v4 renders for the reference.

    A Reference Scene Variant's edit is applied first. Then the Film is told to
    write 32-bit floats (`savefp16` is on by default in pbrt-v4's `RGBFilm`):
    half floats would round every estimate, and two zero-variance pixels then
    compare equal or not depending on that rounding.
    """
    if edit is not None:
        if (count := text.count(edit.find)) != 1:
            raise SceneEditError(
                f"the edit's `find` must occur once in the Scene File, found {count}"
            )
        text = text.replace(edit.find, edit.replace)
    if "savefp16" in text:
        raise SceneEditError("the Scene File already sets `savefp16`")
    films = _FILM.findall(text)
    if len(films) != 1:
        raise SceneEditError(
            f"expected one Film directive in the Scene File, found {len(films)}"
        )
    return _FILM.sub(lambda film: f"{film[1]} {FLOAT_FILM_PARAMETERS}", text)


class ImageError(Exception):
    """An image cannot be read or combined."""


@dataclass(frozen=True)
class Image:
    """An RGB image of `width` × `height` pixels, three values per pixel, rows
    in the order the file stores them (PFM: bottom row first)."""

    width: int
    height: int
    values: array[float]


# The header ends with the single whitespace byte before the pixels.
_PFM_HEADER = re.compile(rb"PF\s+(?P<width>\d+)\s+(?P<height>\d+)\s+(?P<scale>\S+)\s")


def read_pfm(data: bytes) -> Image:
    """Decodes a three-channel PFM, as pbrt-v4 writes for `--outfile x.pfm`."""
    header = _PFM_HEADER.match(data)
    if header is None:
        raise ImageError("not a three-channel PFM")
    width, height = int(header["width"]), int(header["height"])
    try:
        scale = float(header["scale"])
    except ValueError as error:
        raise ImageError(f"malformed PFM scale: {error}") from error
    if abs(scale) != 1.0:
        raise ImageError(f"PFM scale {scale} is not ±1")
    pixels = data[header.end() :]
    count = width * height * 3
    if len(pixels) != count * 4:
        raise ImageError(f"PFM holds {len(pixels)} bytes of pixels, expected {count * 4}")
    values = array("f")
    values.frombytes(pixels)
    if (scale < 0) != (sys.byteorder == "little"):
        values.byteswap()
    return Image(width, height, values)


def write_pfm(image: Image) -> bytes:
    """Encodes `image` as a little-endian three-channel PFM of 32-bit floats."""
    header = f"PF\n{image.width} {image.height}\n-1\n".encode("ascii")
    values = array("f", image.values)
    if sys.byteorder != "little":
        values.byteswap()
    return header + values.tobytes()


def pixel_statistics(images: Sequence[Image]) -> tuple[Image, Image]:
    """Per pixel and channel, the mean and the unbiased sample variance
    (n − 1 in the denominator) of `images`, computed in `f64`.

    Every value must be finite: pbrt-v4's Film never adds a non-finite
    Pixel Sample, so one here means a broken render.
    """
    if len(images) < 2:
        raise ImageError("a variance needs at least two images")
    first = images[0]
    if any((im.width, im.height) != (first.width, first.height) for im in images):
        raise ImageError("the images do not all have the same resolution")
    if not all(math.isfinite(v) for im in images for v in im.values):
        raise ImageError("an image holds a non-finite value")
    n = len(images)
    means = array("d")
    variances = array("d")
    for samples in zip(*(im.values for im in images), strict=True):
        mean = math.fsum(samples) / n
        means.append(mean)
        variances.append(math.fsum((s - mean) ** 2 for s in samples) / (n - 1))
    return (
        Image(first.width, first.height, means),
        Image(first.width, first.height, variances),
    )


# K = 16 renders with seeds 0–15 per Reference Scene (ADR 0013).
VALIDATION_SEEDS = tuple(range(16))
# The Reference Image of QS-1 has 64 × the benchmark Pixel Samples per pixel.
REFERENCE_IMAGE_FACTOR = 64
# Seed of the Reference Image. Not a validation seed, and not the benchmark
# seed 0: with the same seed, the first Pixel Samples of each pixel would be
# those of the render being compared with it, and its error would look smaller.
REFERENCE_IMAGE_SEED = 16
# Version of the manifest's layout; bumped when a reader would misread it.
MANIFEST_SCHEMA = 1


class PublishError(Exception):
    """A Reference Scene cannot be published as asked."""


class Action(Enum):
    """What a run does with one Reference Scene."""

    RENDER = "render"
    SKIP = "skip"


def manifest_inputs(refs: ReferenceList, scene: ReferenceScene) -> dict[str, Any]:
    """Everything that determines a Reference Scene's published reference."""
    return {
        "schema": MANIFEST_SCHEMA,
        "reference-scene": scene.name,
        "pbrt-v4": asdict(refs.pbrt_v4),
        "pbrt-v4-scenes": asdict(refs.pbrt_v4_scenes),
        "scene-file": scene.scene_file,
        "edit": asdict(scene.edit) if scene.edit else None,
        # Stored as 32-bit floats, not pbrt-v4's default half floats.
        "pbrt-v4-film-parameters": FLOAT_FILM_PARAMETERS,
        "validation": {
            "resolution": list(scene.validation.resolution),
            "pixel-samples": scene.validation.pixel_samples,
            "seeds": list(VALIDATION_SEEDS),
            "variance": "unbiased sample variance (n - 1) over the seeds",
        },
        "reference-image": {
            "resolution": list(scene.benchmark.resolution),
            "pixel-samples": REFERENCE_IMAGE_FACTOR * scene.benchmark.pixel_samples,
            "seed": REFERENCE_IMAGE_SEED,
        },
    }


def manifest(
    refs: ReferenceList, scene: ReferenceScene, hashes: dict[str, str]
) -> dict[str, Any]:
    """The manifest published with a Reference Scene's assets, with the
    SHA-256 of each asset by name. It is uploaded last, so its presence in
    the release marks the Reference Scene as published."""
    return manifest_inputs(refs, scene) | {"sha256": dict(sorted(hashes.items()))}


def plan(
    refs: ReferenceList, scene: ReferenceScene, published: dict[str, Any] | None
) -> Action:
    """Decides what to do with `scene` given its published manifest, if any.

    A published reference is immutable: one rendered from other inputs is
    refused rather than skipped or overwritten.
    """
    if published is None:
        return Action.RENDER
    expected = manifest_inputs(refs, scene)
    if differing := sorted(k for k in expected if published.get(k) != expected[k]):
        raise PublishError(
            f"{scene.name} is already published from other inputs ({', '.join(differing)}); "
            "delete its assets from the release to publish it again"
        )
    return Action.SKIP


# ---------------------------------------------------------------------------
# The run: everything below drives git, cmake, pbrt-v4 and gh. It is checked
# by running the script, not by the unit tests.

REPOSITORY_ROOT = Path(__file__).resolve().parent.parent


def log(message: str) -> None:
    print(f"pbrt-reference: {message}", file=sys.stderr, flush=True)


def run(*command: str | Path, cwd: Path | None = None) -> None:
    subprocess.run([str(c) for c in command], cwd=cwd, check=True)


def stdout_of(*command: str | Path, cwd: Path | None = None) -> str:
    return subprocess.run(
        [str(c) for c in command], cwd=cwd, check=True, capture_output=True, text=True
    ).stdout


def release_tag(refs: ReferenceList) -> str:
    return f"pbrt-v4-reference/{refs.pbrt_v4.commit}"


class Assets(NamedTuple):
    """Release asset names of a Reference Scene, in upload order: the
    manifest last."""

    mean: str
    variance: str
    reference: str
    manifest: str

    @classmethod
    def of(cls, scene: ReferenceScene) -> Assets:
        return cls(
            mean=f"{scene.name}.mean.exr",
            variance=f"{scene.name}.variance.exr",
            reference=f"{scene.name}.reference.exr",
            manifest=f"{scene.name}.json",
        )


class Release:
    """The release `pbrt-v4-reference/<commit>` of this repository, through gh."""

    def __init__(self, tag: str) -> None:
        self.tag = tag
        completed = subprocess.run(
            ["gh", "release", "view", tag, "--json", "assets"],
            cwd=REPOSITORY_ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
        # The release's asset names, or None while there is no release.
        self.assets: set[str] | None
        if completed.returncode == 0:
            self.assets = {a["name"] for a in json.loads(completed.stdout)["assets"]}
        elif "release not found" in completed.stderr:
            self.assets = None
        else:
            raise PublishError(f"gh release view {tag} failed: {completed.stderr.strip()}")

    def published_manifest(self, scene: ReferenceScene) -> dict[str, Any] | None:
        names = Assets.of(scene)
        if self.assets is None or names.manifest not in self.assets:
            return None
        if missing := [name for name in names if name not in self.assets]:
            raise PublishError(
                f"{self.tag} has {names.manifest} but not {', '.join(missing)}; "
                "delete the Reference Scene's assets from the release to publish it again"
            )
        text = stdout_of(
            "gh", "release", "download", self.tag, "--pattern", names.manifest,
            "--output", "-", cwd=REPOSITORY_ROOT,
        )
        return json.loads(text)

    def upload(self, refs: ReferenceList, files: Sequence[Path]) -> None:
        if self.assets is None:
            run(
                "gh", "release", "create", self.tag,
                "--title", self.tag,
                "--notes", release_notes(refs),
                "--latest=false",
                cwd=REPOSITORY_ROOT,
            )
            self.assets = set()
        # One file at a time, the manifest last: a run cut short leaves the
        # Reference Scene unpublished, and the next run replaces its assets.
        for file in files:
            run("gh", "release", "upload", self.tag, file, "--clobber", cwd=REPOSITORY_ROOT)
            self.assets.add(file.name)


def release_notes(refs: ReferenceList) -> str:
    return (
        "The pbrt-v4 reference of `umbra validate` (ADR 0013), produced by "
        "`scripts/pbrt_reference.py`.\n\n"
        f"- pbrt-v4: {refs.pbrt_v4.repository} at `{refs.pbrt_v4.commit}`\n"
        f"- Scenes: {refs.pbrt_v4_scenes.repository} at `{refs.pbrt_v4_scenes.commit}`\n\n"
        "Per Reference Scene `<name>`: `<name>.mean.exr` and `<name>.variance.exr`, "
        f"the per-pixel mean and unbiased variance of {len(VALIDATION_SEEDS)} renders "
        f"with seeds {VALIDATION_SEEDS[0]}–{VALIDATION_SEEDS[-1]} at its validation "
        "settings, stored as 32-bit floats; `<name>.reference.exr`, its Reference Image "
        f"at {REFERENCE_IMAGE_FACTOR} × its benchmark Pixel Samples per pixel with seed "
        f"{REFERENCE_IMAGE_SEED}; and `<name>.json`, the inputs and the SHA-256 of each "
        "asset.\n"
    )


def checkout(pin: Pin, directory: Path, sparse: Sequence[str] = ()) -> None:
    """Fetches `pin` into `directory`, only the commit itself, and checks it
    out; with `sparse`, only those top-level directories."""
    if not (directory / ".git").exists():
        directory.mkdir(parents=True, exist_ok=True)
        run("git", "init", "--quiet", cwd=directory)
        run("git", "remote", "add", "origin", pin.repository, cwd=directory)
    if sparse:
        run("git", "sparse-checkout", "set", "--cone", *sparse, cwd=directory)
    head = subprocess.run(
        ["git", "rev-parse", "HEAD"], cwd=directory, capture_output=True, text=True, check=False
    ).stdout.strip()
    if head != pin.commit:
        log(f"fetching {pin.repository} at {pin.commit}")
        fetch = ["git", "fetch", "--depth", "1"]
        if sparse:
            fetch.append("--filter=blob:none")
        run(*fetch, "origin", pin.commit, cwd=directory)
    # Also picks up any directory newly added to the sparse checkout.
    run("git", "checkout", "--quiet", "--detach", pin.commit, cwd=directory)


@dataclass(frozen=True)
class Pbrt:
    """The pinned pbrt-v4's executables."""

    pbrt: Path
    imgtool: Path


def build_pbrt(pin: Pin, work: Path) -> Pbrt:
    source = work / "pbrt-v4"
    build = work / "pbrt-v4-build"
    checkout(pin, source)
    run("git", "submodule", "update", "--init", "--recursive", "--depth", "1", cwd=source)
    log("building pbrt-v4 (CPU)")
    # Without PBRT_OPTIX7_PATH, pbrt-v4 builds no GPU renderer. Its bundled
    # GLFW only serves the interactive viewer, so it needs no Wayland on Linux.
    run(
        "cmake", "-S", source, "-B", build,
        "-DCMAKE_BUILD_TYPE=Release", "-DGLFW_BUILD_WAYLAND=OFF",
    )
    run("cmake", "--build", build, "--parallel", "--target", "pbrt_exe", "imgtool")
    return Pbrt(pbrt=build / "pbrt", imgtool=build / "imgtool")


def render(
    pbrt: Pbrt, scene_file: Path, seed: int, pixel_samples: int, outfile: Path
) -> Image:
    run(
        pbrt.pbrt, "--quiet", "--seed", str(seed), "--spp", str(pixel_samples),
        "--outfile", outfile, scene_file,
    )
    return read_pfm(outfile.read_bytes())


def check_resolution(image: Image, settings: Settings, what: str) -> None:
    if (image.width, image.height) != settings.resolution:
        raise PublishError(
            f"{what} rendered at {image.width} × {image.height}, but its settings say "
            f"{settings.resolution[0]} × {settings.resolution[1]}; pbrt-v4 cannot "
            "override the resolution, so the Scene File (or the edit) must declare it"
        )


def to_exr(pbrt: Pbrt, image: Image, exr: Path) -> None:
    """Writes `image` as an EXR of 32-bit floats through pbrt-v4's imgtool."""
    pfm = exr.with_suffix(".pfm")
    pfm.write_bytes(write_pfm(image))
    run(pbrt.imgtool, "convert", "--outfile", exr, pfm)
    pfm.unlink()


def sha256(path: Path) -> str:
    with path.open("rb") as file:
        return hashlib.file_digest(file, "sha256").hexdigest()


def validation_statistics(
    scene: ReferenceScene, pbrt: Pbrt, scene_file: Path, renders: Path
) -> tuple[Image, Image]:
    """Renders `scene_file` with every validation seed and returns the
    per-pixel mean and variance."""
    validation = scene.validation
    images = []
    for seed in VALIDATION_SEEDS:
        log(f"{scene.name}: seed {seed}, {validation.pixel_samples} Pixel Samples per pixel")
        outfile = renders / f"seed-{seed:02}.pfm"
        image = render(pbrt, scene_file, seed, validation.pixel_samples, outfile)
        check_resolution(image, validation, f"{scene.name} (validation)")
        images.append(image)
    return pixel_statistics(images)


def produce(
    refs: ReferenceList, scene: ReferenceScene, pbrt: Pbrt, scenes: Path, out: Path
) -> list[Path]:
    """Renders a Reference Scene's reference into `out` and returns its
    assets, the manifest last."""
    original = scenes / scene.scene_file
    edited = original.with_name(f"{original.stem}.umbra-reference.{scene.name}.pbrt")
    try:
        text = edit_scene(original.read_text(encoding="utf-8"), scene.edit)
    except SceneEditError as error:
        raise PublishError(f"{scene.name}: {error}") from error
    # Next to the original, so that its Includes and assets resolve.
    edited.write_text(text, encoding="utf-8")

    renders = out / f"{scene.name}.renders"
    if renders.exists():
        shutil.rmtree(renders)
    renders.mkdir(parents=True)
    mean, variance = validation_statistics(scene, pbrt, edited, renders)
    reference_samples = REFERENCE_IMAGE_FACTOR * scene.benchmark.pixel_samples
    log(f"{scene.name}: Reference Image, {reference_samples} Pixel Samples per pixel")
    reference = render(
        pbrt, edited, REFERENCE_IMAGE_SEED, reference_samples, renders / "reference.pfm"
    )
    check_resolution(reference, scene.benchmark, f"{scene.name} (Reference Image)")

    names = Assets.of(scene)
    exrs = [out / names.mean, out / names.variance, out / names.reference]
    for image, exr in zip((mean, variance, reference), exrs, strict=True):
        to_exr(pbrt, image, exr)
    manifest_json = out / names.manifest
    hashes = {exr.name: sha256(exr) for exr in exrs}
    manifest_json.write_text(
        json.dumps(manifest(refs, scene, hashes), indent=2) + "\n", encoding="utf-8"
    )
    shutil.rmtree(renders)
    return [*exrs, manifest_json]


class Publication(Enum):
    """Whether a run publishes what it renders."""

    PUBLISH = "publish"
    LOCAL_ONLY = "local-only"


def parse_arguments(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Build the pinned pbrt-v4, render the reference statistics of the "
            "Reference Scenes and publish them as the release "
            "pbrt-v4-reference/<commit>. Scenes already published with the same "
            "inputs are skipped."
        )
    )
    parser.add_argument(
        "--list",
        type=Path,
        default=REPOSITORY_ROOT / "reference" / "scenes.toml",
        help="the reference list (default: reference/scenes.toml)",
    )
    parser.add_argument(
        "--work-dir",
        type=Path,
        default=REPOSITORY_ROOT / "target" / "pbrt-reference",
        help="where pbrt-v4, the scenes and the outputs go (default: target/pbrt-reference)",
    )
    parser.add_argument(
        "--scene",
        action="append",
        default=[],
        metavar="NAME",
        help="only this Reference Scene (repeatable; default: all)",
    )
    parser.add_argument(
        "--no-publish",
        dest="publication",
        action="store_const",
        const=Publication.LOCAL_ONLY,
        default=Publication.PUBLISH,
        help="render into the work directory without reading or touching the release",
    )
    return parser.parse_args(argv)


def scenes_to_render(
    refs: ReferenceList, selected: Sequence[ReferenceScene], release: Release | None
) -> list[ReferenceScene]:
    """The selected Reference Scenes that `release` lacks; all of them when
    nothing is published."""
    if release is None:
        return list(selected)
    pending = []
    for scene in selected:
        match plan(refs, scene, release.published_manifest(scene)):
            case Action.SKIP:
                log(f"{scene.name}: already published in {release.tag}, skipped")
            case Action.RENDER:
                pending.append(scene)
    return pending


def render_and_publish(
    refs: ReferenceList,
    pending: Sequence[ReferenceScene],
    release: Release | None,
    work: Path,
) -> None:
    pbrt = build_pbrt(refs.pbrt_v4, work)
    scenes = work / "pbrt-v4-scenes"
    top_level = sorted({PurePosixPath(s.scene_file).parts[0] for s in pending})
    checkout(refs.pbrt_v4_scenes, scenes, sparse=top_level)
    out = work / "out"
    out.mkdir(parents=True, exist_ok=True)
    for scene in pending:
        files = produce(refs, scene, pbrt, scenes, out)
        if release is not None:
            log(f"{scene.name}: publishing to {release.tag}")
            release.upload(refs, files)
        log(f"{scene.name}: done, {', '.join(f.name for f in files)} in {out}")


def main(argv: Sequence[str]) -> int:
    arguments = parse_arguments(argv)
    try:
        refs = load_reference_list(arguments.list.read_text(encoding="utf-8"))
    except (OSError, ReferenceListError) as error:
        log(f"{arguments.list}: {error}")
        return 1
    selected = [s for s in refs.scenes if not arguments.scene or s.name in arguments.scene]
    if unknown := sorted(set(arguments.scene) - {s.name for s in refs.scenes}):
        log(f"not in the reference list: {', '.join(unknown)}")
        return 1

    try:
        release = (
            Release(release_tag(refs))
            if arguments.publication is Publication.PUBLISH
            else None
        )
        pending = scenes_to_render(refs, selected, release)
        if pending:
            render_and_publish(refs, pending, release, arguments.work_dir.resolve())
        else:
            log("nothing to render")
    except (PublishError, ImageError) as error:
        log(str(error))
        return 1
    except subprocess.CalledProcessError as error:
        log(f"command failed ({error.returncode}): {' '.join(map(str, error.cmd))}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
