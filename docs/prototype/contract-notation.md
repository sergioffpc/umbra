# PROTOTYPE: contract notation candidates

> Throwaway. Answers the question in "Contract notation for building-block interfaces"
> (sergioffpc/umbra#16). Lives only on `prototype/contract-notation`; never merged.

The same contract, **Scene Ingest**, written three ways. Scene Ingest observes Hydra's
terminal scene index, translates prim notices into edits on Umbra's render-optimised
internal scene, and tells the render loop when the scene is consistent enough to render.

Content is identical across variants on purpose; only the notation changes. Names
(`SceneEdit`, `Epoch`, ...) are placeholders, not decisions.

---

## Variant A: typed IDL sketch

Interface-first, reads like a stripped-down CORBA/Smithy/WIT. Types and operations are
declared; invariants and ordering hang off them as annotations.

```idl
// Building block: Scene Ingest            Level: 1
// Channel: in-process (same address space as the Hydra render delegate)

type PrimPath     = opaque           // Hydra SdfPath
type LocatorSet   = set<DataLocator> // which schema fields changed
type Epoch        = monotonic uint   // one per committed batch of edits

record PrimNotice {
  path:     PrimPath
  kind:     enum { added, removed, dirtied, renamed }
  primType: TypeToken?               // present for `added`
  locators: LocatorSet?              // present for `dirtied`
  newPath:  PrimPath?                // present for `renamed`
}

record SceneEdit {
  epoch:   Epoch
  changes: list<InternalChange>      // geometry, instance, material, light, camera, settings
}

interface SceneIngest {
  // Inbound from Hydra (observer side).
  @caller(hydra) @thread(hydra-sync)
  notify(notices: list<PrimNotice>) -> void
    requires: notices are in Hydra's delivery order
    ensures:  each notice is recorded as pending; no internal-scene mutation yet

  // Called by the render control half when Hydra asks to render.
  @caller(render-control) @thread(hydra-sync)
  commit() -> SceneEdit
    requires: render loop is paused, or epoch == 0
    ensures:  all pending notices are folded into exactly one SceneEdit
              result.epoch == previous epoch + 1
              pending set is empty

  // Outbound to the internal scene.
  @callee(internal-scene)
  emits apply(edit: SceneEdit)
}

invariant: Internal scene never observes a partially translated prim.
invariant: Notices referring to unsupported prim types are dropped and reported once per type.
invariant: Epochs are strictly increasing; workers render against exactly one epoch.
```

**Strengths:** compact, closest to code a reviewer can mentally compile, types are
reusable across contracts.
**Weaknesses:** drifts toward real signatures (the destination forbids them); invariants
and threading feel bolted on; "channel" is a comment, not a first-class field.

---

## Variant B: operation table with pre/postconditions

Design-by-contract as prose tables. Each operation is a row; types are described, not declared.

**Building block:** Scene Ingest · **Level:** 1 · **Channel:** in-process

**Responsibilities**
- Observe Hydra's terminal scene index.
- Translate prim notices into internal-scene changes.
- Batch changes into atomic, numbered edits.

**Operations**

| Operation | Caller → callee | Inputs | Outputs | Preconditions | Postconditions | Thread / ordering |
|---|---|---|---|---|---|---|
| notify | Hydra → Scene Ingest | prim notices (path, kind, type or changed fields) | none | notices in Hydra delivery order | notices recorded as pending; internal scene untouched | Hydra sync thread; order preserved |
| commit | Render control → Scene Ingest | none | one scene edit with a new epoch | render loop paused, or first commit | pending folded into one edit; epoch +1; pending empty | Hydra sync thread; serialised with notify |
| apply | Scene Ingest → Internal scene | scene edit | none | edit epoch = scene epoch + 1 | scene at new epoch | before render loop resumes |

**Data exchanged**

| Name | Meaning |
|---|---|
| prim notice | added / removed / dirtied (with changed fields) / renamed, for one prim path |
| scene edit | epoch plus the list of internal changes (geometry, instance, material, light, camera, settings) |
| epoch | monotonic counter, one per committed edit |

**Invariants**
- The internal scene never observes a partially translated prim.
- Unsupported prim types are dropped and reported once per type.
- Epochs strictly increase; workers render against exactly one epoch.

**Strengths:** no syntax to learn, renders natively on GitHub, impossible to mistake for
code, pre/post conditions are first-class.
**Weaknesses:** wide tables are painful on narrow screens and in diffs; data types are
fuzzy; repeated shapes (e.g. scene edit) are restated per contract.

---

## Variant C: message-oriented schema

Contracts as the messages that cross the boundary, plus a small protocol (state machine).
Neutral about whether a message is a function call or a network packet.

```yaml
block: Scene Ingest
level: 1
channel: in-process

responsibilities:
  - Observe Hydra's terminal scene index.
  - Translate prim notices into internal-scene changes.
  - Batch changes into atomic, numbered edits.

messages:
  PrimNotices:            # Hydra -> Scene Ingest
    from: Hydra
    to: Scene Ingest
    carries:
      - path: prim path
      - kind: added | removed | dirtied | renamed
      - prim type: when added
      - changed fields: when dirtied
      - new path: when renamed
  CommitRequest:          # Render control -> Scene Ingest
    from: Render control
    to: Scene Ingest
    carries: nothing
  SceneEdit:              # Scene Ingest -> Internal scene
    from: Scene Ingest
    to: Internal scene
    carries:
      - epoch: monotonic counter
      - changes: geometry | instance | material | light | camera | settings

protocol:
  states: [Idle, Pending, Committed]
  transitions:
    - Idle      --PrimNotices-->   Pending
    - Pending   --PrimNotices-->   Pending
    - Pending   --CommitRequest--> Committed   / emit SceneEdit(epoch+1)
    - Idle      --CommitRequest--> Idle        / no edit
    - Committed --(applied)-->     Idle

ordering:
  - PrimNotices are consumed in delivery order.
  - CommitRequest is serialised with PrimNotices (single Hydra sync thread).
  - SceneEdit is applied before the render loop resumes.

invariants:
  - The internal scene never observes a partially translated prim.
  - Unsupported prim types are dropped and reported once per type.
  - Epochs strictly increase; workers render against exactly one epoch.
```

**Strengths:** the same shape works for in-process and network contracts (Distribution,
Film merge) without changing notation; the protocol section makes ordering explicit;
maps directly onto arc42 §6 runtime scenarios.
**Weaknesses:** call/return contracts (e.g. a BSDF evaluating a lane) read awkwardly as
messages; YAML looks like config a tool should parse; preconditions live only implicitly
in the state machine.

---

## Template every contract entry follows (any variant)

1. Header: block name, level (1 or 2), parent block, channel (in-process / cross-process / network).
2. Responsibilities: 2–5 bullets, what it owns and what it explicitly does not.
3. Operations or messages: direction, inputs, outputs.
4. Data exchanged: named shapes, defined once, referenced elsewhere.
5. Invariants: always-true statements an implementation can be tested against.
6. Threading and ordering: who calls from where, what is serialised, what may run concurrently.
7. Failure: what the block reports, and what its callers must tolerate.
8. Open questions: links to tickets.
