# Third-party versions

Every git submodule under `third_party/`, pinned to an official release tag.
CI checks that each row matches the submodule commit recorded in the
repository and that each license is on the allow-list. Update the row in the
same commit that moves a submodule.

Columns: `Path` is the submodule path, `Tag` the upstream release tag,
`Commit` the full commit the tag points to, and `License` an SPDX license
expression.

| Name | Path | Tag | Commit | License |
|---|---|---|---|---|
| LLVM | `third_party/llvm-project` | `llvmorg-23.1.6` | `5b10d2d74929eae78912771222c9855af97496f4` | `Apache-2.0 WITH LLVM-exception` |
| GoogleTest | `third_party/googletest` | `v1.18.0` | `063de7e9578f82b369302001269680b4b1553359` | `BSD-3-Clause` |
| Google Benchmark | `third_party/benchmark` | `v1.10.0` | `a6b4a9f00b36c19af9997d68a1cf6e0b5b3d032c` | `Apache-2.0` |
