# Diagnostics go to stderr, one line each; debug and trace on demand

umbra is a command-line program. Results go to stdout, and only results, so
that they can be piped. Diagnostics go to stderr, one line each, as plain
text:

```
fatal: cannot open 'scene.usda': No such file or directory
error: unknown device 'gpu3'; run umbra devices to list them
warning: /World/Lamp: unsupported light type 'PortalLight', ignored
Rendering 1920x1080 on cpu0, gpu0
debug: usd: stage opened in 120 ms (3 layers)
12:34:56.789012 src/gpu/module.cc:161   trace: gpu: frame 12: 4096 spp, 38.2 ms
```

The levels are fatal, error, warning, info, debug and trace. Fatal is the
error umbra gives up on, after which it exits. Fatal errors, errors and
warnings are prefixed with their level. Info is what umbra tells the user
about its work, without a prefix. All four are always shown, but `-q` shows
errors only.

Debug and trace are the details of umbra's work and of its libraries (their
versions, the devices they found), for someone diagnosing a problem. They
are prefixed with their level and their component, and shown only when the
`UMBRA_LOG` variable asks for them, for every component or per component:
`UMBRA_LOG=debug,gpu=trace` shows debug messages from everything and trace
messages from `gpu`. `umbra -h` says so. A trace line starts with the local
time, to the microsecond, and the source line it comes from, relative to the
repository, with its message from column 40.

An entry of `UMBRA_LOG` can end with a destination for the debug and trace
messages it enables: a file descriptor from 2 to 9 (`UMBRA_LOG=debug:3`), or
the absolute path of a file they are appended to
(`UMBRA_LOG=gpu=trace:/tmp/gpu.log`). A destination that cannot be opened
stops umbra before any work starts.

The logger is umbra's own, in `core`, on `std::format`: the format above
needs none of what logging libraries add (sinks, rotation, asynchronous
queues), and their default formats are those of servers, with timestamps and
thread ids on every line. A message is formatted in full and written with a
single `write`, so that lines from parallel tasks do not interleave. A disabled level
costs one comparison, and its arguments are not formatted.

Debug is compiled into release builds, so that a user can be asked to run
with `UMBRA_LOG=debug` and send the output. Trace, meant for hot loops
(per frame, per kernel), exists only in debug builds; asking for it on a
release build says so.

Errors stay values (`std::expected<T, Error>`): an operation that fails
returns its `Error` and does not log it, and the caller that gives up reports
it once, as `fatal:`. umbra exits with 0 on success, 1 on failure and 2
on a usage error.

When a render starts, umbra shows on stderr the devices it renders on
(docs/adr/0004-render-devices.md), one line each, in a block of its own
rather than as log lines, as `ffmpeg` shows its banner:

```
umbra version 0.1.0 (Release, x86-64-v3)
  cpu0  AMD Ryzen Threadripper PRO 3975WX, 64 threads
  gpu0  NVIDIA GeForce RTX 5070 Ti, 15.5 GiB
```

It is shown by default, since what a render runs on explains its speed and
its output, and `-q` hides it. It is not a diagnostic: it has no level, and
`UMBRA_LOG` does not filter it. It goes to stderr, so
stdout keeps only results. The details of the hardware are the output of
`umbra devices`.

After that block, umbra shows the progress of its work on stderr, one line
per phase:

```
Loading stage: 100% (3/3 layers), 0.4 s, done.
Syncing scene: 100% (6/6 prims), 0.1 s, done.
Compiling shaders:  75% (9/12 shaders) | 6.1 shaders/s, ETA 0.5 s
```

A line is `<Phase>: <percent>% (<done>/<total> <unit>)`, followed while the
phase runs by its throughput and the time left, after ` | `, when the phase
measures a rate. A phase that does not know its total yet shows
`<Phase>: <done> <unit>` until it does. When it ends, its line becomes
`<Phase>: 100% (<total>/<total> <unit>), <elapsed>, done.`, and the next
phase starts on a new line; a phase that fails ends its line with
`, failed.`, before the `fatal:` line that says why.

On a terminal, the line of the current phase is redrawn in place with a
carriage return, at most every 100 ms, cut to the terminal's width. A log
line printed during a phase clears the progress line, and the progress line
is drawn again below it, in the same `write`. When stderr is not a terminal
(CI, `2>file`), a phase writes its line each time its work reaches a further
10%, then its `done.` line, without carriage returns, throughput or time
left. The work is checked every 100 ms, so a phase that passes several
tenths in that time writes one line for them, and a phase shorter than that
writes only its `done.` line. A phase without a total writes nothing there
until it has one. `-q` hides the progress, as it hides the start block. Like the start
block, progress has no level, and `UMBRA_LOG` does not filter it.

The numbers on stderr follow one set of rules: durations in seconds with
one decimal under a minute (`38.2 s`), then `m:ss`, then `h:mm:ss`; rates
with an SI prefix and one decimal (`3.1 Gsamples/s`); sizes with a binary
prefix and one decimal (`6.2 GiB`); counts in full up to 10 000, then with
three significant digits and k, M or G (`1.24M prims`). They always use a
decimal point, whatever the locale.

## Consequences

- The libraries umbra uses report through their own channels. umbra
  initializes each one explicitly at startup, in its bridge in `src/ext/`,
  right after the logger, and routes its diagnostics into the logger under
  its component. OpenUSD goes through a `TfDiagnosticMgr::Delegate` (`usd`),
  OSL through an `OIIO::ErrorHandler` (`osl`), OpenColorIO through
  `OCIO::SetLoggingFunction` (`ocio`), OpenSubdiv through its error and
  warning callbacks (`osd`), Embree through its device error function
  (`embree`), and OptiX through the log callback of its device context
  (`gpu`). Otherwise they print to the terminal in their own formats, and
  `-q` cannot silence them. A library that fails to initialize stops umbra
  with an error before any work starts, and `UMBRA_LOG=debug` lists the
  version of each.
- The GPU module, loaded with `dlopen`, keeps no logging state of its own: the
  loader passes it a log callback and the current level through `ModuleApi`.
- Progress is not a log message, but it shares the logger's writer on
  stderr, so that log lines and the progress line never interleave.
- A phase's work is counted with an atomic counter, which the render threads
  update on the hot path without allocating or locking
  (docs/adr/0003-no-allocations-on-the-hot-path.md); the line is formatted
  and written by a timer thread of the phase. That thread sleeps between
  updates and does no parallel work, so it is not a task for the oneTBB
  pool (docs/adr/0001-onetbb-thread-pool.md), whose workers it would hold.
