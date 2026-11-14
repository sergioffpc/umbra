# Every Local Worker in a child process

`umbra render` starts every Local Worker in a child process of its own, the CPU's as well as each GPU's, and each connects back to the Coordinator over QUIC on loopback, as a remote Worker does. The in-memory transport is removed. A Local Worker writes nothing to the terminal: it logs, and the Coordinator logs what it logs. Decided with the owner on 2026-11-14, in [#128](https://github.com/sergioffpc/umbra/issues/128). This amends [ADR 0009](0009-pull-based-pass-major-scheduling.md), [ADR 0010](0010-quic-transport-between-coordinator-and-workers.md) and [ADR 0026](0026-gpu-workers-from-the-first-milestone.md), which kept the CPU Local Worker in the Coordinator's process behind an in-memory transport.

With the GPU's Local Worker in a child process over QUIC, the in-memory transport was the second path of every message: two transports to keep in step, and a CPU Local Worker that borrowed the Coordinator's Scene and threads instead of building its own replica, as every other Worker does. One path makes the machine a Coordinator renders on a Cluster of Workers like any other.

- **One Local Worker per Render Device, each a process.** `umbra render` runs its own executable as `umbra worker --render-device cpu` and `umbra worker --render-device gpu:N`, each with `--coordinator` and the Coordinator's loopback address, and `--nthreads` for the CPU's.
- **One transport.** Every Worker, local or remote, speaks the wire protocol of arc42 §8.13 over QUIC. Inside a Worker's process, its connection and its render loop still pass the typed messages over channels.
- **A child is known by its process.** `Hello` carries the Worker's process identifier, so the Coordinator tells which Worker a child that exits was, and takes back its Work Units at once instead of after the idle timeout.
- **Workers log, they do not print.** `umbra worker` logs its version line and its errors as events. A Local Worker logs JSON lines to a pipe; the Coordinator logs each event at its level, prefixed with the Worker's Render Device, so they appear above the status line, through the same `--log` filter.

## Considered options

- **Keep the CPU Local Worker in the Coordinator's process**, as ADR 0026 had it: rejected by the owner. It saves a process start and a Scene transfer on loopback, but keeps two transports, and a CPU failure would take the Film down with it.
- **The Local Workers' stderr straight to the terminal**: rejected. They would write over the status line, which the Coordinator draws at the bottom of the terminal.
- **The Local Workers' logs discarded, or in a file per Worker**: rejected by the owner. A panic of a Local Worker would go unseen, or be found only by looking for the file.

## Consequences

- A Render Job starts a little later: each Local Worker starts its process, receives the Scene and builds its replica before its first `RequestWork`.
- A CPU Worker failure loses that Worker's Work Units, not the Film, as a GPU's already did.
- Tests check the transport and the reassignment of Work Units with CPU Local Workers, so they run without a GPU.
- The status line and the Render Job Report sum every CPU Worker's threads into the Local Worker's figures until each Worker gets a line of its own.
