# 7. Deployment View

> Only the infrastructure (section 7.1) is settled. How Umbra is built and deployed per machine, and which machine runs the Coordinator, are still open on the implementation-plan map.

## 7.1 Infrastructure

Three machines on one LAN, inventoried on 2026-11-06 ([Hardware inventory of target machines](https://github.com/sergioffpc/umbra/issues/19)).

| | MacBook Pro M2 | Lenovo P620 | Dell PowerEdge R630 |
| --- | --- | --- | --- |
| CPU | Apple M2 Pro (`Mac14,10`) | AMD Threadripper PRO 3975WX (Zen 2) | 2 × Intel Xeon E5-2690 v4 (Broadwell-EP), 2.6 GHz base / 3.5 GHz turbo |
| Architecture | `aarch64-apple-darwin` | `x86_64-unknown-linux-gnu` | `x86_64-unknown-linux-gnu` |
| Cores / threads | 12 / 12 (8 P + 4 E, no SMT) | 32 / 64 (SMT on) | 28 / 56 (14 / 28 per socket, SMT on) |
| SIMD | NEON (AdvSIMD), FP16, DotProd, I8MM, BF16; no SVE | AVX2, FMA, F16C, BMI1/2; **no AVX-512** | AVX2, FMA, F16C, BMI1/2; **no AVX-512** |
| L1d / L2 | P: 128 KiB / 16 MiB shared by 4 cores; E: 64 KiB / 4 MiB shared by 4 | 32 KiB / 512 KiB per core | 32 KiB / 256 KiB per core |
| L3 / LLC | System Level Cache (not reported) | 128 MiB in 8 × 16 MiB slices (one per CCX of 4 cores) | 2 × 35 MiB (one per socket) |
| Cache line | **128 B** | 64 B | 64 B |
| NUMA | 1 node | 1 node (NPS1), but L3 split over 8 CCXs | **2 nodes** (one per socket, 128 GiB each) |
| RAM | 32 GB LPDDR5 | 128 GiB DDR4-3200, 8 channels | 256 GiB DDR4-2133, 8 × 32 GB 4R (4 channels per socket) |
| OS | macOS 26.6 | Ubuntu 26.10, kernel 7.3 | Debian 13, kernel 6.12 |
| Network | Wi-Fi | 1 GbE (AQC107 10G NIC, link at 1G) | 1 GbE (82599ES 10G SFP+ NIC, link at 1G) |
| Notes | 16 KiB pages | GPU (RTX 5070 Ti) unused: GPU is out of scope | Host firewall (nftables) admits only SSH from the LAN |

**Network target: 1 GbE.** Measured P620 ↔ R630 with `iperf3`: ~941 Mb/s in each direction (one stream or four), RTT ~0.25 ms. Both NICs can do 10 GbE, but Umbra is designed for 1 GbE. The MacBook joins over Wi-Fi.

Consequences for later decisions:

- **SIMD baseline**: no target machine has AVX-512, so x86 kernels target `x86-64-v3` (AVX2 + FMA) and arm64 targets NEON; an AVX-512 path has no machine to run or benchmark on.
- **NUMA**: the R630 is the only multi-node machine; the P620's L3 is split over 8 CCXs, so cache locality matters there too.
- **Cache-line padding**: per-thread data must be padded to 128 B to avoid false sharing on Apple silicon (64 B suffices on x86).
- **Heterogeneous threads**: the M2's P and E cores differ in speed; x86 machines use SMT.
- **Bandwidth**: ~117 MB/s per link to the Coordinator bounds Scene replication and Work Result traffic.
- **Firewall**: Worker and Coordinator ports must be opened explicitly on the R630.
