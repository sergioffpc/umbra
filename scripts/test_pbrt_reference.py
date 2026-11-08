"""Tests of the pbrt-v4 reference script (scripts/pbrt_reference.py).

Run with: python3 -m unittest discover -s scripts
"""

import json
from array import array
import textwrap
import unittest
from pathlib import Path

import pbrt_reference as ref

COMMITTED_LIST = Path(__file__).resolve().parent.parent / "reference-scenes.toml"

PBRT_V4 = "b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f"
SCENES = "30cf4a0346ae5a80a2d7a530a3ef7d0fa4f70572"

PINS = f"""
[pbrt-v4]
repository = "https://github.com/mmp/pbrt-v4"
commit = "{PBRT_V4}"

[pbrt-v4-scenes]
repository = "https://github.com/mmp/pbrt-v4-scenes"
commit = "{SCENES}"
"""

KILLEROO = """
[[reference-scene]]
name = "killeroo-simple"
scene-file = "killeroos/killeroo-simple.pbrt"

[reference-scene.benchmark]
resolution = [700, 700]
pixel-samples = 1024
seed = 0
tile-size = 32

[reference-scene.validation]
resolution = [700, 700]
pixel-samples = 64
tile-size = 32

[reference-scene.golden]
resolution = [700, 700]
pixel-samples = 4
seed = 0
tile-size = 32
"""


def toml(*parts: str) -> str:
    return "".join(textwrap.dedent(p) for p in parts)


class LoadReferenceList(unittest.TestCase):
    def test_reads_the_pins_and_a_reference_scene(self) -> None:
        refs = ref.load_reference_list(toml(PINS, KILLEROO))

        self.assertEqual(refs.pbrt_v4.repository, "https://github.com/mmp/pbrt-v4")
        self.assertEqual(refs.pbrt_v4.commit, PBRT_V4)
        self.assertEqual(refs.pbrt_v4_scenes.commit, SCENES)
        [scene] = refs.scenes
        self.assertEqual(scene.name, "killeroo-simple")
        self.assertEqual(scene.scene_file, "killeroos/killeroo-simple.pbrt")
        self.assertEqual(scene.validation.resolution, (700, 700))
        self.assertEqual(scene.validation.pixel_samples, 64)
        self.assertEqual(scene.benchmark.pixel_samples, 1024)
        self.assertIsNone(scene.edit)

    def test_reads_the_declared_edit_of_a_variant(self) -> None:
        variant = KILLEROO.replace('"killeroo-simple"', '"killeroo-simple-box"') + (
            "\n[reference-scene.edit]\n"
            "find = 'PixelFilter \"gaussian\"'\n"
            "replace = 'PixelFilter \"box\"'\n"
        )

        refs = ref.load_reference_list(toml(PINS, KILLEROO, variant))

        self.assertEqual(
            refs.scenes[1].edit,
            ref.Edit(find='PixelFilter "gaussian"', replace='PixelFilter "box"'),
        )

    def test_rejects_a_malformed_list(self) -> None:
        cases = {
            "missing pin": toml(PINS.replace("[pbrt-v4-scenes]", "[other]"), KILLEROO),
            "short commit": toml(PINS.replace(PBRT_V4, "b0bdf46"), KILLEROO),
            "no scenes": toml(PINS),
            "missing settings": toml(
                PINS, KILLEROO.replace("[reference-scene.validation]", "[reference-scene.other]")
            ),
            "zero pixel samples": toml(
                PINS, KILLEROO.replace("pixel-samples = 64", "pixel-samples = 0")
            ),
            "bad resolution": toml(PINS, KILLEROO.replace("[700, 700]", "[700]", 2)),
            "benchmark and validation resolutions differ": toml(
                PINS, KILLEROO.replace("[700, 700]", "[350, 350]", 1)
            ),
            "duplicate name": toml(PINS, KILLEROO, KILLEROO),
            "name not a file name": toml(
                PINS, KILLEROO.replace('"killeroo-simple"', '"../killeroo"')
            ),
            "scene file outside the collection": toml(
                PINS, KILLEROO.replace('"killeroos/', '"../killeroos/')
            ),
            "absolute scene file": toml(PINS, KILLEROO.replace('"killeroos/', '"/killeroos/')),
            "empty find": toml(
                PINS, KILLEROO, "[reference-scene.edit]\nfind = ''\nreplace = 'x'\n"
            ),
        }
        for case, text in cases.items():
            with self.subTest(case), self.assertRaises(ref.ReferenceListError):
                ref.load_reference_list(text)

    def test_the_committed_list_is_well_formed(self) -> None:
        refs = ref.load_reference_list(COMMITTED_LIST.read_text(encoding="utf-8"))

        self.assertEqual(refs.pbrt_v4.commit, PBRT_V4)
        self.assertEqual(refs.pbrt_v4_scenes.commit, SCENES)
        self.assertIn("killeroo-simple", [scene.name for scene in refs.scenes])


