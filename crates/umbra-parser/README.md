# umbra-parser

The adapter from a pbrt-v4 Scene File to a Scene: it tokenises the `.pbrt`
file with locations, resolves its includes as pbrt-v4 does, validates it
strictly, applies pbrt-v4's command-line overrides, and returns either the
Scene with its Render Job Options, its output path and the content hash of
the Scene File, its includes and its assets, or every `ParserError`, with
nothing built (ADR 0003). `parse` is its one public function.

It is a `std` crate, outside the core. It depends on `umbra-scene`, and only
the `umbra` binary depends on it: `umbra-cluster` receives an already parsed
Scene (arc42 §5.1). Its contract is sketched in arc42 §5.3.

```rust
use std::fs;
use umbra_parser::{Overrides, parse};
use umbra_scene::Seed;

let dir = std::env::temp_dir().join("umbra-parser-readme");
fs::create_dir_all(&dir)?;
let scene_file = dir.join("scene.pbrt");
fs::write(
    &scene_file,
    r#"LookAt 0 0 5  0 0 0  0 1 0
Camera "perspective" "float fov" 45
Film "rgb" "integer xresolution" 64 "integer yresolution" 48
WorldBegin
"#,
)?;
let overrides = Overrides { seed: Some(Seed(3)), ..Overrides::default() };
let parsed = parse(&scene_file, &overrides)?;
assert_eq!(parsed.options.seed.0, 3);
assert_eq!(parsed.output, std::path::PathBuf::from("pbrt.exr"));
assert!(parsed.warnings.is_empty());
# Ok::<(), Box<dyn std::error::Error>>(())
```

## What is in it

| Module         | Contents                                                                                                                                                                                                     |
| -------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `lib`          | `parse`, `Overrides` (`--spp`, `--seed` as a `Seed`, `--outfile`), `Parsed`                                                                                                                                  |
| `error`        | `ParserError`, with its `Severity` and `Location` (`file:line:column`), and `ParseError`, what `parse` returns on failure                                                                                    |
| `tokenizer`    | pbrt-v4 `Tokenizer` (`src/pbrt/parser.cpp`) with `nom` and `nom_locate`: brackets, quoted strings with their escapes, bare words; comments skipped                                                           |
| `syntax`       | pbrt-v4 `parse` (`parser.cpp`): directives, parameter lists and `Include`, handed to a `ParserTarget`; unsupported and unknown directives refused with their arguments skipped, so every error is reported   |
| `params`       | pbrt-v4 `ParameterDictionary` (`src/pbrt/paramdict.h`): typed lookups that consume what they read; what is left is refused when pbrt-v4 honours it and Umbra does not yet, and warned of otherwise           |
| `scene_target` | pbrt-v4 `BasicSceneBuilder` (`src/pbrt/scene.cpp`): the current transformation, the block, and each supported directive's type and parameters, into a `SceneBuilder`; the output path and Render Job Options |
| `files`        | pbrt-v4 `ResolveFilename`: paths relative to the Scene File's directory; include cycles; the SHA-256 content hash                                                                                            |

The subset so far is the camera-only Scene File of
[#78](https://github.com/sergioffpc/umbra/issues/78): `LookAt`, `Camera
"perspective"`, `Film "rgb"` (`xresolution`, `yresolution`, `filename`),
`Sampler "halton"`, `PixelFilter "gaussian"`, `Integrator "volpath"`,
`WorldBegin` and `Include`. Shapes, lights and materials arrive from
[#79](https://github.com/sergioffpc/umbra/issues/79) on, and the other
`RGBFilm` parameters with
[#84](https://github.com/sergioffpc/umbra/issues/84).

## Design decisions

### Two layers behind `ParserTarget`

The grammar knows directives and parameter lists, nothing of what they
mean; `SceneTarget` knows the graphics state and the supported types,
nothing of tokens. They meet at `ParserTarget`, as pbrt-v4's parser and
scene builder do, so the tokenizer can change without touching validation
(ADR 0003).

### Report everything, build nothing

Every check pushes a `ParserError` and carries on with pbrt-v4's default, so
one parse lists every error. An unsupported directive is skipped with its
arguments: everything up to the next bare word that is not a number or a
boolean. `parse` builds the Scene only if there is no error.

### Unsupported or unused

A parameter that pbrt-v4 honours for a type and Umbra does not yet is an
error: the render would differ from pbrt-v4's. A parameter that no one reads
is a warning, as pbrt-v4 reports unused parameters. A value that pbrt-v4
accepts but Umbra cannot honour (`"bool regularize" true`, a light sampler
other than `bvh`) is an error at that parameter.

### Overrides inside the parse

`--outfile` replaces the Film's `filename`, so whether the output is an EXR
can be checked only with it; `--spp` and `--seed` replace the sampler's
`pixelsamples` and `seed`. So `parse` takes the overrides, and its output
is final. The Tile size and Pixel Samples per Pass are never in a Scene
File: `Parsed::options` holds their defaults, and the binary sets them.

### The content hash

SHA-256 over every file read, each as its length in little-endian `u64`
then its bytes, in the order read: the Scene File, then each include where
it is included. The paths are not hashed, so moving the files keeps the
hash.

## Things to know when using it

- **Relative paths resolve against the Scene File's directory**, as
  pbrt-v4's search directory: an `Include` in an included file too, and the
  Film's `filename`. `--outfile` is relative to the working directory.
- **The output path defaults to `pbrt.exr`** in the working directory, and
  must end in `.exr` (arc42 §8.15).
- **The seed is `--seed`, else the sampler's `seed`, else 0.** pbrt-v4
  lets the sampler's parameter win; Umbra lets the override win (ADR 0003).
- **`ParseError::Io` is for the Scene File itself.** An include that cannot
  be read is a `ParserError` at its `Include`.
- **A `ParserError`'s column counts characters**, from 1.

## Tests

Through the binary, in `crates/umbra/tests/render/` (spec #72, Seam 1):
located Diagnostics for unsupported directives, types, parameters and
values, all listed at once; errors located in an include, and at an
`Include` that cannot be read; an output that is not an EXR; a warning for
an unused parameter; includes resolved and hashed; the overrides and the
seed's precedence.
