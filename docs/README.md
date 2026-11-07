# Documentation

Umbra's design is written down in three places, each with one job:

- [`GLOSSARY.md`](../GLOSSARY.md): the domain vocabulary. Every other document uses its terms.
- [`architecture/`](architecture/): the architecture as it stands, in the twelve arc42 sections listed below.
- [`adr/`](adr/): the hard-to-reverse decisions, one file each, in the order they were taken. Section 9 indexes them.

When an ADR and an arc42 section disagree, the later ADR is the decision and the section is the error to fix.

## Architecture (arc42)

1. [Introduction and goals](architecture/01-introduction-and-goals.md): requirements, quality goals, stakeholders
2. [Architecture constraints](architecture/02-architecture-constraints.md)
3. [Context and scope](architecture/03-context-and-scope.md): what Umbra talks to, and what is out of scope
4. [Solution strategy](architecture/04-solution-strategy.md): the milestone roadmap and the fundamental decisions
5. [Building block view](architecture/05-building-block-view.md): the crates and their interfaces
6. [Runtime view](architecture/06-runtime-view.md): a Render Job from start-up to image, Workers joining, failures and cancellation
7. [Deployment view](architecture/07-deployment-view.md): the target machines, per-CPU builds, running Workers and Render Jobs
8. [Crosscutting concepts](architecture/08-crosscutting-concepts.md): numerics, determinism, scheduling, wire protocol, tuning, image output
9. [Architecture decisions](architecture/09-architecture-decisions.md): the ADR index
10. [Quality requirements](architecture/10-quality-requirements.md): the measurable quality scenarios
11. [Risks and technical debt](architecture/11-risks-and-technical-debt.md)
12. [Glossary](architecture/12-glossary.md): points to `GLOSSARY.md`

## Background

- [`math/field-guide.html`](math/field-guide.html): an interactive explainer of the mathematics Umbra relies on, from `f32` rounding and error bounds in `umbra-math` to the sampling, spectral and microfacet maths of the crates still to come. Open it in a browser.

## For contributors and agents

- [`CONTRIBUTING.md`](../CONTRIBUTING.md): branching, commits, checks and lint rules.
- [`agents/`](agents/): how coding agents use the issue tracker, triage labels and domain docs.
