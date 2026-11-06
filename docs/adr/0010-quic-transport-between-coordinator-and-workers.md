# QUIC transport between Coordinator and Workers

The Coordinator and remote Workers communicate over QUIC with the `quinn` crate, not over TCP. The networking research ([Networking and serialisation options for coordinator/worker](https://github.com/sergioffpc/umbra/issues/17)) recommended tokio with length-delimited TCP framing and kept quinn as an upgrade path. The decision in [Work decomposition and scheduling](https://github.com/sergioffpc/umbra/issues/23) takes that path from the start: independent streams avoid head-of-line blocking between concurrent Work Results, heartbeats and Scene transfer, and switching transport later would touch every message.

- tokio, postcard messages and raw `bytemuck` tile pixels stay as the research recommended. Only the transport changes.
- QUIC's own keep-alive is not the liveness signal. The application heartbeat from [ADR 0009](0009-pull-based-pass-major-scheduling.md) is kept, because a stalled render pool would not stop quinn's keep-alive.
- The local Worker hosted by the Coordinator uses an in-memory transport behind the same seam, so QUIC is only on the network path.

## Considered options

- **TCP with length-delimited framing**: rejected. One byte stream per Worker serialises large Work Results, heartbeats and cancellations, so a heartbeat can wait behind a Work Result.
- **Several TCP connections per Worker**: rejected. It rebuilds stream multiplexing by hand.
- **gRPC**: rejected in the research, unchanged here.

## Consequences

- QUIC requires TLS 1.3 even on a trusted LAN. How the Coordinator gets a certificate and how Workers trust it (for example a self-signed certificate with a pinned fingerprint) is decided with the wire protocol. Hostile-node security stays out of scope.
- QUIC runs over UDP, so the LAN and host firewalls must allow the Coordinator's UDP port.
- The mapping of messages onto QUIC streams is part of the wire protocol design.
