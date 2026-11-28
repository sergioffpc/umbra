# umbra

## Agent skills

### Issue tracker

Issues and specs live in GitHub Issues for `sergioffpc/umbra` (via the `gh` CLI). See `docs/agents/issue-tracker.md`.

When taking an issue, assign it to yourself first. After its PR is merged: close the issue, unblock the issues it was blocking, and delete its worktree plus local and remote branches. See **Issue lifecycle** in `docs/agents/issue-tracker.md`.

### Triage labels

Default five-role vocabulary: `needs-triage`, `needs-info`, `ready-for-agent`, `ready-for-human`, `wontfix`. See `docs/agents/triage-labels.md`.

### Domain docs

Single-context: one `GLOSSARY.md` and `docs/adr/` at the repo root. See `docs/agents/domain.md`.

## Math

Imath is the math framework. Never write a math function or type that Imath already has; OpenUSD's Gf stays at the Hydra boundary and is converted to Imath on read. See **Math** in `CONTRIBUTING.md` and `docs/adr/0019-imath-is-the-math-framework.md`.
