<!--
The PR title must be a Conventional Commit (for example
`feat(core): add wrapping multiply`), because features and bugfixes are
squash-merged into develop with the title as the commit header.
-->

## Summary

<!-- What does this change do, and why? -->

Closes #

## Changes

-

## Testing

<!-- How was this verified inside the devcontainer? -->

## Checklist

- [ ] The branch follows git flow naming (`feature/*`, `bugfix/*`, `release/X.Y.Z`, `hotfix/X.Y.Z`, kebab-case)
- [ ] Every commit is a signed Conventional Commit, signed off under the DCO (`git commit -s`)
- [ ] `make lint` and `make check` pass inside the devcontainer
- [ ] Every new source file carries the Apache-2.0 SPDX header
- [ ] Every unsafe region and NOLINT is justified (`// SAFETY:` comment, named check with a reason)
- [ ] Breaking changes are marked with `!` or a `BREAKING CHANGE:` footer
- [ ] Documentation and module READMEs are updated where needed
