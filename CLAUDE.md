# umbra

## Git workflow

### Commits

Write every commit message as a Conventional Commit (v1.0.0): `<type>(<scope>)?: <description>`, imperative and lowercase, with `!` and a `BREAKING CHANGE:` footer for breaking changes. Types: `feat`, `fix`, `docs`, `style`, `refactor`, `perf`, `test`, `build`, `ci`, `chore`, `revert`. PR titles follow the same format.

### Branches

Git flow:

- `main` holds released code only; every merge into it is tagged `vX.Y.Z`.
- `develop` is the integration branch and the base for all feature work.
- `feature/<name>` branches from `develop` and merges back into `develop` (PRs target `develop`).
- `release/X.Y.Z` branches from `develop`, merges into both `main` and `develop`.
- `hotfix/X.Y.Z` branches from `main`, merges into both `main` and `develop`.

## Agent skills

### Issue tracker

Issues live in GitHub Issues on `sergioffpc/umbra`, managed via the `gh` CLI. See `docs/agents/issue-tracker.md`.

### Triage labels

Default vocabulary: `needs-triage`, `needs-info`, `ready-for-agent`, `ready-for-human`, `wontfix`. See `docs/agents/triage-labels.md`.

### Domain docs

Single-context: one `GLOSSARY.md` and `docs/adr/` at the repo root. See `docs/agents/domain.md`.
