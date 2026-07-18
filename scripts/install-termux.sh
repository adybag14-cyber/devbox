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
  aarch64|arm64) asset="devbox-setup-android-arm64-v8a" ;;
  armv7l|armv8l|armeabi-v7a) asset="devbox-setup-android-armeabi-v7a" ;;
  x86_64|amd64) asset="devbox-setup-android-x86_64" ;;
  i686|i386|x86) asset="devbox-setup-android-x86" ;;
  *)
    echo "Unsupported Android architecture: $(uname -m)" >&2
    exit 1
    ;;
esac

pkg install -y curl ca-certificates

install_path="${DEVBOX_SETUP_INSTALL_PATH:-${PREFIX}/bin/devbox-setup}"
release_base="${DEVBOX_SETUP_RELEASE_BASE:-${DEVBOX_REPO}/releases/latest/download}"
download_url="${DEVBOX_SETUP_DOWNLOAD_URL:-${release_base}/${asset}}"
checksum_url="${DEVBOX_SETUP_CHECKSUM_URL:-${release_base}/SHA256SUMS}"
temporary="${TMPDIR:-${PREFIX}/tmp}/devbox-setup.$$.tmp"
checksums="${TMPDIR:-${PREFIX}/tmp}/devbox-setup.$$.sha256"
trap 'rm -f "$temporary" "$checksums"' EXIT HUP INT TERM

echo "Canonical Termux app: ${TERMUX_REPO}"
echo "Downloading ${asset} from ${download_url}"
curl --fail --location --retry 3 --output "$temporary" "$download_url"
curl --fail --location --retry 3 --output "$checksums" "$checksum_url"
expected="$(awk -v name="$asset" '$2 == name { print $1; exit }' "$checksums")"
actual="$(sha256sum "$temporary" | awk '{ print $1 }')"
if [ -z "$expected" ] || [ "$actual" != "$expected" ]; then
  echo "Checksum verification failed for ${asset}." >&2
  exit 1
fi

mkdir -p "$(dirname "$install_path")"
chmod 0755 "$temporary"
mv -f "$temporary" "$install_path"
rm -f "$checksums"
trap - EXIT HUP INT TERM

exec "$install_path" "$@"
