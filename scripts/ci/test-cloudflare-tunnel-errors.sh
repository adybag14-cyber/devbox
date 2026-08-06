#!/usr/bin/env sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
TMP_DIR=$(mktemp -d)
trap 'rm -rf "$TMP_DIR"' EXIT INT TERM
: > "$TMP_DIR/empty.env"

cat > "$TMP_DIR/cloudflared" <<'EOF'
#!/usr/bin/env sh
exit 0
EOF
chmod +x "$TMP_DIR/cloudflared"

set +e
CLOUDFLARED_TUNNEL_TOKEN= CLOUDFLARED_PUBLIC_HOSTNAME= PUBLIC_BASE_URL= \
  PATH="$TMP_DIR:/usr/bin:/bin" DEVBOX_ENV_FILE="$TMP_DIR/empty.env" \
  sh "$ROOT_DIR/scripts/install-cloudflare-tunnel.sh" foreground >"$TMP_DIR/token.out" 2>&1
code=$?
set -e
[ "$code" -eq 2 ]
grep -F "CLOUDFLARED_TUNNEL_TOKEN is missing" "$TMP_DIR/token.out" >/dev/null

after_path="/usr/bin:/bin"
set +e
TERMUX_VERSION=ci PREFIX=/data/data/com.termux/files/usr CLOUDFLARED_TUNNEL_TOKEN= \
  PATH="$after_path" DEVBOX_ENV_FILE="$TMP_DIR/empty.env" \
  sh "$ROOT_DIR/scripts/install-cloudflare-tunnel.sh" auto >"$TMP_DIR/termux.out" 2>&1
code=$?
set -e
[ "$code" -eq 2 ]
grep -F "pkg install cloudflared termux-services" "$TMP_DIR/termux.out" >/dev/null

printf 'Cloudflare tunnel error guidance checks passed.\n'
