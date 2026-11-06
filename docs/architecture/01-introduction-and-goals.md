# 1. Introduction and Goals

> Only the quality goals (section 1.2) are settled. The requirements overview and stakeholders are still open on the implementation-plan map.

## 1.2 Quality goals

Decided in [Quality goals and measurable targets](https://github.com/sergioffpc/umbra/issues/28). The order is the priority: when two goals conflict, the higher one wins. Each goal has measurable targets in section 10.

| Priority | Goal | Meaning |
| --- | --- | --- |
| 1 | Correctness | Umbra computes what pbrt-v4 computes: images that agree with pbrt-v4 within its own noise, and a Film that is bit-identical for a fixed Render Job (ADR 0004). |
| 2 | Comprehensibility | The code reads like the book: small documented functions, cohesive modules, simple contracts, and every algorithm traceable to its section of *Physically Based Rendering*, 4th edition, and to its pbrt-v4 counterpart. |
| 3 | Performance | Aggressive, per-architecture performance on the three target machines (section 7.1), reaching pbrt-v4's speed by the last milestone. An aggressive optimisation never weakens goals 1 and 2: it needs a benchmark, a simple contract and a readable reference implementation it is tested against. |
| 4 | Scalability | Rendering throughput grows with the cores of a Worker and with the Workers on the LAN. |
| 5 | Debuggability | Any Pixel Sample of a finished image can be computed again and inspected as a Pixel Trace (ADR 0011). |

Scalability and debuggability rank last because ADRs 0004 and 0009–0011 already secure most of them by design. Section 10 gives them targets, but they rarely need to win a conflict.
