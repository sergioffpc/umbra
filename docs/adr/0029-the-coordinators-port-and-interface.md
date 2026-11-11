# The Coordinator's port and interface

The Coordinator listens on a UDP port the OS picks, unless the operator sets one with `--port <n>` or `UMBRA_PORT`. It listens and announces on every network interface, unless the operator names one with `--interface <name>` or `UMBRA_INTERFACE`. Decided with the owner on 2026-11-11. This amends [ADR 0016](0016-one-render-job-per-quic-connection.md), whose Coordinator listened on 7470 by default, and [ADR 0021](0021-plug-and-play-workers-in-a-named-cluster.md), whose consequences opened 7470 on every machine that runs a Coordinator.

A fixed default port made one Coordinator per machine the rule without saying so: a second `umbra render` on the same machine failed to bind. Since ADR 0021, Workers learn the port from the SRV record of the announcement, and no Worker is ever given a port, so the fixed number served only the firewall. On a machine with several interfaces (wired, Wi-Fi, a Docker bridge), listening on all of them also exposes the port on networks that hold no Worker.

- **Ephemeral port by default.** With no `--port` and no `UMBRA_PORT`, the Coordinator binds port 0 and announces the port the OS assigned. Two Coordinators on one machine then run side by side, each with its own announcement.
- **A fixed port on request.** `--port <n>` binds exactly that port; `--port 0` asks for an ephemeral one explicitly.
- **Every interface by default.** With no `--interface` and no `UMBRA_INTERFACE`, the Coordinator binds the unspecified address and announces on every interface, as before. A machine with one network interface needs nothing more.
- **One interface on request.** `--interface <name>` takes the OS name of an interface, such as `enp5s0`. The Coordinator binds that interface's IPv4 address only, and announces the Render Job only on that interface, with only that address. Discovery is IPv4 already: the mDNS group of ADR 0021 is 224.0.0.251.
- **Errors before the announcement.** A port that is taken, an interface that does not exist, or an interface with no IPv4 address makes the Coordinator exit with an error before announcing anything, like an invalid Scene File: no Worker ever joins. For an unknown interface, the error lists the interfaces that exist.
- **Same precedence as the Cluster Name.** The flag wins over the environment variable, which wins over the default, as for `--cluster` and `UMBRA_CLUSTER`.
- **GPU Local Workers follow.** A GPU Local Worker is a child process that connects back over QUIC ([ADR 0026](0026-gpu-workers-from-the-first-milestone.md)). The Coordinator passes it the address and port it bound: loopback when it listens on every interface, the interface's address otherwise, since loopback is then not bound.
- **Workers are unchanged.** A Worker only opens outgoing connections, so it has no port to restrict. It browses on every interface and connects to the address the announcement gives.

## Considered options

- **Keep 7470 by default, with `--port` to override**: rejected by the owner. A firewalled machine works without a flag, but a second Coordinator on the same machine fails unless the operator remembers to pick another port.
- **Try 7470, fall back to an ephemeral port when it is taken**: rejected. The port would depend on what else runs on the machine, so a firewall rule for 7470 would work for the first Coordinator and silently not for the second.
- **The interface of the default route by default**: rejected by the owner. It needs no flag, but when the default route is Wi-Fi and the Cluster is wired, the Workers never see the Render Job.
- **An interface required whenever a machine has several**: rejected by the owner. It never guesses, but every Coordinator on the P620 or the R630 would need a flag.
- **An IP address instead of an interface name**: rejected. Addresses from DHCP change; interface names do not.

## Consequences

- A machine whose firewall drops inbound UDP must run its Coordinator with a fixed port, and that port must be open, for example `--port 7470` with a matching rule. Otherwise the Render Job is announced but no remote Worker can connect: the Coordinator renders with its Local Workers only, and each Worker that tries fails to connect and rejoins on the backoff of ADR 0021. The README's firewall section says so.
- The R630's nftables, which admits only SSH and mDNS, needs both a rule and a `--port` for `umbra render` to run there with remote Workers.
- Restricting the port to one interface is opt-in. Without `--interface`, a machine with several interfaces exposes the port, and announces the Render Job, on all of them.
