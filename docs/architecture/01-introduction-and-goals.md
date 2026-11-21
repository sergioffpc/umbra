# 1. Introduction and goals

## 1.2 Quality goals

The goals are ranked. When two goals conflict, the higher one wins.

| # | Goal | Summary |
|---|------|---------|
| 1 | Physical and spectral fidelity | Predictive, unbiased spectral light transport in absolute physical units |
| 2 | Scene scale | Production scenes render whole, including scenes larger than one node's memory |
| 3 | Distributed scalability | One frame spreads across many executors and nodes without a central bottleneck |
| 4 | Robustness and safety | Bad input, failed nodes and memory pressure never lose a long render |
| 5 | Interactivity | Edits show up quickly, on the same engine and image as batch rendering |

### 1. Physical and spectral fidelity

- **1.1 Spectral end to end.** All light transport inside Umbra is spectral (hero wavelength). RGB exists only at the two edges: authored values coming in through OSL and MaterialX, and the film going out through OCIO.
- **1.2 Unbiased by default.** The default estimators are unbiased and consistent. Any biased technique (firefly clamping, radiance caches, denoising) is opt-in, and turning it on is recorded in the image metadata.
- **1.3 Statistical reproducibility.** Every execution topology produces an image with the same expected value and no worse variance. This covers node count, thread count, executor kinds, placement plans, ray migration and node failures. Bit-exact output is not required.
- **1.4 Absolute physical units.** Lights, camera and film use absolute radiometric and photometric units. The camera has a physical exposure model and a configurable spectral sensor response.
- **1.5 Energy conservation.** Every closure and medium conserves energy, and is reciprocal where physics requires it.
- **1.6 Dispersion and fluorescence.** Dispersion is supported. Fluorescence is supported in the core: path state and the BSDF contract both allow wavelength-changing events. Both are authored through Umbra extensions to MaterialX and USD.

### 2. Scene scale

- **2.1 Single node.** A scene renders on a single render node whenever its resident part fits in that node's memory. The resident part is geometry, acceleration structures and shade-before-hit data. Textures and volumes are always out-of-core behind bounded caches, so they never count against that limit.
- **2.2 Beyond one node.** A scene whose resident part exceeds the memory of every single executor renders with selective replication, provided it fits in the aggregate memory of the executors. No executor exceeds its memory budget.
- **2.3 Compact representation.** The resident representation is compact (instancing never copies, among other things), so that 2.2 is the exception rather than the rule.

### 3. Distributed scalability

- **3.1 Full replication.** Under full replication, sampling throughput grows with the aggregate capacity of possibly heterogeneous executors. No central component, such as coordination or film merge, becomes a serialisation point as their number grows.
- **3.2 Selective replication.** Under selective replication, the placement planner minimises ray migration traffic. The render reports enough migration telemetry to diagnose and tune it: the migrated fraction, traffic per link and network wait.
- **3.3 Dynamic balancing.** Load balancing is dynamic, so an executor that is slow, saturated or joins late delays the frame by no more than its share of the work.

### 4. Robustness and safety

- **4.1 Memory and thread safety.** Memory and thread safety are guaranteed by construction, enforced by the compiler, static analysis and CI.
- **4.2 Malformed input.** Malformed input never takes down a render. This covers NaN and Inf values, invalid USD prims, OSL shaders that fail to compile and missing textures. The offending element is isolated, replaced by a visible fallback and reported, and the render continues.
- **4.3 Node loss.** Losing a render node never loses the frame. Its work is reassigned, and a shard whose only holder failed is re-placed, provided the remaining aggregate memory suffices. If it does not, the render fails explicitly and with a diagnosis.
- **4.4 Memory pressure.** Memory pressure degrades rather than aborts: caches give way before the process is killed. Each executor's memory budget is an explicit parameter.
- **4.5 Resume.** A long render can be resumed from a checkpoint after a total failure.

### 5. Interactivity

- **5.1 Incremental edits.** An edit arriving as a Hydra notice invalidates only what depends on it, never the whole scene. Interactive shader parameters change without recompiling.
- **5.2 First image fast.** After an edit, a first image covering the whole frame comes first and is then refined progressively, with priority given to the region in view.
- **5.3 Same engine.** Interactive rendering uses the same core, shards and placement plans as batch. Edits propagate only to the executors holding the affected shards, and the placement plan stays stable during a session.
- **5.4 Same image.** Interactive rendering uses the same estimator as batch and converges to the same image. There is no approximate preview mode.
