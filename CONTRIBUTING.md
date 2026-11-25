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

Nothing depends on a GCC runtime: CI fails when any binary of the toolchain,
of the dependencies or of an umbra build tree needs libstdc++, libgcc_s or
GCC's libatomic (`.github/actions/no-gcc-runtime`).

The image also carries the NVIDIA development stack under `/opt/cuda`: CUDA
13.5 (nvcc, cudart, CCCL) and the OptiX 9.1 headers. nvcc compiles device code
only, with clang as its host compiler; the GPU backend is a module,
`libumbra-gpu.so`, that umbra loads at run time.
CI fails when any binary other than such a module needs the CUDA driver,
`libcuda.so.1` (`.github/actions/no-libcuda`). Those NVIDIA tools are not
redistributable, so the image is a **private** package: log in to the GitHub
Container Registry before opening the devcontainer, with a personal access
token that has `read:packages`:

```sh
podman login ghcr.io --compat-auth-file ~/.docker/config.json
```

If the host's `gh` is logged in with the `read:packages` (or `write:packages`)
scope, its token works too:

```sh
gh auth token | podman login ghcr.io --username "$(gh api user --jq .login)" \
    --password-stdin --compat-auth-file ~/.docker/config.json
```

The Dev Containers extension reads only `~/.docker/config.json`, never
Podman's own login file, which lives in `$XDG_RUNTIME_DIR` and is gone after a
reboot; Podman reads `~/.docker/config.json` too.

The devcontainer is set up for VS Code Dev Containers on rootless Podman
(set `"dev.containers.dockerPath": "podman"` in your VS Code user settings).
It maps your host user onto the container's non-root `dev` user (keep-id user
namespace), so workspace files keep your ownership, and allows ptrace with an
unconfined seccomp profile so that lldb and the sanitizers work. When it is
created it:

- installs the git hooks (`git config core.hooksPath .githooks`);
- enables SSH commit and tag signing in the repository's local configuration
  (`gpg.format ssh`, `commit.gpgsign`, `tag.gpgsign`);
