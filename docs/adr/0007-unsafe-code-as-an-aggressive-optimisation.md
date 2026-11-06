# `unsafe` code as an aggressive optimisation

Umbra code is safe Rust by default. `unsafe_code = "deny"` applies to every crate of the workspace. SIMD, byte casts, thread pinning and allocators come from crates with safe APIs (`wide` behind the `Vec4`/`Mask4` facade, `bytemuck`, …), so the expected amount of `unsafe` in Umbra itself is zero.

An `unsafe` exception is treated as an **aggressive optimisation** and has to meet the same rule as any other: a benchmark showing the gain end-to-end ([arc42 §8.4](../architecture/08-crosscutting-concepts.md#84-performance-measurement)), isolation behind a simple safe contract, and a readable, book-faithful safe reference implementation that the unsafe version is tested against. On top of that rule:

- **Narrowest scope.** The exception is a `#[expect(unsafe_code, reason = "...")]` on the narrowest item possible, never on a module or crate. The lint stays at `deny` and never at `forbid`, because `forbid` cannot be overridden locally.
- **Documented.** Every `unsafe` block has a `// SAFETY:` comment stating why its preconditions hold, and holds only one unsafe operation. Every `unsafe fn` has a `# Safety` section. `SAFETY` comments appear only on `unsafe` code. All of this is enforced by lints.
- **Verified by tools.** The first exception brings a CI job that runs the tests of every `unsafe` item under Miri, on a nightly toolchain pinned for that job only. Where Miri cannot execute the code (unsupported `std::arch` intrinsics, FFI), those tests run under AddressSanitizer on Linux nightly. The tests include the equivalence tests against the safe reference. ThreadSanitizer is not required, because parallelism comes from crates with safe APIs.

There is no fixed list of modules allowed to contain `unsafe`. Each exception is justified on its own, wherever it lives.

## Considered options

- **`forbid` everywhere**, with all `unsafe` delegated to external crates: rejected. It rules out optimisations a benchmark may later justify, such as unchecked indexing into the arenas or hand-written `std::arch` kernels where `wide` falls short.
- **A fixed set of unsafe modules** (`simd`, `arena`, `platform`), with `unsafe` free inside them: rejected. It fixes module boundaries before they are designed, and makes `unsafe` acceptable by location rather than by measured need.
- **A Miri CI job from day one**: deferred. With no `unsafe` code it would test nothing, so it lands with the first exception.

## Consequences

- An `unsafe` PR carries more weight than any other optimisation: benchmark report, safe reference, equivalence tests, Miri or ASan job.
- Reviewers can find every exception with a search for `expect(unsafe_code`.
