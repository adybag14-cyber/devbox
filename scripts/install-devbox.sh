#!/usr/bin/env sh
set -eu

DEVBOX_REPO="https://github.com/adybag14-cyber/devbox"
RELEASE_BASE="${DEVBOX_SETUP_RELEASE_BASE:-${DEVBOX_REPO}/releases/latest/download}"
INSTALL_DIR="${DEVBOX_SETUP_INSTALL_DIR:-${HOME}/.local/bin}"

os=$(uname -s)
arch=$(uname -m)
case "$os" in
  Linux)
    case "$arch" in
      x86_64|amd64) suffix="linux-x86_64" ;;
      aarch64|arm64) suffix="linux-aarch64" ;;
      *) echo "Unsupported Linux architecture: $arch" >&2; exit 1 ;;
    esac
    ;;
  Darwin)
    case "$arch" in
      x86_64|amd64) suffix="macos-x86_64" ;;
      arm64|aarch64) suffix="macos-aarch64" ;;
      *) echo "Unsupported macOS architecture: $arch" >&2; exit 1 ;;
    esac
    ;;
  *)
    echo "This installer supports Linux and macOS. Windows users should download the Windows release bundle; Termux users should run scripts/install-termux.sh." >&2
    exit 1
    ;;
esac

setup_asset="devbox-setup-${suffix}"
tui_asset="devbox-tui-${suffix}"
mkdir -p "$INSTALL_DIR"
setup_path="$INSTALL_DIR/devbox-setup"
tui_path="$INSTALL_DIR/devbox-tui"
tmpdir="${TMPDIR:-/tmp}/devbox-install.$$"
mkdir -p "$tmpdir"
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM

fetch() {
  url=$1
  dest=$2
  if command -v curl >/dev/null 2>&1; then
    curl --fail --location --retry 3 --output "$dest" "$url"
  elif command -v wget >/dev/null 2>&1; then
    wget -O "$dest" "$url"
  else
    echo "curl or wget is required to download Devbox." >&2
    exit 1
  fi
}

sha256_file() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{print $1}'
  elif command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$1" | awk '{print $1}'
  else
    echo "No SHA-256 tool found (sha256sum or shasum)." >&2
    exit 1
  fi
}

checksums="$tmpdir/SHA256SUMS"
fetch "$RELEASE_BASE/SHA256SUMS" "$checksums"

install_asset() {
  asset=$1
  target=$2
  temp="$tmpdir/$asset"
  echo "Downloading $asset"
  fetch "$RELEASE_BASE/$asset" "$temp"
  expected=$(awk -v name="$asset" '$2 == name {print $1; exit}' "$checksums")
  actual=$(sha256_file "$temp")
  if [ -z "$expected" ] || [ "$actual" != "$expected" ]; then
    echo "Checksum verification failed for $asset." >&2
    exit 1
  fi
  chmod 0755 "$temp"
  mv -f "$temp" "$target"
}

install_asset "$setup_asset" "$setup_path"
install_asset "$tui_asset" "$tui_path"

echo "Installed: $setup_path"
echo "Installed: $tui_path"
case ":$PATH:" in
  *":$INSTALL_DIR:"*) ;;
  *) echo "Tip: add $INSTALL_DIR to PATH for future shells." ;;
esac

trap - EXIT HUP INT TERM
rm -rf "$tmpdir"

if [ "$#" -gt 0 ] || [ ! -t 0 ]; then
  exec "$setup_path" "$@"
fi
exec "$tui_path" --bootstrap "$setup_path"
