# 6. Runtime View

The scenarios a Session runs through, from the decisions in [ADR 0004](../adr/0004-deterministic-film-for-a-fixed-render-job.md) (deterministic merge), [ADR 0009](../adr/0009-pull-based-pass-major-scheduling.md) (scheduling), [ADR 0012](../adr/0012-per-cpu-binaries-locked-to-one-commit.md) (Build Identity), [ADR 0015](../adr/0015-one-render-job-per-quic-connection.md) and [§8.13](08-crosscutting-concepts.md#813-wire-protocol) (wire protocol). The messages are those of §8.13; the rules behind each step are in §8.8 and §8.13 and are not repeated here.

Two parts of these scenarios are still open and are marked where they appear: how a Session starts and who may join it ([Session start-up and Worker joining](https://github.com/sergioffpc/umbra/issues/59)), and how connections close when a Worker is declared dead or a Render Job is cancelled ([Closing connections: dead Workers and cancellation](https://github.com/sergioffpc/umbra/issues/60)).

## 6.1 Worker join and Scene transfer

A Worker started by `scripts/workers up` connects to the Coordinator, is admitted, receives the whole Scene and announces it is ready by asking for work.

```mermaid
sequenceDiagram
    participant W as Worker
    participant C as Coordinator
    W->>C: QUIC connect (UDP 7470), accept any certificate
    W->>C: control: Hello (session_id, Build Identity, host, threads)
    Note over C: Session identifier and commit match,<br/>tree clean or --allow-dirty
    C->>W: control: Welcome (worker_id)
    Note over C: status line: Worker loading
    C-->>W: Scene stream: Render Job options
    C-->>W: Scene stream: Scene structure (arenas, no BVH)
    loop every asset
        C-->>W: asset bytes + content hash
        Note over W: check the hash
    end
    Note over W: end of Scene stream
    Note over W: build the Scene and its BVH<br/>(deterministic, ADR 0004)
    W->>C: control: RequestWork (count)
    Note over C: status line: Worker alive
    W-->>C: heartbeat stream, one heartbeat per second
```

1. The Worker is the QUIC client. It accepts the Coordinator's self-signed certificate without checking it; admission rests on the handshake instead.
2. `Hello` carries the Session identifier from `scripts/workers up` and the Build Identity. A mismatch ends here (§6.5).
3. The Scene stream carries only what a Worker needs: the Render Job options (seed, Tile size, Pixel Samples per Pass, total Pixel Samples per pixel), the Scene structure, and every asset. There is no asset cache, so every connection receives everything. An asset whose hash does not match ends the Worker with `Fatal` (§6.3).
4. Each Worker builds its own BVH from the Scene. Its first `RequestWork` is the readiness signal, and the Coordinator assigns nothing before it. The time from `Welcome` to that request is the Worker's loading phase in the Render Job Report.

The in-process local Worker skips the network: it receives the same typed messages through the in-memory transport, and shares the Coordinator's Scene.

Open ([Session start-up and Worker joining](https://github.com/sergioffpc/umbra/issues/59)): whether the Coordinator parses the Scene before it listens, whether a Worker retries a connection made before the Coordinator is up, how a Worker joins a Session that is already rendering, and when the heartbeat stream starts. If it starts only with the first `RequestWork`, the 5 s death rule does not apply to a loading Worker, and a Worker that hangs while loading is never detected.

## 6.2 A Render Job from submission to image

The operator starts a Session from their own machine and gets the final image back.

```mermaid
sequenceDiagram
    actor O as Operator
    participant S as scripts/workers up
    participant C as Coordinator (umbra render)
    participant F as Film
    participant W as Workers
    O->>S: up --ref or --version, Scene File, options
    Note over S: generate the Session identifier
    S->>C: SSH: start Coordinator (--session)
    S->>W: SSH: start each Worker (--coordinator, --session)
    Note over C: parse the Scene File (umbra-parser),<br/>apply overrides, build the Reproduction Record
    alt Diagnostics
        C->>S: Diagnostics (file, line, column), exit, no Work Unit
    end
    W->>C: join and load (§6.1)
    loop until every Work Unit is merged
        W->>C: RequestWork (count)
        C->>W: Assign (Tile, Pass), Pass-major, raster order
        W-->>C: heartbeat: cumulative Pixel Samples per Work Unit
        W-->>C: Work Result stream: header (hash, Anomalous Pixel Samples) + f64 sums
        C->>F: accept: Merged, or Buffered until the previous Pass of that Tile
        W->>C: RequestWork (1)
    end
    C->>W: JobFinished
    W->>C: close (FINISHED), exit 0
    Note over C: write the image (with Reproduction Record)<br/>and the Render Job Report,<br/>log Anomalous Pixel Samples as umbra trace commands
    Note over C: exit once Workers have closed, or after 2 s
    S->>O: copy the final image back
```

1. The binary parses the Scene File once ([ADR 0003](../adr/0003-pbrt-v4-scene-files-parsed-on-the-coordinator.md)). A file with any error produces no Work Unit.
2. `umbra-cluster`'s `coordinate` starts the optional local Worker and accepts remote Workers (§6.1).
3. Scheduling follows §8.8. Work Units go out Pass by Pass in raster Tile order, and Pass p of a Tile waits until Pass p − 2 of that Tile is merged. A Worker keeps about two Work Units in flight per eight threads, and returns one credit per Work Result.
4. The Coordinator checks each Work Result's hash on receipt. The Film merges each Tile's Work Results in Pass order and holds the one early arrival per Tile that scheduling allows (ADR 0004).
5. The status line on the Coordinator's stderr (§8.10) shows the current Pass, the percentage of Pixel Samples merged or in flight, each Worker's state and the ETA throughout.
6. After the last merge, `JobFinished` releases every Worker. The image and its Render Job Report are written next to each other, and the image only exists once every Pass has been rendered.

Open ([Session start-up and Worker joining](https://github.com/sergioffpc/umbra/issues/59)): how the Scene File and its assets reach the Coordinator's host when it is not the operator's machine, and what connected Workers see when parsing fails.

## 6.3 Worker failure, reassignment and a duplicate Work Result

Worker B stops sending heartbeats while two Work Units are in flight. One of them was already being sent and arrives after its reassigned copy.

```mermaid
sequenceDiagram
    participant A as Worker A
    participant C as Coordinator
    participant F as Film
    participant B as Worker B
    C->>B: Assign (Tile 4, Pass 1), (Tile 5, Pass 1)
    B-->>C: heartbeat
    Note over B: stops responding
    Note over C: 5 s without a heartbeat:<br/>Worker B dead
    Note over C: (Tile 4, Pass 1), (Tile 5, Pass 1)<br/>go to the front of the queue
    A->>C: RequestWork (2)
    C->>A: Assign (Tile 4, Pass 1), (Tile 5, Pass 1)
    A-->>C: Work Result (Tile 4, Pass 1), hash h
    C->>F: accept: Merged
    B-->>C: late Work Result (Tile 4, Pass 1), hash h'
    C->>F: accept: Duplicate if h' = h, Conflict otherwise
    Note over C: Conflict: error log naming A and B,<br/>counted in the Render Job Report
    A-->>C: Work Result (Tile 5, Pass 1)
    C->>F: accept: Merged
```

1. A Worker is dead when its connection closes, when it sends `Fatal`, or after 5 s without a heartbeat. There is no per-Work-Unit timeout.
2. Its Work Units in flight go to the front of the queue, because they are the ones holding back ordered merges.
3. The first Work Result for a Work Unit is merged, even from a Worker already declared dead. Any later one is compared by hash and discarded. Equal hashes are the expected outcome (`Duplicate`). Different hashes (`Conflict`) are a bug in determinism and are logged as an error naming both Workers. The Film keeps the first.
4. With no Workers left, the Render Job waits. A Worker that comes back joins as a new Worker (§6.1).

A Worker whose connection merely went quiet but whose render pool is still progressing is not dead: it keeps sending heartbeats. If its Pixel Sample count stops growing for 30 s, the status line shows it as `stalled` and nothing is reassigned. The speculative tail (§6.4) covers it at the end.

Open ([Closing connections: dead Workers and cancellation](https://github.com/sergioffpc/umbra/issues/60)): whether the Coordinator closes the connection of a Worker it declared dead by heartbeat, and with which close code, given that closing it would cut off the late Work Result shown above.

## 6.4 Speculative tail

At the end of the last Pass, the queue is empty while Worker B, the slowest, still renders a Work Unit.

```mermaid
sequenceDiagram
    participant A as Worker A
    participant C as Coordinator
    participant F as Film
    participant B as Worker B
    Note over C: queue empty, (Tile 3, Pass 7) in flight on B
    A->>C: RequestWork (1)
    C->>A: Assign copy of (Tile 3, Pass 7)
    A-->>C: Work Result (Tile 3, Pass 7)
    C->>F: accept: Merged
    C->>B: Cancel (Tile 3, Pass 7)
    alt B still rendering
        Note over B: drop it at the next Tile row
        B->>C: RequestWork (1), the cancelled credit
    else B already sending its Work Result
        C->>B: stop_sending on that stream
        B->>C: RequestWork (1), the cancelled credit
    else B's Work Result arrived first
        B-->>C: Work Result (Tile 3, Pass 7)
        C->>F: accept: Duplicate, hash compared
    end
```

1. When the queue is empty and a Worker has pending demand, the Coordinator assigns it a copy of the oldest Work Unit still in flight. Each Work Unit gets at most one copy.
2. The first Work Result wins. The other copy is cancelled, or, if it arrives anyway, compared by hash and discarded as in §6.3.
3. A cancelled Work Unit gives its credit back, so the Worker can take another copy while any remain.

Copies happen in every Render Job, so the hash comparison of ADR 0004 runs routinely, not only after failures.

## 6.5 A rejected Worker

A Worker from another Session, or built from another commit, reaches the Coordinator.

```mermaid
sequenceDiagram
    participant W as Worker
    participant C as Coordinator
    W->>C: QUIC connect
    W->>C: Hello (session_id, Build Identity)
    alt Session identifier differs
        C->>W: Reject (SessionMismatch)
    else commit differs
        C->>W: Reject (CommitMismatch, coordinator_commit)
    else dirty tree without --allow-dirty
        C->>W: Reject (DirtyBuild)
    end
    C->>W: close (REJECTED)
    Note over W: log the reason, exit non-zero
    Note over C: log the rejection, no Scene sent,<br/>the Render Job continues with the other Workers
```

1. The Coordinator checks the Session identifier, the commit of the Build Identity and the dirty flag ([ADR 0012](../adr/0012-per-cpu-binaries-locked-to-one-commit.md)). The target CPU is informational and never rejects.
2. Nothing of the Scene is sent before `Welcome`, so a rejected Worker costs the Render Job nothing.
3. `--allow-dirty` admits dirty builds and is recorded in the Render Job Report.

The certificate is never checked, so these rejections catch mistakes of operation, such as a Worker left over from another Session, and not hostile nodes, which are out of scope (ADR 0015).

## 6.6 Ctrl-C cancellation

The operator cancels a running Render Job on the Coordinator.

```mermaid
sequenceDiagram
    actor O as Operator
    participant C as Coordinator
    participant W as Workers
    O->>C: Ctrl-C
    C->>W: close (JOB_CANCELLED)
    Note over W: drop every Work Unit in flight, exit
    Note over C: no image is written (ADR 0004),<br/>exit
```

1. A Render Job either renders every Pass or produces no image. There is no partial image and no time budget (ADR 0004).
2. Closing a connection with `JOB_CANCELLED` ends every stream on it, so Workers need no separate `Cancel` per Work Unit.

Open ([Closing connections: dead Workers and cancellation](https://github.com/sergioffpc/umbra/issues/60)): how Ctrl-C in `scripts/workers up`, which runs everything over SSH, reaches the Coordinator, what Ctrl-C on a Worker means, the exit codes on `JOB_CANCELLED`, and whether a cancelled Render Job still writes its Render Job Report.
