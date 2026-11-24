# Diagnostics go to stderr, one line each, prefixed with the level

umbra is a command-line program. Results go to stdout, and only results, so
that they can be piped. Diagnostics go to stderr, one line each, as plain
text prefixed with the level:

```
fatal: cannot open 'scene.usda': No such file or directory
error: unknown device 'gpu3'; run umbra devices to list them
warning: /World/Lamp: unsupported light type 'PortalLight', ignored
info: gpu: OptiX 9.1, device 0: RTX 5090 (sm_120)
debug: usd: stage opened in 120 ms (3 layers)
12:34:56.789012 src/gpu/module.cc:161   trace: gpu: frame 12: 4096 spp, 38.2 ms
```

The levels are fatal, error, warning, info, debug and trace, and by default
umbra shows fatal errors, errors and warnings. Fatal is the error umbra gives
up on, after which it exits. The flags `-q` (errors only), `-v` (info), `-vv`
(debug) and `-vvv` (trace) set the level. `UMBRA_LOG` sets it too, per
component: `UMBRA_LOG=warn,gpu=trace` shows warnings and everything from
`gpu`. The flags take precedence over the levels of the variable. The
component is shown from info down, where it helps to tell sources apart;
errors and warnings are for the user and stay plain. A trace line starts with
the local time, to the microsecond, and the source line it comes from,
relative to the repository, with its message from column 40.

An entry of `UMBRA_LOG` can end with a destination for the messages it
enables: a file descriptor from 2 to 9 (`UMBRA_LOG=debug:3`), or the absolute
path of a file they are appended to (`UMBRA_LOG=warn,gpu=trace:/tmp/gpu.log`
keeps warnings on stderr and sends `gpu` to the file). A destination that
cannot be opened stops umbra before any work starts.

The logger is umbra's own, in `core`, on `std::format`: the format above
needs none of what logging libraries add (sinks, rotation, asynchronous
queues), and their default formats are those of servers, with timestamps and
thread ids on every line. A message is formatted in full and written with a single
`write`, so that lines from parallel tasks do not interleave. A disabled level
costs one comparison, and its arguments are not formatted.

Info and debug are compiled into release builds, so that a user can be asked
to run with `-vv` and send the output, as with `ssh -vvv`. Trace, meant for
hot loops (per frame, per kernel), exists only in debug builds; `-vvv` on a
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
  gpu0  NVIDIA GeForce RTX 5070 Ti, 16 GiB
```

It is shown by default, since what a render runs on explains its speed and
its output, and `-q` hides it. It is not a diagnostic: it has no level, and
`UMBRA_LOG` does not filter it. It goes to stderr, so
stdout keeps only results. The details of the hardware are the output of
`umbra devices`.

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
  with an error before any work starts, and `-v` lists the version of each.
- The GPU module, loaded with `dlopen`, keeps no logging state of its own: the
  loader passes it a log callback and the current level through `ModuleApi`.
- Progress (a render's progress bar) is not a log message. It goes to stderr
  too, and only when stderr is a terminal.
