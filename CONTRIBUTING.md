# Contributing to Umbra

Thank you for your interest in Umbra. This document describes how the project
is developed and what every change must satisfy. By participating you agree to
follow the [Code of Conduct](CODE_OF_CONDUCT.md).

All code, comments, documentation, commit messages, issues and pull requests
are written in English.

## Environment

**The devcontainer is the only supported environment.** It pulls a prebuilt
image pinned by content hash in `.devcontainer/`, so every machine and every
CI run uses the same LLVM 23.1.6 toolchain (clang, libc++, compiler-rt,
libunwind, lld, lldb) and the same third-party libraries. Problems that cannot
be reproduced inside the devcontainer are not supported.

The devcontainer works with Docker and with rootless Podman. When it is
created it:

- installs the git hooks (`git config core.hooksPath .githooks`);
- enables commit and tag signing in the repository's local configuration;
- configures the debug CMake preset.

## Compatibility policy

Umbra is pre-1.0. **The API may break in any release before 1.0.0.** Breaking
changes bump the minor version until then; see
[Versioning and releases](#versioning-and-releases).

## Daily commands

The Makefile is a thin wrapper over CMake presets and the shared checks:

| Target | Action |
| --- | --- |
| `make` / `make all` | Debug build |
| `make test` | Debug build and run the tests |
| `make release` | Release build and tests |
| `make asan` | AddressSanitizer + UndefinedBehaviorSanitizer build and tests |
| `make tsan` | ThreadSanitizer build and tests |
| `make format` | Format the sources with clang-format |
| `make lint` | Run clang-tidy and the clang-query rules |
| `make checks` | Run the validators shared with the git hooks and CI |
| `make clean` | Remove the build trees |
| `make help` | List the targets |

CI uses the same targets where they exist.

## Git flow

Umbra follows git flow:

| Branch | Purpose | Branches from | Merges into |
| --- | --- | --- | --- |
| `main` | Released code; every merge is tagged `vX.Y.Z` | | |
| `develop` | Integration of the next release | `main` | |
| `feature/<name>` | New work | `develop` | `develop` (squash) |
| `bugfix/<name>` | Fixes for unreleased code | `develop` | `develop` (squash) |
| `release/X.Y.Z` | Release preparation | `develop` | `main` and `develop` (`--no-ff`) |
| `hotfix/X.Y.Z` | Urgent fixes for a release | `main` | `main` and `develop` (`--no-ff`) |

- Branch names are kebab-case, for example `feature/wrapping-arithmetic`.
  The pre-push hook and CI reject other names.
- Features and bugfixes are **squash-merged** into `develop`. The pull request
  title becomes the commit header, so it must be a valid Conventional Commit;
  CI checks it.
- Releases and hotfixes are merged with `--no-ff` and tagged `vX.Y.Z` on
  `main`.
- git-flow-next is available in the devcontainer as an optional helper.

## Commit messages

Commits follow [Conventional Commits 1.0.0](https://www.conventionalcommits.org/en/v1.0.0/).
The commit-msg hook checks every commit, and CI checks every commit of a pull
request.

```text
<type>[(<scope>)][!]: <description>

[body]

[footers]
```

- **Types**: `feat`, `fix`, `perf`, `refactor`, `docs`, `test`, `build`, `ci`,
  `style`, `chore`, `revert`.
- **Scope** is optional. When present it must be the name of an existing
  module or one of `deps`, `ci`, `devcontainer`, `lint`.
- **Description**: English, imperative mood, lowercase, no trailing period.
- **Header** at most 72 characters.
- **Breaking changes** are marked with `!` after the type or scope, or with a
  `BREAKING CHANGE:` footer.
- `fixup!` and `squash!` commits are rejected by CI; squash them locally
  before pushing.

Examples:

```text
feat(core): add wrapping multiply helper
fix: report the dirty flag when only untracked files changed
build(deps): bump googletest to v1.18.0
```

## Signed commits and tags

Every commit and release tag must be cryptographically signed.

- Signing uses SSH (`gpg.format ssh`) with your existing signing key, kept in
  the host's ssh-agent. The devcontainer forwards the agent and your host git
  configuration, so private keys never enter the container.
- The pre-push hook rejects pushes that contain unsigned commits.
- CI requires every pull request commit to be reported as verified by GitHub,
  and the release job verifies that release tags are signed. Add your signing
  key to your GitHub account as a **signing key** so your commits verify.
- Squash merges, Dependabot commits and automation commits (such as the
  devcontainer image pin update) are signed by GitHub.

## Git hooks

The hooks in `.githooks/` are Python 3 standard-library executables that
share their validators (in `checks/`) with CI, so local feedback matches CI:

- **pre-commit** checks staged files only: formatting (git-clang-format),
  clang-tidy in parallel on the affected translation units, the clang-query
  rules and the shared validators.
- **commit-msg** validates the Conventional Commit format.
- **pre-push** validates branch names, rejects unsigned commits, and runs a
  quick debug build and test.

The hooks use LLVM's own tools (git-clang-format, run-clang-tidy /
clang-tidy-diff, clang-query) rather than reimplementations.

## Safety rules

Umbra aims for as many Rust-like guarantees as C++ allows. These rules are
enforced by compiler errors, clang-tidy, clang-query, the hooks and CI.

### Errors and language features

- C++23 with compiler extensions off.
- Exceptions and RTTI are disabled. Errors are returned as `std::expected`
  with the project's `[[nodiscard]]` error type.
- Only `umbra::ext::<dep>` bridge modules (under `src/ext/` and
  `include/umbra/ext/`) may enable RTTI or exceptions or include the headers
  of libraries that need them. They expose only umbra types and convert
  exceptions to `std::expected`.

### Memory and ownership

- `-Wunsafe-buffer-usage` is an error: no pointer arithmetic or raw indexing.
  Unsafe buffer access is allowed only between the unsafe-buffer begin/end
  macros of the safety header, **immediately preceded by a `// SAFETY:` comment** that
  explains why the access is sound. The hooks and CI verify the comment.
- No raw `new`/`delete`/`malloc`/`free`, no C arrays. Follow the rule of
  five, make locals `const` by default, initialize everything, and mark
  single-argument constructors `explicit`. Use-after-move is detected.

### Lifetimes

- Functions that return references, pointers, spans or views derived from
  `this` or from a parameter **must** be marked with the lifetimebound macro
  (or the lifetime-capture-by macro when storing a reference). Owner and
  pointer types use the gsl Owner/Pointer annotation macros. All safety
  macros live in the safety header and are prefixed `UMBRA_`.
- `-Wdangling` and the experimental lifetime-safety analysis are errors.

### Thread safety

- Shared mutable state must be guarded by a capability, atomic, or `const`.
- Clang Thread Safety Analysis is an error. Annotate locks and guarded data
  with the capability, guarded-by, requires, excludes and acquire/release
  macros. ThreadSanitizer in CI is the runtime backstop.
- Call the core floating-point configuration function (FTZ/DAZ) first on
  every thread, tests included.

### Nullability

- Every header wraps its declarations in the assume-nonnull begin/end macros;
  the hooks and CI verify it. Pointers are non-null by default, and nullable
  pointers are marked with the nullable macro.
- Nullability warnings are errors, the clang analyzer nullability checks run,
  and test builds run the nullability sanitizer.

### Integers, conversions and booleans

- Implicit narrowing, sign, 64-to-32, int, float and double-promotion
  conversions are errors: write an explicit `static_cast`.
- Test builds abort on signed and unsigned overflow, implicit conversion,
  shift, divide-by-zero and float-cast overflow. Intentional overflow (RNG,
  hashing) uses the wrapping arithmetic helpers (add, sub, mul, shl) only.
- `bool` parameters and `bool` members are forbidden (including
  `std::optional<bool>`, `std::vector<bool>` and `std::bitset` used as flags).
  Use an `enum class` with an explicit underlying type.
- `bool` is allowed only as a predicate return type, a local variable, or
  `std::atomic<bool>` for inter-thread signalling. Predicates start with
  `Is`, `Has`, `Can`, `Should`, `Contains` or `Equals`.
- Implicit conversions to `bool` are errors: compare explicitly.

### Floating point

- `-ffast-math` is never applied globally. Fast-math is allowed only in local
  regions using clang floating-point pragmas, **preceded by a `// SAFETY:`
  comment** that justifies the precision trade-off.

### Style and lint

- Formatting is clang-format with Google style and left pointer alignment.
- clang-tidy runs the google, bugprone, cert, cppcoreguidelines,
  clang-analyzer, concurrency, misc, modernize, performance, portability and
  readability families, all as errors.
- Google naming: `CamelCase` types and functions, `lower_case` variables,
  trailing underscore for class members, `kConstants`, `UMBRA_` macros.
- Functions are limited to 40 lines, 30 statements, 8 branches, nesting depth
  3, 5 parameters, 12 variables and cognitive complexity 15.
- Every `NOLINT` names the check and gives a reason, for example
  `// NOLINT(bugprone-narrowing-conversions): <reason>`. The clang-query
  rules honour `NOLINT(umbra-<rule>)` suppressions only when they carry a
  reason.
- The namespace matches the directory.

### Modules

- Each module is an isolated static CMake library that sees only the headers
  of the modules it explicitly depends on. The module graph must be acyclic;
  CI checks it.
- Each module has a short README stating its single responsibility.

### Licensing

- Every source file starts with an SPDX header identifying the license as
  `Apache-2.0` (`SPDX-License-Identifier: Apache-2.0`); the hooks and CI
  verify it.

- Third-party dependencies must use a license on the allow-list:
  Apache-2.0, BSD-2-Clause, BSD-3-Clause, MIT, Zlib or BSL-1.0.

## Testing policy

- Tests use GoogleTest through CTest and exercise the **public interface** of
  a module only, never generated headers or internal plumbing.
- Tests are added only to modules the maintainers explicitly request. There
  is no one-test-per-source rule and no coverage gate; the coverage preset is
  for local reports only.
- A tested source file gets one test file under `tests/<module>/`, mirroring
  the module tree.
- Test bodies follow the same lint rules, except the function-size limit and
  cognitive complexity.
- Infrastructure (hooks, checks, workflows, CMake, Dockerfiles) has no tests;
  it is validated by use.
- Tests run in the debug, release, asan and tsan presets, both locally and in
  CI.

## Pull requests

1. Open an issue first for anything beyond a small fix.
2. Branch from `develop` following the git flow naming rules.
3. Keep commits small, themed and signed.
4. Run `make checks`, `make lint` and `make test` inside the devcontainer.
5. Open the pull request against `develop` and fill in the template. Its title
   must be a Conventional Commit.

Every pull request must pass the required CI jobs: conventions, format, lint,
build-debug, build-release, sanitize-address, sanitize-thread, modules and,
on release and hotfix branches, release.

## Versioning and releases

Umbra follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html),
starting at 0.1.0. The version is declared in the CMake project.

