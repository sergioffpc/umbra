# umbra

The `umbra` binary: the `render`, `worker`, `trace` and `bench` subcommands,
and how they put the other crates together. All of it is in the library,
`umbra::run`; `main.rs` only calls it, and the tests in `tests/` use the
library's public items. It parses the Scene File with
`umbra-parser`, applies the command-line overrides, builds the Reproduction
Record, and calls `umbra-cluster`, or `umbra-cpu` for `trace`. The
statistics of `bench` are modules of the binary.

It is the root of the workspace graph: it depends on `umbra-parser`,
`umbra-render` and `umbra-cluster`, and nothing depends on it (arc42 §5.1).
Its contract is sketched in arc42 §5.3. What Umbra is, how to build it and
how to run Workers on several machines are in the
[repository's README](https://github.com/sergioffpc/umbra#readme).

```sh
umbra render scene.pbrt --spp 16 --seed 0 --outfile out.exr --tile-size 32 --spp-per-pass 64
```

## What is in it

| Module           | Contents                                                                                                                     |
| ---------------- | ---------------------------------------------------------------------------------------------------------------------------- |
| `lib`            | `run`: the exit codes of arc42 §8.13, a panic hook that exits 1, and the version line on stderr at start and for `--version` |
| `cli`            | The `clap` command line: `render` with pbrt-v4's `--spp`, `--seed`, `--outfile`, `--nthreads`, and Umbra's own               |
| `render`         | `umbra render`: parse, set the Render Job Options, `coordinate`, write the image, and the Render Job Report to stdout        |
| `error`          | `Error`: why `umbra render` did not write an image, and the exit code it maps to                                             |
| `logs`           | `tracing` to stderr: human-readable or JSON lines, filtered by `--log`, and to Tracy with the `tracy` feature                |
| `footer`         | `Footer`, public: stderr with the status line kept at its bottom by `indicatif`, log events printed above it                 |
| `build_identity` | The Build Identity that `build.rs` embeds: commit, clean or dirty tree; the version line and the `--version` notice          |

`build.rs` reads the commit and tree from git, or from `UMBRA_COMMIT` and
`UMBRA_TREE` where there is no `.git` (a container build).

Every command starts by writing its version line to stderr,
`umbra 0.1.0 (9bd75ce)`, with `-dirty` after the commit for a dirty tree.
`umbra --version` (or `-V`) writes it to stderr too, not to stdout as
`clap` would, followed by the copyright and the licences:

```text
umbra 0.1.0 (9bd75ce)
Copyright (c) 2026 Sérgio Carvalho
MIT License. Includes code from pbrt-v4, under the Apache License 2.0; see NOTICE.
```

While a Render Job runs, the status line is redrawn at the bottom of
stderr when it is a terminal that can move its cursor (not `TERM=dumb`),
each line cut to the terminal's width, with log events printed above it;
otherwise it is logged as `info` events about every 10 s. Logs go
to stderr through `tracing`: human-readable on a terminal and JSON lines
otherwise, or as `--log-format human|json` says, filtered by `--log`, a
`RUST_LOG`-style filter (`info` by default). Ctrl-C cancels the Render
Job: its report says `Render Job: cancelled`, no image is written, and
`umbra` exits 130; a second Ctrl-C exits at once. The `tracy` feature, off
by default, sends the `tracing` spans to Tracy.

Not here yet: `trace`, with
[#88](https://github.com/sergioffpc/umbra/issues/88); `bench`, with
[#93](https://github.com/sergioffpc/umbra/issues/93); `validate`, with
[#92](https://github.com/sergioffpc/umbra/issues/92); `worker`, with
[#129](https://github.com/sergioffpc/umbra/issues/129).

## Design decisions

### pbrt-v4's names for pbrt-v4's options

`--spp`, `--seed`, `--outfile` and `--nthreads` mean what they mean to
pbrt-v4 (ADR 0003); `--tile-size` and `--spp-per-pass` are Umbra's Render
Job Options, never in a Scene File. `--spp-per-pass` is named after `--spp`
because it counts the same Pixel Samples: see
[`--spp` and `--spp-per-pass`](#--spp-and---spp-per-pass).

### The record names the Scene File by its canonical path

The Reproduction Record holds the Scene File's absolute, canonical path, so
`umbra trace` finds it from any working directory.

### Writes are atomic

The image is written to a temporary file in its directory, synced, and
renamed over the target (arc42 §8.15).

### The report is text on stdout

The Render Job Report is the command's output: one `Name: value` line per
figure on stdout, and nothing else is written there. Logs, the status line
and Diagnostics go to stderr. There is no report file and no JSON for now;
`umbra render scene.pbrt > report.txt` keeps one.

## `--spp` and `--spp-per-pass`

Both count Pixel Samples per pixel, but they answer different questions.

| Option           | Question                                     | Changes the image?                    | Default                                    |
| ---------------- | -------------------------------------------- | ------------------------------------- | ------------------------------------------ |
| `--spp`          | How many Pixel Samples does every pixel get? | Yes: more Pixel Samples, less noise   | the Sampler's `pixelsamples`, otherwise 16 |
| `--spp-per-pass` | How many of them go into one Pass?           | No: the same Pixel Samples, cut apart | 64                                         |

`--spp` sets quality. Each Pixel Sample is one estimate of the light a
pixel receives, and the pixel is their weighted average, so more of them
mean less noise and a longer render. It is pbrt-v4's option and overrides
the Scene File.

`--spp-per-pass` only cuts the work. Pass p holds the sample indices
`p × spp-per-pass` up to the next Pass, in every pixel; the last Pass stops
at `--spp`. One Tile for one Pass is a Work Unit, the piece a Worker renders
and the Film merges. With `--spp 256 --spp-per-pass 64`:

```text
sample index  0 ........ 63 | 64 ...... 127 | 128 ..... 191 | 192 ..... 255
              Pass 0        | Pass 1        | Pass 2        | Pass 3
```

Every pixel still gets 256 Pixel Samples. A 1920 × 1080 image in 32 × 32
Tiles has 60 × 34 = 2040 Tiles, so 4 × 2040 = 8160 Work Units. With
`--spp-per-pass 256` it would be one Pass and 2040 larger Work Units.

- **Smaller Passes** give more and smaller Work Units: a Worker that fails
  loses less work, and the end of the render waits less on one slow Work
  Unit (ADR 0009). Each Work Unit carries a fixed cost, so very small Passes
  spend more on overhead.
- **The noise does not depend on it**, but the image's last bits can: the
  Film adds the Pixel Samples Pass by Pass, and a different grouping rounds
  differently. Same options on one machine give the same bits (ADR 0022).
- **A value of `--spp` or more** renders in one Pass, so the order in which
  Work Results arrive cannot change the Film. The golden renders of ADR 0022
  will use one Pass for that reason
  ([#87](https://github.com/sergioffpc/umbra/issues/87)).

The Reproduction Record in the Render Job Report shows them as `spp` and
`spp_per_pass`, the names of the `RenderJobOptions` fields they set.

`--spp-per-pass` does not decide how a Worker fills its cores: it asks for
about two Work Units per eight threads and shares each one's rows among
them. The Pass size matters there only when Tiles × Passes is smaller than
what the Workers ask for, as with a small image on a Cluster. The
[Render Guide](../umbra-render/docs/render-guide.html#cores) shows this with
an interactive calculator.

## Things to know when using it

- **Exit codes** (arc42 §8.13): 0 rendered; 1 an I/O error or a panic,
  including a Scene File that cannot be read; 2 invalid usage; 3 an invalid
  Scene File, with every `Diagnostic` on stderr as `file:line:column:
error: ...`; 130 cancelled by Ctrl-C.
- **The version line, Diagnostics and errors are plain text on stderr**,
  written with `writeln!`, whatever `--log-format`: they are the command's
  own output, not log events.
- **The Build Identity's tree is checked when the git index or `HEAD`
  changes**, not on every edit: a binary rebuilt after an unstaged edit alone
  may still say `clean`.

## Tests

The integration tests are the cross-crate tests of arc42 §5.4:

- `tests/render/`, `umbra render` through the binary (spec #72, Seam 1): a
  camera-only Scene File renders a black RGB EXR, and a report on stdout
  that says `Render Job: finished` and writes no report file; the EXR's bytes are the same on every run; the
  report carries the Reproduction Record, with the overrides; the seed's
  precedence; the Film hash is the same with 1 thread and with all; output
  paths resolve as in pbrt-v4; includes resolve and are hashed; unsupported
  Scene Files give located Diagnostics and exit 3, with no image; unused
  parameters warn; a missing Scene File exits 1 and invalid usage 2; the
  version line starts every command on stderr, and `--version` adds the
  copyright and the licences, with nothing on stdout; the report has the
  options with the output path, phase times, Pixel Samples and their
  rate, the Local Worker's host, CPU and figures, and a cost per Tile;
  logs are JSON lines without a terminal, text with `--log-format human`,
  and filtered by `--log`, with the final status among them; Ctrl-C writes
  a cancelled report, no image, and exits 130.
- `tests/allocation.rs`, the counting-allocator test of ADR 0008, which
  renders Work Units with N and 2N Pixel Samples and requires the same
  number of heap allocations.
- `tests/footer.rs`, `Footer` on `indicatif`'s in-memory terminal, by
  what its screen shows: a status update replaces the last one, a log
  event prints above the status line, which is redrawn below it, a line
  wider than the terminal is cut to its width, and the last status stays
  once released.
