#!/data/data/com.termux/files/usr/bin/sh
set -eu

DEVBOX_REPO="https://github.com/adybag14-cyber/devbox"
TERMUX_REPO="https://github.com/adybag14-cyber/termux-app"

if [ -z "${PREFIX:-}" ] || [ ! -x "${PREFIX}/bin/pkg" ]; then
  echo "This installer must run inside Termux." >&2
  echo "Canonical Android app: ${TERMUX_REPO}/releases" >&2
  exit 1
fi

api_level="$(getprop ro.build.version.sdk 2>/dev/null || true)"
if [ -n "$api_level" ] && [ "$api_level" -lt 21 ] 2>/dev/null; then
  echo "Android API 21 or newer is required; detected API ${api_level}." >&2
  exit 1
fi

case "$(uname -m)" in
  aarch64|arm64) suffix="android-arm64-v8a" ;;
  armv7l|armv8l|armeabi-v7a) suffix="android-armeabi-v7a" ;;
  x86_64|amd64) suffix="android-x86_64" ;;
  i686|i386|x86) suffix="android-x86" ;;
  *)
    echo "Unsupported Android architecture: $(uname -m)" >&2
    exit 1
    ;;
esac

pkg install -y curl ca-certificates

setup_asset="devbox-setup-${suffix}"
tui_asset="devbox-tui-${suffix}"
runtime_asset="devbox-mcp-${suffix}"
setup_path="${DEVBOX_SETUP_INSTALL_PATH:-${PREFIX}/bin/devbox-setup}"
tui_path="${DEVBOX_TUI_INSTALL_PATH:-${PREFIX}/bin/devbox-tui}"
runtime_path="${DEVBOX_MCP_INSTALL_PATH:-$(dirname "$setup_path")/devbox-mcp}"
release_base="${DEVBOX_SETUP_RELEASE_BASE:-${DEVBOX_REPO}/releases/latest/download}"
tmpdir=$(mktemp -d "${TMPDIR:-${PREFIX}/tmp}/devbox-install.XXXXXXXX")
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM
checksums="$tmpdir/SHA256SUMS"

curl --fail --location --retry 3 --output "$checksums" "$release_base/SHA256SUMS"

verify_asset() {
  asset=$1
  temp="$tmpdir/$asset"
  echo "Downloading ${asset} from ${release_base}/${asset}"
  curl --fail --location --retry 3 --output "$temp" "$release_base/$asset"
  expected="$(awk -v name="$asset" '$2 == name { print $1; exit }' "$checksums")"
  actual="$(sha256sum "$temp" | awk '{ print $1 }')"
  if [ -z "$expected" ] || [ "$actual" != "$expected" ]; then
    echo "Checksum verification failed for ${asset}." >&2
    exit 1
  fi
  chmod 0755 "$temp"
}

echo "Canonical Termux app: ${TERMUX_REPO}"
verify_asset "$setup_asset"
verify_asset "$tui_asset"
verify_asset "$runtime_asset"
mkdir -p "$(dirname "$setup_path")" "$(dirname "$tui_path")" "$(dirname "$runtime_path")"
mv -f "$tmpdir/$runtime_asset" "$runtime_path"
mv -f "$tmpdir/$setup_asset" "$setup_path"
mv -f "$tmpdir/$tui_asset" "$tui_path"

trap - EXIT HUP INT TERM
rm -rf "$tmpdir"

if [ "$#" -gt 0 ] || [ ! -t 0 ]; then
  exec "$setup_path" "$@"
fi
exec "$tui_path" --bootstrap "$setup_path"
