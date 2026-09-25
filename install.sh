#!/bin/sh
# Installs the latest whoport release:
#   curl -fsSL https://raw.githubusercontent.com/vqorn/whoport/main/install.sh | sh
set -eu

REPO="vqorn/whoport"
case "$(uname -s)" in
    Linux)
        case "$(uname -m)" in
            x86_64 | amd64) TARGET=linux-x86_64 ;;
            aarch64 | arm64) TARGET=linux-arm64 ;;
            *) echo "whoport: no prebuilt binary for $(uname -m). Build from source: make && sudo make install" >&2; exit 1 ;;
        esac ;;
    Darwin) TARGET=macos-universal ;;
    *) echo "whoport: $(uname -s) is not supported yet (Linux and macOS only)." >&2; exit 1 ;;
esac

URL="https://github.com/$REPO/releases/latest/download/whoport-$TARGET.tar.gz"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

echo "Downloading $URL"
if command -v curl >/dev/null 2>&1; then
    curl -fsSL "$URL" -o "$TMP/whoport.tar.gz"
else
    wget -qO "$TMP/whoport.tar.gz" "$URL"
fi
tar xzf "$TMP/whoport.tar.gz" -C "$TMP"

DEST=${WHOPORT_INSTALL_DIR:-/usr/local/bin}
if [ -w "$DEST" ]; then
    install -m 755 "$TMP/whoport" "$DEST/whoport"
elif [ -z "${WHOPORT_INSTALL_DIR:-}" ] && command -v sudo >/dev/null 2>&1; then
    echo "Installing to $DEST (needs sudo)"
    sudo install -m 755 "$TMP/whoport" "$DEST/whoport"
else
    DEST="$HOME/.local/bin"
    mkdir -p "$DEST"
    install -m 755 "$TMP/whoport" "$DEST/whoport"
    case ":$PATH:" in
        *":$DEST:"*) ;;
        *) echo "Add $DEST to your PATH to use whoport." ;;
    esac
fi
echo "Installed $("$DEST/whoport" --version) to $DEST"
