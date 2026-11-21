# CUDA and OptiX GPU executors with full feature parity

Umbra runs on CPUs and on NVIDIA GPUs through CUDA and OptiX. A GPU is not a separate render mode. It is an executor with its own memory budget inside the placement plan, so the CPU and the GPUs of one render node work on the same frame. Rays migrate between them through the same mechanism they use to migrate between nodes. We chose this so that GPU acceleration doesn't cap scene scale at VRAM size: a shard that doesn't fit on a GPU stays on the CPU, and the planner gives the GPUs the hottest shards. All executors produce the same image with the same estimator, so no closure, primitive or technique may exist on only one of them.

## Considered options

- **CPU only, contracts open to a GPU later:** rejected because GPU acceleration is a goal now.
- **GPU as an all-or-nothing alternative mode:** rejected because it limits GPU renders to scenes that fit in VRAM.
- **GPU only for interactive rendering:** rejected because batch and interactive must share one engine.

## Consequences

- **NVIDIA only.** No AMD or Intel GPU support.
- **First non-permissive dependency.** The CUDA toolkit and the OptiX SDK are proprietary, so the GPU backend is optional at build time. Umbra must still build and run with permissive dependencies only.
- **LLVM toolchain kept.** clang compiles CUDA. OSL's OptiX backend generates PTX from the same shaders.
- **Single-source closures.** Umbra's spectral closures compile for both host and device, which restricts the C++ they may use.
- **Two traversal backends.** Embree accelerates the CPU executor and OptiX accelerates the GPU executors. Each sees only its local shards.
