# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).
Before 1.0.0, breaking changes bump the minor version.

## [Unreleased]

## [0.1.0] - 2026-11-25

### Added

- Adopt the developer certificate of origin (#5)
- **core:** Log diagnostics to stderr and initialize the libraries (#37)
- List render devices, render on them, add version and help (#39)
- Stop with a fatal error on a CPU without x86-64-v3 (#46)

### Fixed

- **devcontainer:** Drop the GPU host requirement that breaks Podman
- **devcontainer:** Fix the host setup for GHCR login, CDI and SSH (#11)
- **deps:** Keep the compiler cache image whole (#23)
- Launch umbra from the build tree's bin in VS Code (#27)