- configures the debug CMake preset;
- installs the GitHub CLI (`gh`, pinned devcontainer feature) and Claude Code
  (Anthropic's native installer), with the `mattpocock-skills` plugin that
  `.claude/settings.json` enables. These developer tools are never part of
  the CI images.

Your Claude Code and `gh` logins live in the named volumes `umbra-claude` and
`umbra-gh`, so they survive rebuilds.

VS Code gets clangd (live clang-tidy, background index, no header insertion,
format on save), lldb-dap and CMake Tools; Microsoft's C/C++ extension is
opted out and its IntelliSense disabled. `.clangd` also attaches the static
index of the third-party sources shipped in the image, so Go to Definition and
Find References reach into the dependencies' implementations.

### GPU hosts

The GPU is optional: the same devcontainer configuration starts on a host
without one, and umbra then reports no GPU executor. On a host with an NVIDIA
GPU (RTX 5070 Ti class, `sm_120`, or newer), the devcontainer sees it once
the host is set up:

1. Install the NVIDIA driver, R590 or newer; OptiX 9.1 needs it. (An R580
   driver loads CUDA but not OptiX 9.1; whether R590 reads the OptiX-IR of
   nvcc 13.5 or R620 is needed is still to be confirmed.)
2. Install the NVIDIA Container Toolkit, 1.18 or newer. Its
   `nvidia-cdi-refresh` service writes the CDI specification of your GPUs to
   `/var/run/cdi/nvidia.yaml` and rewrites it after every driver update. Do
   not also generate one in `/etc/cdi`: that copy goes stale on the next
   driver update, and the container then fails to mount the driver's
   libraries. After a driver update, reboot before rebuilding the container,
   so that the loaded kernel module matches the driver's libraries:

   ```sh
   nvidia-smi    # fails with "Driver/library version mismatch" until then
   ```

3. Give your rootless Podman containers the GPUs, in
   `~/.config/containers/containers.conf`:

   ```toml
   [containers]
   devices = ["nvidia.com/gpu=all"]
   ```

   This applies to every container of your user on the host. Never add
   `--device` to the devcontainer's `runArgs`: hosts without a CDI
   specification then fail to start it. The devcontainer's
   `NVIDIA_DRIVER_CAPABILITIES` include `graphics`, which mounts the
   driver's OptiX runtime.

4. Rebuild the devcontainer and check: `nvidia-smi -L` lists the GPU,
   and `build/debug/bin/umbra devices` lists the GPU as `gpu0`.

## Compatibility policy

Umbra is pre-1.0. **The API may break in any release before 1.0.0.** Breaking
changes bump the minor version until then; see
[Versioning and releases](#versioning-and-releases).

## Daily commands

`forge` runs the CMake presets and the shared checks, as
`python3 -m forge <command>` from the repository root:

| Command | Action |
| --- | --- |
| `build` | Debug build; `--preset release` builds the release preset (LTO) instead |
| `check` | Build and run the tests, except those that need a GPU; `--label gpu` runs only the tests with that CTest label; `--preset release` as for `build` |
| `check-asan` | AddressSanitizer + UndefinedBehaviorSanitizer build and tests; `--label gpu` runs only the tests with that label, also under NVIDIA's compute-sanitizer (memcheck and initcheck), on a GPU host |
| `check-tsan` | ThreadSanitizer + UndefinedBehaviorSanitizer integer, conversion and nullability checks build and tests; `--label gpu` runs only the tests with that label, also under compute-sanitizer (racecheck and synccheck), on a GPU host |
| `check-all` | `lint`, `check`, `check-asan` and `check-tsan`, stopping at the first failure: everything to run before a pull request |
| `coverage` | Coverage build and tests, then the coverage report (local only) |
| `format` | Format the C++ (clang-format) and Python (pyink, isort) sources |
| `lint` | Check without changing any file: the validators shared with the git hooks (the file validators on every tracked file, the current branch name, the commit messages since `develop`), the formatting, then clang-tidy, the clang-query rules, pylint, actionlint and the embedded Python checks |
| `clean` | Remove what the builds built, keeping their configuration |
| `distclean` | Remove the build trees, with their configuration |

`python3 -m forge --help` lists the commands, and
`python3 -m forge <command> --help` their options. `-v`/`--verbose` makes
any command that runs CMake or CTest as verbose as its tools allow: the
configure's messages down to `TRACE`, every command the build runs, and the
full output of every test, e.g. `python3 -m forge check -v`. Every step is
echoed to stderr before it runs, prefixed with `+ `, and a command stops at
the first step that fails, with its exit status.

CI does not use forge: it runs the CMake workflow presets and the checks
directly.

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

- Disposable branches hold research and prototyping work: `research/<name>`
  and `prototype/<name>`. They may be committed to and pushed, but they never
  get a pull request, are never merged, and may be deleted at any time. Their
  outcome, findings or a prototype, is posted as a comment on the issue that
  asked for it, so deleting the branch loses nothing. Only the git flow
  branches in the table above are opened as pull requests.
- Branch names are kebab-case, for example `feature/wrapping-arithmetic`.
  The pre-push hook rejects other names, and CI also rejects `research/` and
  `prototype/` as pull request branches; only Dependabot's own
  `dependabot/...` branches are exempt in CI.
- Features and bugfixes are **squash-merged** into `develop`. Squash is the
  only merge method the repository allows on GitHub: merge commits and
  rebase merging are disabled.
- The squash commit takes the pull request title as its header, so the title
  must be a valid Conventional Commit; CI checks it. The commit body joins
  the messages of the branch's commits.
- Releases and hotfixes are merged with `--no-ff` and tagged `vX.Y.Z` on
  `main`. GitHub cannot create those merge commits, so they are made
  locally and pushed; see
  [Versioning and releases](#versioning-and-releases).
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
  module (a directory directly under `src/`) or one of `deps`, `ci`,
  `devcontainer`, `lint`.
- **Description**: English (ASCII only), imperative mood, starting in
  lowercase, no trailing period.
- **Header** at most 72 characters, followed by a blank line when there is a
  body.
- **Breaking changes** are marked with `!` after the type or scope, or with a
  `BREAKING CHANGE:` footer.
- `fixup!`, `squash!` and `amend!` commits are accepted by the commit-msg
  hook but rejected by CI; squash them locally (`git rebase --autosquash`)
  before opening or updating a pull request.
- Pull request history is linear: rebase instead of merging. Only release and
  hotfix pull requests into `main` may contain merge commits (the git flow
  back-merges), and those are not checked as Conventional Commits.

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
- Squash merges, Dependabot commits and automation commits (the devcontainer
  image pin update and the submodule bumps) are signed by GitHub.

## Developer Certificate of Origin

Every commit must be signed off under the
[Developer Certificate of Origin 1.1](DCO): by adding a
`Signed-off-by: Name <email>` trailer, you certify that you wrote the change,
or otherwise have the right to submit it under the project's license, and
that the record of your contribution, sign-off included, is public. The
sign-off is a statement, not a cryptographic signature; commits need both.

- Commit with `git commit -s`, which adds the trailer from your `user.name`
  and `user.email`. The sign-off must carry the email of the commit author.
  Commits from VS Code's Source Control view are signed off automatically
  (`git.alwaysSignOff` in `.vscode/settings.json`).
- Sign off with your real name. A sign-off cannot be made on someone else's
  behalf: when you submit work written by others, keep their sign-off and add
  yours.
- To add a forgotten sign-off, amend the last commit
  (`git commit --amend --no-edit -s`) or rebase the branch
  (`git rebase --signoff develop`), then force-push it.
- The commit-msg hook and CI reject commits without a sign-off matching the
  author. Merge commits and commits authored by bots (Dependabot and the
  automation workflows) are exempt.

## Git hooks

The hooks in `.githooks/` are Python 3 standard-library executables that
share their validators (in `checks/`) with CI, so local feedback matches CI:

- **pre-commit** checks staged files only: formatting (git-clang-format,
  pyink, isort), the shared validators, clang-tidy in parallel and the
  clang-query rules on the affected translation units (those that include a
  staged file), pylint on staged Python files, and actionlint and the
  embedded Python checks on staged workflows.
- **commit-msg** validates the Conventional Commit format and the
  Developer Certificate of Origin sign-off.
- **pre-push** validates branch names, rejects unsigned commits, and runs a
  quick debug build and test.

The hooks use LLVM's own tools (git-clang-format, run-clang-tidy,
clang-query) rather than reimplementations.

## Safety rules

Umbra aims for as many Rust-like guarantees as C++ allows. These rules are
enforced by compiler errors, clang-tidy, clang-query, the hooks and CI.

### Errors and language features

- C++23 with compiler extensions off.
- Exceptions and RTTI are disabled. Errors are returned as
  `std::expected<T, Error>` with the project's `[[nodiscard]]` error type
  (or another error type where one fits).
- Results are must-use, as in Rust: discarding any `std::expected` fails
  lint (`bugprone-unused-return-value`), and a cast to `void` does not
  silence it.
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
  The experimental and beta analyses sit behind the `UMBRA_EXPERIMENTAL_SAFETY`
  CMake option (ON); turn it off only to work around a noisy compiler release.

### Thread safety

- Shared mutable state must be guarded by a capability, atomic, or `const`.
- Clang Thread Safety Analysis is an error. Annotate locks and guarded data
  with the capability, guarded-by, requires, excludes and acquire/release
  macros. ThreadSanitizer in CI is the runtime backstop.
- ThreadSanitizer targets umbra's own code. `sanitizers/tsan.supp` may only
  suppress reports from uninstrumented dependencies proven to cause false
  positives (`called_from_lib`), each entry preceded by a comment that
  justifies it.
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

C++ follows the
[Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html)
and Python follows the
[Google Python Style Guide](https://google.github.io/styleguide/pyguide.html).
Where this document is stricter (C++23, the safety rules above, the extra
clang-tidy families and the function limits below), this document wins.

- Formatting is clang-format with Google style; pointers bind to the type
  (`int* p`), as in Google style.
- clang-tidy runs the google, bugprone, cert, cppcoreguidelines,
  clang-analyzer, concurrency, misc, modernize, performance, portability and
  readability families, all as errors.
- Google naming: `CamelCase` types and functions, `lower_case` variables,
  trailing underscore for class members, `kConstants`, `UMBRA_` macros.
- Functions are limited to 40 lines, 30 statements, 8 branches, nesting depth
  3, 5 parameters, 12 variables and cognitive complexity 15.
- Every `NOLINT` names the check and gives a reason, for example
  `// NOLINT(bugprone-narrowing-conversions): <reason>`. The clang-query
  rules (`checks/queries/`: `umbra-bool-parameter`, `umbra-bool-field`,
  `umbra-predicate-name`) honour `NOLINT(umbra-<rule>)` on the line, or
  `NOLINTNEXTLINE(umbra-<rule>)` on the line above, only when it carries a
  reason.
- The namespace matches the directory: `umbra::<module>` in
  `include/umbra/<module>/`, `src/<module>/` and `tests/<module>/`
  (`umbra::ext::<dep>` for bridges), or a namespace nested in it.
- Python (`checks/`, `forge/` and the extensionless scripts in
  `.githooks/`) is enforced like C++: [Pyink](https://github.com/google/pyink)
  formats it (80 columns, 4-space indent, configured in `pyproject.toml`),
  isort orders the imports (`profile = "google"`), and pylint lints it with
  Google's unmodified `pylintrc` (`.pylintrc`). The versions are pinned in
  the devcontainer image.
- Workflows are linted with actionlint, which runs shellcheck on their
  scripts; both are pinned in the devcontainer image.
- CI-only logic stays inline in the workflows. Python there is written as a
  quoted heredoc read by `python3 -` (`python3 - <<'EOF'`), follows the same
  Google style (a `main()` and the `__main__` guard, no module-level logic)
  and is checked like a Python file: `python3 -m forge lint` and the
  pre-commit hook extract it (`python3 -m checks.embedded` prints what they
  find) and fail when pyink or isort would change it or pylint reports
  anything. Format it by hand.
- Every pylint suppression names the message and gives a reason, for example
  `# pylint: disable=invalid-name` after a comment explaining why. The git
  hooks are the only standing exception to Google's naming and import rules:
  git fixes their file names, and they add the repository root to the import
  path before importing `checks`. `forge/__main__.py` also disables
  `invalid-name`, because Python fixes the name of a package's entry point.

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
  Apache-2.0 (including `Apache-2.0 WITH LLVM-exception`), BSD-2-Clause,
  BSD-3-Clause, MIT, Zlib or BSL-1.0. CI checks the license expressions of
  the versions manifest.

## Testing policy

- Tests use GoogleTest through CTest and exercise the **public interface** of
  a module only, never generated headers or internal plumbing.
- Tests are added only to modules the maintainers explicitly request. There
  is no one-test-per-source rule and no coverage gate. The coverage preset
  (`python3 -m forge coverage`: clang source-based coverage, merged with
  llvm-profdata) writes llvm-cov reports of umbra's sources, tests excluded, to
  `build/coverage/report/` (`summary.txt` and `html/index.html`) for local
  use only; CI does not run it.
- A tested source file gets one test file under `tests/<module>/`, mirroring
  the module tree.
- Test bodies follow the same lint rules, except the function-size limit and
  cognitive complexity.
- Infrastructure (hooks, checks, forge, workflows, CMake, Dockerfiles) has no
  tests; it is validated by use.
- Tests run in the debug, release, asan and tsan presets, both locally and in
  CI.
- Tests that need a GPU carry the CTest label `gpu` (`umbra_add_test(...
  LABELS gpu)`). CI runners have no GPU, so the test presets leave them out
  (`ctest -LE gpu`); run them on a GPU host with
  `python3 -m forge check --label gpu` (`ctest -L gpu`). CI still builds
  them, and the GPU module.

## Pull requests

1. Open an issue first for anything beyond a small fix.
2. Branch from `develop` following the git flow naming rules.
3. Keep commits small, themed and signed.
4. Run `python3 -m forge lint` and `python3 -m forge check` inside the
   devcontainer.
5. Open the pull request against `develop` and fill in the template. Its title
   must be a Conventional Commit.

Every pull request must pass the required CI jobs: conventions, format, lint,
build-debug, build-release, sanitize-address, sanitize-thread, modules,
third-party and, on release and hotfix branches, release and release-image.

## Versioning and releases

Umbra follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html),
starting at 0.1.0. The version is declared in the CMake project.

| Change | Before 1.0.0 | From 1.0.0 |
| --- | --- | --- |
| Breaking change | minor | major |
| `feat` | minor | minor |
| `fix` | patch | patch |

The move to 1.0.0 is a deliberate, manual decision by the maintainers
(`git cliff --bump major`); git-cliff never makes it on its own.

git-cliff (configured in `cliff.toml`, installed in the devcontainer) computes
the next version from the Conventional Commits since the last `vX.Y.Z` tag and
drafts the changelog. Only `feat`, `fix`, `perf`, `revert` and breaking
commits reach the changelog, under ⚠ Breaking Changes, Added, Fixed,
Performance and Reverted.

To cut a release:

1. Run `git cliff --bumped-version` to compute the next version, `vX.Y.Z`.
   The first release is 0.1.0.
2. Create `release/X.Y.Z` from `develop`.
3. Set the CMake project version to `X.Y.Z`
   (`project(umbra VERSION X.Y.Z ...)`).
4. Run `git cliff --bump --unreleased --strip all` and paste the dated
   `## [X.Y.Z] - YYYY-MM-DD` section it drafts into
   [CHANGELOG.md](CHANGELOG.md), right below `## [Unreleased]`, in
   [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) format. Edit the
   entries if they need it.
5. Open a pull request into `main`. The CI `release` job checks that the
   branch version equals the CMake project version and git-cliff's bumped
   version, that `vX.Y.Z` is not tagged yet, that the changelog has a dated
   section for it, and that every existing release tag is signed. The CI
   `release-image` job builds the release image, `docker/release.Dockerfile`,
   without publishing it, checks that it runs as UID 65532 with no shell and
   no package manager, and runs `umbra version --build-options`,
   `umbra devices` and `umbra render` in it, without a GPU.
6. Once CI passes, merge locally with `--no-ff`, not with the pull request's
   merge button (the repository allows only squash merges), create the
   signed annotated tag `vX.Y.Z` on `main` (`git tag -s`, or
   `git flow release finish` with signing enabled), merge `main` back into
   `develop`, and push both branches and the tag. GitHub marks the pull
   request as merged when `main` receives its commits.

Hotfixes follow the same steps from `main` with `hotfix/X.Y.Z`.

## Third-party dependencies

- Dependencies are git submodules under `third_party/`, pinned to official
  release tags, never patched in-tree. Patches live in a dedicated directory
  and are applied at build time.
- The versions manifest, `third_party/versions.md`, lists the name, path,
  tag, commit and license of each submodule. Update its row in the same
  commit that moves a submodule. The CI `third-party` job checks that every
  submodule has a row, that the row's commit is the submodule commit and the
  commit its upstream tag points to, and that the license is on the
  allow-list.
- A new dependency arrives in its own `build(deps): add <dep>` pull request.
  When umbra installs its libraries, the table of components in
  `cmake/InstallRuntime.cmake` maps them to the dependency, whose license
  texts the install takes; a library the table does not cover stops the
  install.
- The NVIDIA development stack (CUDA, the OptiX headers) is not a
  submodule: it is prebuilt downloads, each pinned by version and SHA-256 in
  the `nvidia` stage of `docker/deps.Dockerfile`, and listed under
  "Proprietary build-time downloads" in `third_party/versions.md`. Nothing of
  it is compiled into umbra except the CUDA runtime, linked statically into
  `libumbra-gpu.so` as its license allows.
- Updates arrive as weekly pull requests titled `build(deps): bump ...` and
  labelled `dependencies`, one per dependency:
  - GitHub Actions are updated by Dependabot.
  - Submodules are updated by the `submodules` workflow, which moves a
    submodule to its newest upstream release tag (tags shaped like the pinned
    one, so pre-releases are skipped) and updates its manifest row in the same
    commit, on a `feature/bump-<dep>-<tag>` branch. Dependabot is not used
    for submodules because it follows branch heads, not release tags.
  - Pull requests opened by the workflow start no workflow runs. Run the
    deps workflow on the branch: it builds and pins the image, then
    dispatches CI on the branch (see below). The workflow needs the
    repository setting "Allow GitHub Actions to create and approve pull
    requests".
- Dependency images are rebuilt by the deps workflow only when third-party,
  Docker or dependency build inputs change.
- When the `umbra-deps` tag changes, the deps workflow commits the new pin in
  `.devcontainer/devcontainer.json` to the branch (GitHub signs that commit);
  pull it before pushing again. The CI `conventions` job fails while the pin
  is stale.
- A commit made by a workflow starts no workflow run, and CI for a push that
  changes an image input fails until the deps workflow publishes the image.
  So at the end of every run the deps workflow dispatches the ci workflow on
  the branch (`gh workflow run ci.yml --ref <branch>`), after committing the
  pin when it changed. Its jobs, with the same names as on a pull request,
  report on the branch's head, which heads the pull request.
  The dispatched `conventions` job checks the branch's open pull request
  (found with `gh pr list --head <branch>`), and only the image pin when
  there is none. Dispatch CI the same way whenever a branch's head has no CI
  run.
- Runs with a read-only token (Dependabot branches) build and verify the
  images but neither publish nor pin them, and branches pushed by another
  workflow do not trigger the deps workflow at all. Run it manually on such a
  branch: `gh workflow run deps.yml --ref <branch>`, or Actions, deps, Run
  workflow.

## Reporting issues

Use the issue forms for bugs and feature requests. Report security
vulnerabilities and Code of Conduct violations privately, as described in
[SECURITY.md](SECURITY.md).

## License

By contributing, you agree that your contributions are licensed under the
[Apache License, Version 2.0](LICENSE), and that copyright notices read
"Copyright The Umbra Authors".
