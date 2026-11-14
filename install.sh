#!/bin/sh
# Installs the latest anton release (anton and its plugins) from GitHub.
#
# Usage:
#   curl --proto '=https' --tlsv1.2 -sSf \
#     https://raw.githubusercontent.com/Thinato/anton/main/install.sh | sh
#
# Environment overrides:
#   ANTON_INSTALL_DIR  install location (default: $HOME/.local/bin)
#   ANTON_VERSION      release tag to fetch (default: latest, e.g. v0.1.0-beta)

set -eu

REPO="Thinato/anton"
INSTALL_DIR="${ANTON_INSTALL_DIR:-$HOME/.local/bin}"
VERSION="${ANTON_VERSION:-latest}"

err() {
    printf 'error: %s\n' "$1" >&2
    exit 1
}

for cmd in uname tar mktemp curl; do
    command -v "$cmd" >/dev/null 2>&1 || err "missing required command: $cmd"
done

os="$(uname -s)"
arch="$(uname -m)"
case "$os/$arch" in
    Linux/x86_64 | Linux/amd64) target="linux-x86_64" ;;
    Darwin/arm64 | Darwin/aarch64) target="macos-arm64" ;;
    *) err "no prebuilt binary for $os $arch -- build from source, see https://github.com/$REPO#install" ;;
esac

# Asset names carry no version, so "latest" is GitHub's own redirect and needs
# no API call.
if [ "$VERSION" = "latest" ]; then
    url="https://github.com/$REPO/releases/latest/download/anton-$target.tar.gz"
else
    url="https://github.com/$REPO/releases/download/$VERSION/anton-$target.tar.gz"
fi

echo "downloading $url"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT INT TERM HUP

curl --proto '=https' --tlsv1.2 -fsSL "$url" -o "$tmp/anton.tar.gz" ||
    err "download failed (is $VERSION a published release?)"
tar -C "$tmp" -xzf "$tmp/anton.tar.gz"

mkdir -p "$INSTALL_DIR"
for bin in anton anton-slkx anton-llm anton-temoc; do
    [ -f "$tmp/$bin" ] || continue  # an older ANTON_VERSION without this plugin
    mv "$tmp/$bin" "$INSTALL_DIR/$bin"
    chmod +x "$INSTALL_DIR/$bin"
    echo "installed: $INSTALL_DIR/$bin"
done

if ! "$INSTALL_DIR/anton" --help >/dev/null 2>&1; then
    echo "warning: anton is installed but does not run on this system."
fi

# Runtime dependencies anton only execs, so a missing one shows up late.
if ! command -v gh >/dev/null 2>&1; then
    echo
    echo "note: anton needs the GitHub CLI, logged in: https://cli.github.com, then gh auth login"
fi
if ! command -v slkx >/dev/null 2>&1; then
    echo
    echo "note: the Slack plugin (anton-slkx) needs slkx: https://github.com/Thinato/slkx#install"
fi

case ":${PATH:-}:" in
    *":$INSTALL_DIR:"*) ;;
    *)
        echo
        echo "warning: $INSTALL_DIR is not on your PATH. Add this to your shell rc:"
        echo "  export PATH=\"$INSTALL_DIR:\$PATH\""
        ;;
esac
