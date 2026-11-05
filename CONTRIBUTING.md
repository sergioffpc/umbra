# Contributing to Umbra

Thanks for your interest in contributing! This guide explains how work flows
through the repository.

## Reporting bugs and requesting features

Open an issue using one of the [issue templates](https://github.com/sergioffpc/umbra/issues/new/choose).
Search existing issues first to avoid duplicates. For security issues, follow
[SECURITY.md](SECURITY.md) instead of opening a public issue.

## Branching model

Umbra uses [git flow](https://nvie.com/posts/a-successful-git-branching-model/):

| Branch            | Branches from | Merges into         | Purpose                         |
| ----------------- | ------------- | ------------------- | ------------------------------- |
| `main`            |               |                     | Released code, tagged `vX.Y.Z`  |
| `develop`         | `main`        |                     | Integration of upcoming changes |
| `feature/<name>`  | `develop`     | `develop`           | New features and fixes          |
| `release/X.Y.Z`   | `develop`     | `main` and `develop` | Release preparation             |
| `hotfix/X.Y.Z`    | `main`        | `main` and `develop` | Urgent fixes to a release       |

Open pull requests against `develop` unless you are working on a release or
hotfix.

## Commit messages

Commits follow [Conventional Commits 1.0.0](https://www.conventionalcommits.org/en/v1.0.0/):

```
<type>(<optional scope>): <description>

[optional body]

[optional footer(s)]
```

Allowed types: `feat`, `fix`, `docs`, `style`, `refactor`, `perf`, `test`,
`build`, `ci`, `chore`, `revert`. Mark breaking changes with `!` after the type
and a `BREAKING CHANGE:` footer. Pull request titles use the same format.

Examples:

```
feat(integrator): add path tracing integrator
fix(bvh): handle degenerate bounding boxes
```

## Lint and formatting rules

- Formatting: [`rustfmt.toml`](rustfmt.toml) and [`.editorconfig`](.editorconfig).
- Lints: the `[lints]` table in [`Cargo.toml`](Cargo.toml) (rustc lints, clippy
  `all`, `pedantic`, `cargo` and selected `restriction` lints) plus
  [`clippy.toml`](clippy.toml). Every warning is an error in the hook and in CI.
- To silence a lint, use `#[expect(clippy::lint_name, reason = "...")]` on the
  narrowest item possible. `#[allow]` and reasonless attributes are rejected.

Enable the git hooks once per clone:

```sh
git config core.hooksPath .githooks
```

- `pre-commit` checks staged whitespace, `cargo fmt` and `cargo clippy`.
- `commit-msg` rejects messages that are not Conventional Commits (see
  [Commit messages](#commit-messages)): valid type, lowercase scope and
  description, no trailing period, header of at most 72 characters, a blank
  line before the body, and a `BREAKING CHANGE:` footer whenever `!` is used.
  Git-generated merge, revert, `fixup!` and `squash!` messages are allowed.

## Before opening a pull request

Run the same checks CI runs:

```sh
cargo fmt --all -- --check
cargo clippy --all-targets --all-features --locked -- -D warnings
RUSTDOCFLAGS="-D warnings" cargo doc --no-deps --document-private-items --all-features --locked
cargo test --all-features --locked
```

Then:

- Add an entry under `## [Unreleased]` in [CHANGELOG.md](CHANGELOG.md) for
  user-visible changes.
- Add or update tests for the behaviour you changed.
- Keep each pull request focused on one change.

## License

By contributing, you agree that your contributions are licensed under the
[MIT License](LICENSE).
