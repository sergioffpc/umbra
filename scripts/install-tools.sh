#!/usr/bin/env sh
# Installs the pinned versions of the tools that the git hooks, CI and
# CONTRIBUTING.md use. The one place where their versions live: CI
# (.github/workflows/), the dev container (.devcontainer/) and contributors
# all install them with this script. Bump a version here and nowhere else.
#
# Usage: scripts/install-tools.sh [tool...]   (no tool: all of them)
#
# Cargo tools and the linter binaries go to $CARGO_HOME/bin (~/.cargo/bin by
# default), which rustup puts on PATH; prettier and markdownlint-cli2 are
# global npm packages. Linux on x86-64 or arm64 only (ADR 0023). shellcheck
# comes from the system.
set -eu

BINSTALL_VERSION=1.26.2
NEXTEST_VERSION=0.9.151
LLVM_COV_VERSION=0.9.1
DENY_VERSION=0.20.2
SHEAR_VERSION=1.14.0
TYPOS_VERSION=1.52.0
GIT_CLIFF_VERSION=2.14.2
ACTIONLINT_VERSION=1.7.12
EDITORCONFIG_CHECKER_VERSION=4.0.2
TAPLO_VERSION=0.10.0
PRETTIER_VERSION=3.9.10
MARKDOWNLINT_CLI2_VERSION=0.24.0

ALL_TOOLS="cargo-nextest cargo-llvm-cov cargo-deny cargo-shear typos git-cliff \
actionlint editorconfig-checker taplo prettier markdownlint-cli2"

bin="${CARGO_HOME:-$HOME/.cargo}/bin"

# The machine as each project names it in its release assets: Rust's
# target triple (`rust`) and Go's architecture (`go`).
case "$(uname -s)-$(uname -m)" in
    Linux-x86_64) rust=x86_64 go=amd64 ;;
    Linux-aarch64) rust=aarch64 go=arm64 ;;
    *)
        echo "install-tools: Linux on x86-64 or arm64 only (ADR 0023)" >&2
        exit 1
        ;;
esac

# Downloads an https URL to stdout.
fetch() {
    curl -fsSL --proto '=https' --tlsv1.2 "$1"
}

# Installs a crate's prebuilt binary with cargo-binstall, installing
# cargo-binstall itself first if needed.
binstall() {
    if ! command -v cargo-binstall >/dev/null 2>&1; then
        fetch "https://github.com/cargo-bins/cargo-binstall/releases/download/v${BINSTALL_VERSION}/cargo-binstall-${rust}-unknown-linux-musl.tgz" \
            | tar -xz -C "$bin" cargo-binstall
    fi
    cargo binstall --no-confirm --locked --disable-telemetry --install-path "$bin" "$1"
}

install_tool() {
    case "$1" in
        cargo-nextest) binstall "cargo-nextest@${NEXTEST_VERSION}" ;;
        cargo-llvm-cov) binstall "cargo-llvm-cov@${LLVM_COV_VERSION}" ;;
        cargo-deny) binstall "cargo-deny@${DENY_VERSION}" ;;
        cargo-shear) binstall "cargo-shear@${SHEAR_VERSION}" ;;
        typos) binstall "typos-cli@${TYPOS_VERSION}" ;;
        git-cliff) binstall "git-cliff@${GIT_CLIFF_VERSION}" ;;
        actionlint)
            fetch "https://github.com/rhysd/actionlint/releases/download/v${ACTIONLINT_VERSION}/actionlint_${ACTIONLINT_VERSION}_linux_${go}.tar.gz" \
                | tar -xz -C "$bin" actionlint
            ;;
        editorconfig-checker)
            fetch "https://github.com/editorconfig-checker/editorconfig-checker/releases/download/v${EDITORCONFIG_CHECKER_VERSION}/editorconfig-checker-linux-${go}.tar.gz" \
                | tar -xz -C "$bin" editorconfig-checker
            ;;
        taplo)
            fetch "https://github.com/tamasfe/taplo/releases/download/${TAPLO_VERSION}/taplo-linux-${rust}.gz" \
                | gunzip > "$bin/taplo"
            chmod +x "$bin/taplo"
            ;;
        prettier) npm install --global --no-fund --no-audit "prettier@${PRETTIER_VERSION}" ;;
        markdownlint-cli2)
            npm install --global --no-fund --no-audit "markdownlint-cli2@${MARKDOWNLINT_CLI2_VERSION}"
            ;;
        *)
            echo "install-tools: unknown tool '$1'; known: $ALL_TOOLS" >&2
            exit 1
            ;;
    esac
}

mkdir -p "$bin"
if [ "$#" -eq 0 ]; then
    # Word splitting of the list is intended.
    # shellcheck disable=SC2086
    set -- $ALL_TOOLS
fi
for tool in "$@"; do
    echo "install-tools: $tool"
    install_tool "$tool"
done
