## Summary

<!-- What does this change and why? Link the issue it closes, e.g. "Closes #12". -->

## Checklist

- [ ] PR title follows [Conventional Commits](https://www.conventionalcommits.org/en/v1.0.0/) (e.g. `feat(bvh): add SAH split`)
- [ ] Targets `develop` (or `main` for a release/hotfix branch)
- [ ] `cargo fmt --all -- --check`, `cargo clippy --all-targets --all-features -- -D warnings` and `cargo test --all-features` pass
- [ ] Tests added or updated
- [ ] `CHANGELOG.md` updated under `[Unreleased]` for user-visible changes
