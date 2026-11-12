# umbra

The `umbra` binary: the `render`, `worker`, `trace` and `bench` subcommands,
and how they put the other crates together. It parses the Scene File with
`umbra-parser`, applies the command-line overrides, builds the Reproduction
Record, and calls `umbra-cluster`, or `umbra-render` for `trace`. The
statistics of `bench` are modules of the binary.

It is the root of the workspace graph: it depends on `umbra-parser`,
`umbra-render` and `umbra-cluster`, and nothing depends on it (arc42 §5.1).
Its contract is sketched in arc42 §5.3. What Umbra is, how to build it and
how to run Workers on several machines are in the
[repository's README](https://github.com/sergioffpc/umbra#readme).

```sh
umbra render scene.pbrt --spp 16 --seed 0 --outfile out.exr --tile-size 32 --pixel-samples-per-pass 64
```

## What is in it

| Module           | Contents                                                                                                       |
| ---------------- | -------------------------------------------------------------------------------------------------------------- |
| `main`           | The exit codes of arc42 §8.13, and a panic hook that exits 1                                                   |
| `cli`            | The `clap` command line: `render` with pbrt-v4's `--spp`, `--seed`, `--outfile`, `--nthreads`, and Umbra's own |
| `render`         | `umbra render`: parse, set the Render Job Options, `coordinate`, write the EXR and `<image>.report.json`       |
| `build_identity` | The Build Identity that `build.rs` embeds: commit, clean or dirty tree, target CPU                             |

`build.rs` reads the commit and tree from git, or from `UMBRA_COMMIT` and
`UMBRA_TREE` where there is no `.git` (a container build), and the target CPU
from `-C target-cpu` in the compilation's flags.

Not here yet: Ctrl-C and exit code 130, `--log` and `--log-format`, with
[#86](https://github.com/sergioffpc/umbra/issues/86); `trace`, with
[#88](https://github.com/sergioffpc/umbra/issues/88); `bench`, with
[#93](https://github.com/sergioffpc/umbra/issues/93); `validate`, with
[#92](https://github.com/sergioffpc/umbra/issues/92); `worker`, with
[#129](https://github.com/sergioffpc/umbra/issues/129).

## Design decisions

### pbrt-v4's names for pbrt-v4's options

`--spp`, `--seed`, `--outfile` and `--nthreads` mean what they mean to
pbrt-v4 (ADR 0003); `--tile-size` and `--pixel-samples-per-pass` are
Umbra's Render Job Options, never in a Scene File.

### The record names the Scene File by its canonical path

The Reproduction Record holds the Scene File's absolute, canonical path, so
`umbra trace` finds it from any working directory.

### Writes are atomic

The image and its report are each written to a temporary file in their
directory, synced, and renamed over the target (arc42 §8.15).

## Things to know when using it

- **Exit codes** (arc42 §8.13): 0 rendered; 1 an I/O error or a panic,
  including a Scene File that cannot be read; 2 invalid usage; 3 an invalid
  Scene File, with every Diagnostic on stderr as `file:line:column:
error: ...`.
- **Warnings and the final status go to stderr** with `writeln!` until
  `tracing` arrives with #86.
- **The Build Identity's tree is checked when the git index or `HEAD`
  changes**, not on every edit: a binary rebuilt after an unstaged edit alone
  may still say `clean`.

## Tests

The integration tests are the cross-crate tests of arc42 §5.4:

- `tests/render/`, `umbra render` through the binary (spec #72, Seam 1): a
  camera-only Scene File renders a black RGB EXR and a report with
  `outcome: "finished"`; the EXR's bytes are the same on every run; it
  carries the Reproduction Record, with the overrides; the seed's
  precedence; the Film hash is the same with 1 thread and with all; output
  paths resolve as in pbrt-v4; includes resolve and are hashed; unsupported
  Scene Files give located Diagnostics and exit 3, with no image; unused
  parameters warn; a missing Scene File exits 1 and invalid usage 2.
- `tests/allocation.rs`, the counting-allocator test of ADR 0008, which
  renders Work Units with N and 2N Pixel Samples and requires the same
  number of heap allocations.
