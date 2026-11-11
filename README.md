# Umbra

[![CI](https://github.com/sergioffpc/umbra/actions/workflows/ci.yml/badge.svg?branch=develop)](https://github.com/sergioffpc/umbra/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

Umbra is a physically based ray tracer written in Rust, inspired by
[pbrt-v4](https://github.com/mmp/pbrt-v4) and the book
[_Physically Based Rendering: From Theory to Implementation_](https://pbr-book.org/).

It is designed to render on x86-64 CPUs, with a GPU backend planned after the
CPU milestones, use every core of a machine, and spread the work of one image
over the machines of a LAN.

> **Status:** early development. Nothing renders yet.

## Building

Requires Rust 1.99.1, pinned in `rust-toolchain.toml`; rustup installs it automatically.

```sh
git clone https://github.com/sergioffpc/umbra.git
cd umbra
cargo build --release
```

## Usage

```sh
cargo run --release
```

## Rendering on several machines

> **Status:** remote Workers and GPU Workers arrive in v0.1.0. Until then,
> nothing renders.

Umbra spreads one image over every machine on the LAN that runs a Worker. You
start a Worker on each machine once and leave it running. A Worker sits idle
until some machine runs `umbra render`. It then joins that Render Job, renders
its share, and goes back to waiting. You never write a host list or an address:
the Coordinator (the `umbra render` process) announces each Render Job with
multicast DNS (DNS-SD, service `_umbra._udp`), and idle Workers find it.

### Clusters

Every Worker and every `umbra render` belongs to a Cluster. A Coordinator only
uses Workers of its own Cluster. Name the Cluster with `--cluster <name>` or the
`UMBRA_CLUSTER` environment variable; without either, it is `default`. Give two
groups of machines different names to keep them apart on the same LAN, for
example to benchmark one machine on its own. The name is a label, not a
password.

### 1. Start a Worker on each machine

From a release image, on Linux:

```sh
docker run -d --name umbra-worker --restart unless-stopped --network host \
  ghcr.io/sergioffpc/umbra:X.Y.Z worker
```

`--network host` lets the Worker see multicast DNS. `--restart unless-stopped`
brings the Worker back after a crash or a reboot.

From source:

```sh
git checkout <ref>
cargo build --release
target/release/umbra worker
```

Run one Worker per NUMA node. On a machine with two nodes, such as a
two-socket server, bind one Worker to each node:

```sh
numactl --cpunodebind=0 --membind=0 target/release/umbra worker &
numactl --cpunodebind=1 --membind=1 target/release/umbra worker &
```

With containers, use `--cpuset-cpus "$(cat /sys/devices/system/node/node0/cpulist)"`
and `--cpuset-mems 0`, then the same with node 1.

A Worker logs when it joins a Render Job, loads the Scene and goes back to idle.
Stop it with Ctrl-C, `SIGTERM` or `docker stop`.

### 2. Render

On the machine that holds the Scene File:

```sh
umbra render scene.pbrt
```

`umbra render` parses the Scene File first. If the file has errors, it reports
them and exits before any Worker joins. Otherwise it announces the Render Job,
and every idle Worker of its Cluster joins, receives the Scene and starts
rendering. The Coordinator also renders on its own machine, so a Render Job
finishes even if no Worker joins. A Worker started while a Render Job runs joins
it too. The image and its Render Job Report are written on this machine, and the
status line shows each Worker as it works.

Ctrl-C cancels the Render Job: there is no image, the Render Job Report is still
written, and the Workers go back to waiting.

### Keep every machine on the same commit

A Coordinator only admits Workers built from its own commit, with a clean tree
unless it runs with `--allow-dirty`. It refuses a Worker on any other commit and
logs that Worker's host and commit. After you change commit, rebuild and restart
the Worker on every machine, or pull and restart the release image.

### Firewall, once per machine

- Every machine: allow mDNS, UDP port 5353, from the LAN.
- Every machine you run `umbra render` on: also allow UDP port 7470, the
  Coordinator's port.

## Container image

Every release publishes a minimal [chiseled Ubuntu](https://ubuntu.com/containers/chiselled)
image to the GitHub Container Registry: no shell, no package manager, running
as the non-root user `umbra` (uid 10001) in `/work`. The image is
`linux/amd64`, tagged `X.Y.Z`, and its binary is built for `x86-64-v3` (AVX2
and FMA), so it runs on any x86-64 CPU from Intel Haswell or AMD Zen onwards.

Pull the release you want:

```sh
docker pull ghcr.io/sergioffpc/umbra:X.Y.Z
docker run --rm --user "$(id -u):$(id -g)" -v "$PWD:/work" ghcr.io/sergioffpc/umbra:X.Y.Z
```

`--user` lets `umbra` write its output into the mounted directory with your
own ownership.

There is no `latest` or `X.Y` tag: every machine in a Render Job runs the same
release (see [Keep every machine on the same commit](#keep-every-machine-on-the-same-commit)). Verify an image's build provenance with:

```sh
gh attestation verify oci://ghcr.io/sergioffpc/umbra:X.Y.Z -R sergioffpc/umbra
```

## Documentation

The architecture (arc42), the architecture decision records and the glossary
are indexed in [`docs/`](docs/README.md).

## Contributing

Contributions are welcome. Read [CONTRIBUTING.md](CONTRIBUTING.md) for the
branching model, commit conventions and checks to run before opening a pull
request. This project follows a [Code of Conduct](CODE_OF_CONDUCT.md).

To report a security issue, see [SECURITY.md](SECURITY.md).

## License

Umbra is licensed under the [MIT License](LICENSE). It includes code and data
ported from pbrt-v4, under the Apache License 2.0: see [NOTICE](NOTICE).
