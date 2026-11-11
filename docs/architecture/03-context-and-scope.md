# 3. Context and Scope

Umbra is a command-line program. The operator gives it a Scene File and options; it renders on the machines of the LAN and writes an image with a Render Job Report next to it. Around that core sit the tools that check Umbra against pbrt-v4 and the tools that show how a pixel was computed.

## 3.1 Business context

```mermaid
flowchart LR
    op(["Operator"])
    umbra["Umbra"]
    scenes[("mmp/pbrt-v4-scenes<br/>pinned commit")]
    ref[("pbrt-v4 reference<br/>release assets")]
    pbrt["pbrt-v4<br/>pinned commit"]
    blender["Blender"]
    op -- "Scene File, options" --> umbra
    umbra -- "image, Render Job Report,<br/>status line" --> op
    op -- "umbra trace: pixel" --> umbra
    umbra -- "Pixel Trace (text, glTF)" --> blender
    scenes -- "Reference Scenes" --> umbra
    scenes -- "Reference Scenes" --> pbrt
    pbrt -- "16-seed renders,<br/>Reference Image" --> ref
    ref -- "umbra validate" --> umbra
```

| Partner                     | Input to Umbra                                                                                                                                                        | Output from Umbra                                                                                                                                   |
| --------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------- |
| Operator                    | A Scene File with its includes and assets; command-line options and overrides (`render`, `trace`, `bench`, `validate`); Ctrl-C                                        | The final image with its Reproduction Record; the Render Job Report; the status line on stderr; the reasons for an invalid Scene File (§8.2, §8.10) |
| `mmp/pbrt-v4-scenes`        | The Reference Scenes, downloaded at a pinned commit, never vendored (§8.4)                                                                                            | —                                                                                                                                                   |
| pbrt-v4                     | Through its published reference release: 16 seeds a side and a Reference Image per Reference Scene and variant (§8.11); its speed on the same machine for QS-5 (§8.4) | —                                                                                                                                                   |
| Blender, or any glTF viewer | —                                                                                                                                                                     | The resolved Scene and the paths of a Pixel Trace as glTF (§8.9)                                                                                    |
| Contributor                 | Code, through PRs                                                                                                                                                     | Benchmark and validation results committed to `docs/benchmarks/` and `docs/validation/`, or attached to PRs (§8.4, §8.11)                           |

## 3.2 Technical context

```mermaid
flowchart LR
    subgraph opm["Operator machine"]
        fs[("Scene File,<br/>image, report")]
        coord["umbra render:<br/>Coordinator (+ Local Workers:<br/>CPU in-process, GPU child)"]
        val["umbra validate"]
    end
    subgraph hosts["Other hosts on the LAN"]
        w["Workers"]
    end
    gh[("GitHub:<br/>ghcr.io images,<br/>release assets")]
    fs <--> coord
    coord -. "mDNS: Render Job announced" .-> w
    w -- "QUIC, UDP (ephemeral or --port)" --> coord
    w -. "docker pull (releases)" .-> gh
    val -. "download reference" .-> gh
```

| Channel            | Between                                         | Protocol and format                                                                                                                                                                    | Where it is decided                                                                                                                             |
| ------------------ | ----------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------- |
| Command line       | Operator and `umbra`                            | `clap` subcommands, exit codes `0`/`1`/`2`/`3`/`130`                                                                                                                                   | §5.3, §8.13                                                                                                                                     |
| Files              | Operator machine and Coordinator                | `.pbrt` Scene File and assets in; image and versioned-JSON Render Job Report out, next to the image                                                                                    | §8.2, §8.10, §8.15, [ADR 0020](../adr/0020-bit-identical-exr-final-image.md)                                                                    |
| Discovery          | Coordinator and the idle Workers of its Cluster | DNS-SD over multicast DNS (UDP 5353): the Coordinator announces its Render Job as `_umbra._udp` with `cluster=<name>`                                                                  | §7.3, §8.13, [ADR 0021](../adr/0021-plug-and-play-workers-in-a-named-cluster.md)                                                                |
| Coordinator–Worker | Coordinator and each remote Worker              | QUIC (`quinn`), an ephemeral UDP port unless `--port` sets one, on every interface unless `--interface` names one, unverified TLS, `postcard` messages and raw little-endian bulk data | §8.13, [ADR 0010](../adr/0010-quic-transport-between-coordinator-and-workers.md), [ADR 0016](../adr/0016-one-render-job-per-quic-connection.md) |
| Local Worker       | Coordinator and the Workers it starts           | In-memory transport for the CPU one, QUIC on loopback for each GPU one in a child process; same messages                                                                               | §8.8                                                                                                                                            |
| Releases           | CI and Linux hosts                              | One `x86-64-v3` container image on `ghcr.io`                                                                                                                                           | §7.2, [ADR 0025](../adr/0025-one-x86-64-v3-release-image.md)                                                                                    |
| Reference          | GitHub release assets and `umbra validate`      | `pbrt-v4-reference/<commit>` release, downloaded and hash-checked                                                                                                                      | §8.11                                                                                                                                           |
| Telemetry          | Coordinator and the operator                    | Status line on stderr; `tracing` logs; Tracy behind the `tracy` feature. No metrics endpoint                                                                                           | §8.4, §8.10                                                                                                                                     |

## 3.3 Out of scope

Ruled out of this architecture, so no section of this document plans for them:

- GPUs other than NVIDIA's, and GPU toolchains other than upstream `rustc` with CUDA and OptiX ([ADR 0028](../adr/0028-one-core-for-cpu-and-gpu.md)).
- The `bdpt`, `sppm` and `mlt` integrators on the GPU ([ADR 0026](../adr/0026-gpu-workers-from-the-first-milestone.md)).
- arm64 ([ADR 0023](../adr/0023-x86-64-only.md)).
- Cloud deployment, autoscaling, Workers outside the LAN.
- Out-of-core Scenes: every Worker holds the whole Scene.
- Hostile nodes: the TLS certificate is not verified, and the Cluster Name and Build Identity guard only against mistakes of operation (§6.5).
- A live view: the image is seen only when the Render Job finishes.
- Input formats other than pbrt-v4's `.pbrt`, and extensions to it.
