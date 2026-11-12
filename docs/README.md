# Documentation

Umbra's design is written down in four places, each with one job:

- [`GLOSSARY.md`](../GLOSSARY.md): the domain vocabulary. Every other document uses its terms.
- [`architecture/`](architecture/): the architecture as it stands, in the twelve arc42 sections listed below. Section 5 describes how the crates fit together.
- [`adr/`](adr/): the hard-to-reverse decisions, one file each, in the order they were taken. Section 9 indexes them.
- `crates/<name>/README.md`: each crate from the inside, as it is built: its modules, its own design decisions, what to know when using it, its performance and its tests. It is also the crate's rustdoc documentation. See [Crate READMEs](../CONTRIBUTING.md#crate-readmes).

When an ADR and an arc42 section disagree, the later ADR is the decision and the section is the error to fix.

## Architecture (arc42)

1. [Introduction and goals](architecture/01-introduction-and-goals.md): requirements, quality goals, stakeholders
2. [Architecture constraints](architecture/02-architecture-constraints.md)
3. [Context and scope](architecture/03-context-and-scope.md): what Umbra talks to, and what is out of scope
4. [Solution strategy](architecture/04-solution-strategy.md): the milestone roadmap and the fundamental decisions
5. [Building block view](architecture/05-building-block-view.md): the crates and their interfaces
6. [Runtime view](architecture/06-runtime-view.md): a Render Job from start-up to image, Workers joining, failures and cancellation
7. [Deployment view](architecture/07-deployment-view.md): the target machines, release and development builds, running Workers and Render Jobs
8. [Crosscutting concepts](architecture/08-crosscutting-concepts.md): numerics, determinism, scheduling, wire protocol, tuning, image output
9. [Architecture decisions](architecture/09-architecture-decisions.md): the ADR index
10. [Quality requirements](architecture/10-quality-requirements.md): the measurable quality scenarios
11. [Risks and technical debt](architecture/11-risks-and-technical-debt.md)
12. [Glossary](architecture/12-glossary.md): points to `GLOSSARY.md`

## Background

Interactive explainers live next to the crate they explain, in its `docs/` folder. Open them in a browser.

- [`umbra-math/docs/field-guide.html`](../crates/umbra-math/docs/field-guide.html): `f32` rounding, error bounds, intervals, compensated arithmetic, normals, frames, octahedral vectors and the perspective projection.

## For contributors and agents

- [`CONTRIBUTING.md`](../CONTRIBUTING.md): branching, commits, checks and lint rules.
- [`agents/`](agents/): how coding agents use the issue tracker, triage labels and domain docs.
