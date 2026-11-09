# umbra-cluster

The Coordinator and the Worker: `coordinate` runs a Render Job and `work`
runs a Worker, both synchronous, each owning its `tokio` runtime. Behind them
are the QUIC wire protocol (ADR 0010), discovery by Cluster Name, heartbeats,
pull-based Pass-major scheduling (ADR 0009), reassignment and the speculative
tail, the Local Worker, Build Identity admission, the status line and the
Render Job Report.

It is a `std` crate, outside the core. It depends on `umbra-render`, and the
`umbra` binary depends on it; it receives a parsed Scene and does not depend
on `umbra-parser` (arc42 §5.1). Its contract is sketched in arc42 §5.3.

> **Status:** not implemented yet. Its first code arrives with
> [#78 umbra render end to end on a camera-only Scene File](https://github.com/sergioffpc/umbra/issues/78).