SCENE_FILE = """\
LookAt 400 20 30  0 63 -110  0 0 1
Camera "perspective" "float fov" [ 39 ]
# Film "gbuffer" in a comment is not a directive
Film "rgb"
    "string filename" [ "killeroo-simple.exr" ]
PixelFilter "gaussian"
WorldBegin
"""


class EditScene(unittest.TestCase):
    def test_forces_float_pixels_on_the_film(self) -> None:
        self.assertEqual(
            ref.edit_scene(SCENE_FILE, None),
            SCENE_FILE.replace(
                'Film "rgb"\n', 'Film "rgb" "bool savefp16" [ false ]\n'
            ),
        )

    def test_applies_a_variants_edit(self) -> None:
        edit = ref.Edit(find='PixelFilter "gaussian"', replace='PixelFilter "box"')

        edited = ref.edit_scene(SCENE_FILE, edit)

        self.assertIn('\nPixelFilter "box"\n', edited)
        self.assertNotIn("gaussian", edited)
        self.assertIn('Film "rgb" "bool savefp16" [ false ]\n', edited)

    def test_refuses_an_edit_it_cannot_apply_unambiguously(self) -> None:
        cases = {
            "edit matches nothing": (SCENE_FILE, ref.Edit(find='"box"', replace='"mitchell"')),
            "edit matches twice": (SCENE_FILE, ref.Edit(find="Film", replace="Film")),
            "no Film directive": (SCENE_FILE.replace('Film "rgb"', ""), None),
            "two Film directives": (SCENE_FILE + 'Film "rgb"\n', None),
            "savefp16 already set": (
                SCENE_FILE.replace('Film "rgb"', 'Film "rgb" "bool savefp16" [ true ]'),
                None,
            ),
        }
        for case, (text, edit) in cases.items():
            with self.subTest(case), self.assertRaises(ref.SceneEditError):
                ref.edit_scene(text, edit)


def rgb(*pixels: tuple[float, float, float], width: int | None = None) -> ref.Image:
    """An image of the given RGB pixels, one row unless `width` is given."""
    width = width or len(pixels)
    return ref.Image(
        width=width,
        height=len(pixels) // width,
        values=array("d", (c for pixel in pixels for c in pixel)),
    )


class Pfm(unittest.TestCase):
    # 2 × 1 RGB, little-endian (negative scale), floats 1.0 0.5 -2.0 0.25 0 3.0.
    LITTLE = (
        b"PF\n2 1\n-1.000000\n"
        + bytes.fromhex("0000803f 0000003f 000000c0 0000803e 00000000 00004040")
    )

    def test_reads_a_little_endian_pfm(self) -> None:
        self.assertEqual(
            ref.read_pfm(self.LITTLE), rgb((1.0, 0.5, -2.0), (0.25, 0.0, 3.0))
        )

    def test_reads_a_big_endian_pfm(self) -> None:
        big = b"PF\n1 2\n1\n" + bytes.fromhex(
            "3f800000 3f000000 c0000000 3e800000 00000000 40400000"
        )

        self.assertEqual(ref.read_pfm(big), rgb((1.0, 0.5, -2.0), (0.25, 0.0, 3.0), width=1))

    def test_writes_a_little_endian_pfm(self) -> None:
        self.assertEqual(
            ref.write_pfm(rgb((1.0, 0.5, -2.0), (0.25, 0.0, 3.0))),
            b"PF\n2 1\n-1\n" + self.LITTLE.split(b"-1.000000\n")[1],
        )

    def test_refuses_what_it_cannot_read(self) -> None:
        cases = {
            "grayscale": b"Pf\n1 1\n-1\n" + bytes(4),
            "truncated": self.LITTLE[:-1],
            "scaled": self.LITTLE.replace(b"-1.000000", b"-2.0"),
            "not a pfm": b"P6\n1 1\n255\n" + bytes(3),
        }
        for case, data in cases.items():
            with self.subTest(case), self.assertRaises(ref.ImageError):
                ref.read_pfm(data)