| Change | Before 1.0.0 | From 1.0.0 |
| --- | --- | --- |
| Breaking change | minor | major |
| `feat` | minor | minor |
| `fix` | patch | patch |

The move to 1.0.0 is a deliberate, manual decision by the maintainers.

To cut a release:

1. Run `git cliff --bumped-version` to compute the next version.
2. Create `release/X.Y.Z` from `develop`.
3. Set the CMake project version to `X.Y.Z` and prepend the changelog section
   drafted by git-cliff to [CHANGELOG.md](CHANGELOG.md), dated, in
   [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) format.
4. Open a pull request into `main`. CI checks that the branch version equals
   the CMake version and git-cliff's bumped version, and that the changelog
   has a dated section for it.
5. Merge with `--no-ff`, create the signed tag `vX.Y.Z` on `main`, and merge
   `main` back into `develop`.

Hotfixes follow the same steps from `main` with `hotfix/X.Y.Z`.

## Third-party dependencies

- Dependencies are git submodules under `third_party/`, pinned to official
  release tags, never patched in-tree. Patches live in a dedicated directory
  and are applied at build time.
- The versions manifest lists the name, tag, commit and license of each
  submodule; CI checks it against the actual submodule commits.
- A new dependency arrives in its own `build(deps): add <dep>` pull request.
- Dependency images are rebuilt by the deps workflow only when third-party,
  Docker or superbuild inputs change.

## Reporting issues

Use the issue forms for bugs and feature requests. Report security
vulnerabilities and Code of Conduct violations privately, as described in
[SECURITY.md](SECURITY.md).

## License

By contributing, you agree that your contributions are licensed under the
[Apache License, Version 2.0](LICENSE), and that copyright notices read
"Copyright The Umbra Authors".
