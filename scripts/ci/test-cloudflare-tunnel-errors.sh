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


cat > "$TMP_DIR/edge.env" <<'EOF'
CLOUDFLARED_TUNNEL_TOKEN=test-token
CLOUDFLARED_PUBLIC_HOSTNAME=mcp.example.com
PUBLIC_BASE_URL=https://mcp.example.com
CLOUDFLARED_EDGE_IP_VERSION=bogus
EOF
set +e
PATH="$TMP_DIR:/usr/bin:/bin" DEVBOX_ENV_FILE="$TMP_DIR/edge.env" \
  sh "$ROOT_DIR/scripts/install-cloudflare-tunnel.sh" foreground >"$TMP_DIR/edge.out" 2>&1
code=$?
set -e
[ "$code" -eq 2 ]
grep -F "CLOUDFLARED_EDGE_IP_VERSION must be one of: auto, 4, 6" "$TMP_DIR/edge.out" >/dev/null

cat > "$TMP_DIR/systemctl" <<'EOF'
#!/usr/bin/env sh
printf '%s\n' "$*" >> "$DEVBOX_FAKE_SYSTEMCTL_LOG"
exit 0
EOF
chmod +x "$TMP_DIR/systemctl"
: > "$TMP_DIR/systemctl.calls"
DEVBOX_FAKE_SYSTEMCTL_LOG="$TMP_DIR/systemctl.calls" PATH="$TMP_DIR:/usr/bin:/bin" \
  sh "$ROOT_DIR/scripts/restart-cloudflare-tunnel.sh" systemd >"$TMP_DIR/restart.out" 2>&1
grep -F -- "--user status devbox-cloudflared.service" "$TMP_DIR/systemctl.calls" >/dev/null
grep -F -- "--user restart devbox-cloudflared.service" "$TMP_DIR/systemctl.calls" >/dev/null
grep -F "Restarted Linux Cloudflare user service" "$TMP_DIR/restart.out" >/dev/null

printf 'Cloudflare tunnel error guidance checks passed.\n'