class PixelStatistics(unittest.TestCase):
    def test_mean_and_unbiased_variance_per_pixel_and_channel(self) -> None:
        seeds = [
            rgb((1.0, 0.5, 0.0), (0.0, 0.0, 10.0)),
            rgb((2.0, 0.5, 0.0), (0.0, 0.0, 20.0)),
            rgb((6.0, 0.5, 3.0), (0.0, 0.0, 30.0)),
        ]

        mean, variance = ref.pixel_statistics(seeds)

        self.assertEqual(mean, rgb((3.0, 0.5, 1.0), (0.0, 0.0, 20.0)))
        self.assertEqual(variance, rgb((7.0, 0.0, 3.0), (0.0, 0.0, 100.0)))

    def test_refuses_images_it_cannot_combine(self) -> None:
        cases = {
            "one image": [rgb((1.0, 1.0, 1.0))],
            "different resolutions": [rgb((1.0, 1.0, 1.0)), rgb((1.0, 1.0, 1.0), (1.0, 1.0, 1.0))],
            "infinite value": [rgb((1.0, 1.0, 1.0)), rgb((float("inf"), 1.0, 1.0))],
            "NaN value": [rgb((1.0, 1.0, 1.0)), rgb((float("nan"), 1.0, 1.0))],
        }
        for case, images in cases.items():
            with self.subTest(case), self.assertRaises(ref.ImageError):
                ref.pixel_statistics(images)


class PublishPlan(unittest.TestCase):
    HASHES = {"killeroo-simple.mean.exr": "ab" * 32}

    def setUp(self) -> None:
        self.refs = ref.load_reference_list(toml(PINS, KILLEROO))
        [self.scene] = self.refs.scenes

    def published(self, text: str) -> dict[str, object]:
        """The manifest a run with reference list `text` would have published."""
        refs = ref.load_reference_list(text)
        manifest = ref.manifest(refs, refs.scenes[0], self.HASHES)
        return json.loads(json.dumps(manifest))

    def test_the_manifest_records_the_film_edit_only_pbrt_v4_gets(self) -> None:
        manifest = ref.manifest(self.refs, self.scene, self.HASHES)

        self.assertEqual(manifest["pbrt-v4-film-parameters"], '"bool savefp16" [ false ]')
        self.assertEqual(manifest["sha256"], self.HASHES)

    def test_renders_a_scene_not_yet_published(self) -> None:
        self.assertEqual(ref.plan(self.refs, self.scene, None), ref.Action.RENDER)

    def test_skips_a_scene_published_with_the_same_inputs(self) -> None:
        published = self.published(toml(PINS, KILLEROO))

        self.assertEqual(ref.plan(self.refs, self.scene, published), ref.Action.SKIP)

    def test_refuses_to_mix_inputs_with_a_published_scene(self) -> None:
        cases = {
            "validation pixel samples": toml(
                PINS, KILLEROO.replace("pixel-samples = 64", "pixel-samples = 128")
            ),
            "benchmark pixel samples": toml(
                PINS, KILLEROO.replace("pixel-samples = 1024", "pixel-samples = 512")
            ),
            "scenes commit": toml(PINS.replace(SCENES, "f" * 40), KILLEROO),
            "edit": toml(
                PINS, KILLEROO, "[reference-scene.edit]\nfind = 'a'\nreplace = 'b'\n"
            ),
        }
        for case, text in cases.items():
            with self.subTest(case), self.assertRaises(ref.PublishError):
                ref.plan(self.refs, self.scene, self.published(text))


if __name__ == "__main__":
    unittest.main()
